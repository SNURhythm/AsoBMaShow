#pragma once

#include "RealtimeGameplayWorker.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace gameplay {

inline constexpr std::size_t kRealtimeTouchFingerCapacity = 32;

struct RealtimeTouchPoint {
  float x = 0.0F;
  float y = 0.0F;
};

// Ordered hit region authored by the gameplay skin. Regions are tested in
// vector order, so an earlier region owns a shared edge or overlap.
struct RealtimeTouchLaneRegion {
  RealtimeTouchPoint bottomLeft;
  RealtimeTouchPoint bottomRight;
  RealtimeTouchPoint topLeft;
  RealtimeTouchPoint topRight;
  int lane = -1;
  bool scratch = false;
};

struct RealtimeTouchLayout {
  RealtimeTouchPoint bottomLeft;
  RealtimeTouchPoint bottomRight;
  RealtimeTouchPoint topLeft;
  RealtimeTouchPoint topRight;
  std::vector<int> lanes;
  std::vector<bool> scratch;
  std::vector<RealtimeTouchLaneRegion> laneRegions;
  std::size_t laneCount = 0;
  int keyMode = 7;
  bool dragMode = false;
};

enum class RealtimeTouchPhase : std::uint8_t {
  Down,
  Move,
  Up,
  Cancel,
  CancelExpired
};

struct RealtimeTouchSample {
  std::int64_t fingerId = 0;
  RealtimeTouchPhase phase = RealtimeTouchPhase::Move;
  float normalizedX = 0.0F;
  float normalizedY = 0.0F;
  std::int64_t steadyTimestampMicros = 0;
  bool excludedFromGameplay = false;

  bool operator==(const RealtimeTouchSample &) const = default;
};

struct RealtimeTouchInputSink {
  void *context = nullptr;
  bool (*emit)(void *, const RealtimeGameplayInput &) = nullptr;
  bool (*scratchLongNoteHeld)(void *, int lane) = nullptr;
  bool (*cancelTouchLifecycle)(void *, const RealtimeTouchSample &) = nullptr;
};

class RealtimeTouchInputRouter {
public:
  RealtimeTouchInputRouter(std::uint64_t epoch, RealtimeTouchLayout layout,
                           RealtimeTouchInputSink sink) noexcept;

  bool consume(const RealtimeTouchSample &sample) noexcept;
  bool cancelAll(std::int64_t steadyTimestampMicros) noexcept;
  bool updateLayout(RealtimeTouchLayout layout,
                    std::int64_t steadyTimestampMicros) noexcept;
  bool setGameplayEnabled(bool enabled,
                          std::int64_t steadyTimestampMicros) noexcept;
  void reset() noexcept;

private:
  struct FingerState {
    std::int64_t fingerId = 0;
    int lane = -1;
    bool active = false;
    bool excluded = false;
    bool pressed = false;
    bool scratch = false;
    int scratchDirection = 0;
    std::optional<replay::LogicalControl> replayControl;
    float lastX = 0.0F;
    float lastY = 0.0F;
    std::int64_t cancelDeadlineMicros = 0;
  };

  [[nodiscard]] std::optional<std::size_t>
  laneIndexAt(float x, float y, bool requireInside) const noexcept;
  [[nodiscard]] static bool
  normalizeLayout(RealtimeTouchLayout &layout) noexcept;
  [[nodiscard]] FingerState *findFinger(std::int64_t fingerId) noexcept;
  [[nodiscard]] FingerState *allocateFinger(std::int64_t fingerId) noexcept;
  [[nodiscard]] bool laneOccupied(int lane,
                                  std::int64_t exceptFinger) const noexcept;
  bool emit(RealtimeGameplayInputType type, int lane,
            std::optional<replay::LogicalControl> replayControl,
            std::int64_t timestampMicros, bool backSpin = false) noexcept;
  bool beginLane(FingerState &finger, std::size_t laneIndex,
                 const RealtimeTouchSample &sample) noexcept;
  bool releaseLane(FingerState &finger, std::int64_t timestampMicros,
                   bool backSpin = false) noexcept;
  bool handleScratchMove(FingerState &finger,
                         const RealtimeTouchSample &sample) noexcept;

  std::uint64_t epoch_ = 0;
  RealtimeTouchLayout layout_;
  RealtimeTouchInputSink sink_;
  bool legacyUniformLayout_ = false;
  bool gameplayEnabled_ = true;
  std::array<FingerState, kRealtimeTouchFingerCapacity> fingers_{};
};

} // namespace gameplay
