#pragma once
#include "Judgement.h"
#include <atomic>
#include <bit>
#include <cstdint>
#include <utility>

// Shared fixed-size lane presentation state for beams and keybombs.
struct LaneKeybombEvent {
  Judgement judgement = None;
  long long timeMicros = 0;
  bool operator==(const LaneKeybombEvent &) const = default;
};

struct LaneState {
  long long lastStateTime = -1;
  long long lastPressedTime = -1;
  bool isPressed = false;
  JudgeResult lastPressedJudge = JudgeResult(None, 0);
  LaneKeybombEvent keybomb;
};

struct AtomicLaneState {
  std::atomic<long long> lastStateTime{-1};
  std::atomic<long long> lastPressedTime{-1};
  std::atomic<bool> isPressed{false};
  std::atomic<int> lastPressedJudgement{None};
  std::atomic<long long> lastPressedDiff{0};

  // One packed publication prevents a new timestamp being paired with an old
  // judgement. No lock, retry, queue growth, or renderer work on the producer.
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
  std::atomic<std::uint64_t> keybombWord{None};
  void publishKeybomb(Judgement judge, long long time) noexcept {
    if (judge < PGreat || judge > Poor) return;
    keybombWord.store((static_cast<std::uint64_t>(time) << 3) |
                         static_cast<std::uint64_t>(judge), std::memory_order_relaxed);
  }
  LaneKeybombEvent keybomb() const noexcept {
    const auto word = keybombWord.load(std::memory_order_relaxed);
    return {static_cast<Judgement>(word & 7), std::bit_cast<std::int64_t>(word) >> 3};
  }
  void clearKeybomb() noexcept { keybombWord.store(None, std::memory_order_relaxed); }

  AtomicLaneState() = default;
  AtomicLaneState(const AtomicLaneState &) = delete;
  AtomicLaneState &operator=(const AtomicLaneState &) = delete;
  AtomicLaneState(AtomicLaneState &&other) noexcept;
  AtomicLaneState &operator=(AtomicLaneState &&other) noexcept;
};

inline AtomicLaneState::AtomicLaneState(AtomicLaneState &&other) noexcept {
  *this = std::move(other);
}

inline AtomicLaneState &
AtomicLaneState::operator=(AtomicLaneState &&other) noexcept {
  lastStateTime.store(other.lastStateTime.load(std::memory_order_relaxed),
                      std::memory_order_relaxed);
  lastPressedTime.store(other.lastPressedTime.load(std::memory_order_relaxed),
                        std::memory_order_relaxed);
  isPressed.store(other.isPressed.load(std::memory_order_relaxed),
                  std::memory_order_relaxed);
  lastPressedJudgement.store(
      other.lastPressedJudgement.load(std::memory_order_relaxed),
      std::memory_order_relaxed);
  lastPressedDiff.store(other.lastPressedDiff.load(std::memory_order_relaxed),
                        std::memory_order_relaxed);
  keybombWord.store(other.keybombWord.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
  return *this;
}
