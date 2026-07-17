#pragma once

#include "GameplaySimulation.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <semaphore>
#include <thread>
#include <type_traits>
#include <vector>

namespace gameplay {

inline constexpr std::size_t kRealtimeGameplayIngressSize = 4096;

enum class RealtimeGameplayInputType : std::uint8_t { Press, Release };

struct RealtimeGameplayInput {
  std::uint64_t epoch = 0;
  RealtimeGameplayInputType type = RealtimeGameplayInputType::Press;
  int lane = -1;
  int compensateLane = -1;
  bool backSpin = false;
  std::int64_t steadyTimestampMicros = 0;
};

struct RealtimeGameplayAudioReservation {
  std::uintptr_t value = 0;
};

struct RealtimeGameplayClock {
  void *context = nullptr;
  std::optional<std::int64_t> (*mapSteadyToSong)(void *, std::int64_t) =
      nullptr;
  std::optional<std::int64_t> (*currentSongTime)(void *) = nullptr;
};

struct RealtimeGameplayAudioSink {
  void *context = nullptr;
  bool (*reserve)(void *, NoteId, RealtimeGameplayAudioReservation &) =
      nullptr;
  bool (*commit)(void *, RealtimeGameplayAudioReservation, NoteId) = nullptr;
};

enum class RealtimeGameplayFault : std::uint8_t {
  None,
  IngressOverflow,
  ClockUnavailable,
  AudioCapacityUnavailable,
  AudioCommitFailed,
  InternalConsistency,
};

struct RealtimeGameplayWorkerConfig {
  std::uint64_t epoch = 0;
  GameplaySimulationConfig simulation;
  RealtimeGameplayClock clock;
  RealtimeGameplayAudioSink audio;
  bool inputTriggeredKeysounds = true;
};

struct RealtimeGameplaySnapshot {
  std::uint64_t generation = 0;
  std::uint64_t transactionSequence = 0;
  std::vector<NoteRuntimeState> noteStates;
  std::array<bool, 16> lanePressed{};
  GameplayAttemptSnapshot attempt;
  GameplayInputResult latestTransaction;
  std::size_t replayEventCount = 0;
  GameplayTerminalReason terminalReason = GameplayTerminalReason::None;
};

template <typename T, std::size_t Capacity> class BoundedMpscQueue {
  static_assert(Capacity > 0);
  static_assert(std::is_trivially_copyable_v<T>);

public:
  BoundedMpscQueue() noexcept {
    for (std::size_t index = 0; index < Capacity; ++index) {
      slots_[index].sequence.store(index, std::memory_order_relaxed);
    }
  }

  bool tryPush(const T &value) noexcept {
    std::size_t position = enqueuePosition_.load(std::memory_order_relaxed);
    for (;;) {
      Slot &slot = slots_[position % Capacity];
      const std::size_t sequence =
          slot.sequence.load(std::memory_order_acquire);
      const auto difference = static_cast<std::intptr_t>(sequence) -
                              static_cast<std::intptr_t>(position);
      if (difference == 0) {
        if (enqueuePosition_.compare_exchange_weak(
                position, position + 1, std::memory_order_relaxed,
                std::memory_order_relaxed)) {
          slot.value = value;
          slot.sequence.store(position + 1, std::memory_order_release);
          return true;
        }
      } else if (difference < 0) {
        return false;
      } else {
        position = enqueuePosition_.load(std::memory_order_relaxed);
      }
    }
  }

  bool tryPop(T &result) noexcept {
    Slot &slot = slots_[dequeuePosition_ % Capacity];
    const std::size_t sequence =
        slot.sequence.load(std::memory_order_acquire);
    const auto difference = static_cast<std::intptr_t>(sequence) -
                            static_cast<std::intptr_t>(dequeuePosition_ + 1);
    if (difference != 0) {
      return false;
    }
    result = slot.value;
    slot.sequence.store(dequeuePosition_ + Capacity,
                        std::memory_order_release);
    ++dequeuePosition_;
    return true;
  }

private:
  struct Slot {
    std::atomic<std::size_t> sequence{0};
    T value{};
  };

  std::array<Slot, Capacity> slots_{};
  alignas(64) std::atomic<std::size_t> enqueuePosition_{0};
  alignas(64) std::size_t dequeuePosition_ = 0;
};

class RealtimeGameplayWorker {
public:
  class SnapshotLease {
  public:
    SnapshotLease() = default;
    SnapshotLease(const SnapshotLease &) = delete;
    SnapshotLease &operator=(const SnapshotLease &) = delete;
    SnapshotLease(SnapshotLease &&other) noexcept;
    SnapshotLease &operator=(SnapshotLease &&other) noexcept;
    ~SnapshotLease();

    [[nodiscard]] explicit operator bool() const noexcept {
      return snapshot_ != nullptr;
    }
    [[nodiscard]] const RealtimeGameplaySnapshot *operator->() const noexcept {
      return snapshot_;
    }
    [[nodiscard]] const RealtimeGameplaySnapshot &operator*() const noexcept {
      return *snapshot_;
    }

  private:
    friend class RealtimeGameplayWorker;
    SnapshotLease(const RealtimeGameplayWorker *owner, std::size_t index,
                  const RealtimeGameplaySnapshot *snapshot) noexcept;
    void release() noexcept;

    const RealtimeGameplayWorker *owner_ = nullptr;
    std::size_t index_ = 0;
    const RealtimeGameplaySnapshot *snapshot_ = nullptr;
  };

  RealtimeGameplayWorker(GameplayDefinition definition,
                         RealtimeGameplayWorkerConfig config);
  ~RealtimeGameplayWorker();
  RealtimeGameplayWorker(const RealtimeGameplayWorker &) = delete;
  RealtimeGameplayWorker &operator=(const RealtimeGameplayWorker &) = delete;

  bool start();
  void stop();
  bool enqueueInput(const RealtimeGameplayInput &input) noexcept;
  [[nodiscard]] SnapshotLease acquireLatestSnapshot() const noexcept;
  [[nodiscard]] RealtimeGameplayFault fault() const noexcept;
  [[nodiscard]] bool running() const noexcept;

private:
  struct SnapshotBuffer {
    RealtimeGameplaySnapshot snapshot;
    mutable std::atomic<std::uint32_t> readers{0};
  };

  void run();
  void signal() noexcept;
  void processInput(const RealtimeGameplayInput &input);
  bool advanceAutomatic();
  void publishSnapshot();
  void latchFault(RealtimeGameplayFault fault) noexcept;
  void releaseSnapshot(std::size_t index) const noexcept;

  GameplayDefinition definition_;
  RealtimeGameplayWorkerConfig config_;
  GameplaySimulation simulation_;
  BoundedMpscQueue<RealtimeGameplayInput, kRealtimeGameplayIngressSize>
      ingress_;
  std::array<SnapshotBuffer, 3> snapshots_{};
  mutable std::atomic<std::size_t> latestSnapshot_{0};
  std::uint64_t snapshotGeneration_ = 0;
  std::uint64_t transactionSequence_ = 0;
  GameplayInputResult latestTransaction_;
  std::binary_semaphore wake_{0};
  std::atomic_bool wakePending_{false};
  std::atomic_bool started_{false};
  std::atomic_bool stopRequested_{false};
  std::atomic<RealtimeGameplayFault> fault_{RealtimeGameplayFault::None};
  std::thread thread_;
};

} // namespace gameplay
