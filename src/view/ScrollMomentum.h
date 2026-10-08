#pragma once

#include <cmath>
#include <cstdint>

class ScrollMomentum {
public:
  void stop() {
    velocity = 0.0f;
    pendingDragDelta = 0.0f;
    lastSampleWeight = 0.0f;
    active = false;
  }

  void beginDrag(std::uint32_t timestampMs) {
    stop();
    lastDragTimestampMs = timestampMs;
  }

  void recordDragDelta(float delta, std::uint32_t timestampMs) {
    active = false;
    pendingDragDelta += delta;
    // SDL event timestamps share SDL_GetTicks' clock. Unsigned subtraction
    // also handles its 32-bit wraparound; batch events can share a millisecond.
    const auto elapsedMs = timestampMs - lastDragTimestampMs;
    if (elapsedMs == 0) {
      // Amend the current sample when the final batched motion shares its time.
      if (lastSampleWeight > 0.0f) {
        velocity += pendingDragDelta * lastSampleWeight;
        pendingDragDelta = 0.0f;
      }
      return;
    }
    const float elapsedSeconds = elapsedMs / 1000.0f;
    const float sampledVelocity = pendingDragDelta / elapsedSeconds;
    const float smoothing = std::exp(-elapsedSeconds / kDragSmoothingSeconds);
    lastSampleWeight = (1.0f - smoothing) / elapsedSeconds;
    velocity = velocity * smoothing + sampledVelocity * (1.0f - smoothing);
    pendingDragDelta = 0.0f;
    lastDragTimestampMs = timestampMs;
  }

  void release(std::uint32_t timestampMs) {
    if (pendingDragDelta != 0.0f) recordDragDelta(0.0f, timestampMs);
    if (timestampMs - lastDragTimestampMs > kMaximumReleaseDelayMs ||
        std::fabs(velocity) < kMinimumReleaseVelocity) {
      stop();
      return;
    }
    lastStepTimestampMs = timestampMs;
    active = true;
  }

  bool step(std::uint32_t timestampMs, float &delta) {
    delta = 0.0f;
    if (!active) {
      return false;
    }

    const auto elapsedMs = timestampMs - lastStepTimestampMs;
    if (elapsedMs == 0) return false;
    lastStepTimestampMs = timestampMs;
    const float elapsedSeconds = elapsedMs / 1000.0f;
    // Integrate exponential decay over the frame, instead of treating an
    // event's drag distance as a distance to repeat every rendered frame.
    const float decayMinusOne = std::expm1(-kDecayRate * elapsedSeconds);
    delta = velocity * -decayMinusOne / kDecayRate;
    velocity *= 1.0f + decayMinusOne;
    if (std::fabs(velocity) < kStopVelocity) {
      stop();
    }
    return true;
  }

private:
  static constexpr float kDragSmoothingSeconds = 0.012f;
  static constexpr float kDecayRate = 3.0776f; // 0.95 friction per frame at 60 Hz.
  static constexpr float kMinimumReleaseVelocity = 120.0f; // UI units/second.
  static constexpr float kStopVelocity = 3.0f;
  static constexpr std::uint32_t kMaximumReleaseDelayMs = 100;

  float velocity = 0.0f;
  float pendingDragDelta = 0.0f;
  float lastSampleWeight = 0.0f;
  std::uint32_t lastDragTimestampMs = 0;
  std::uint32_t lastStepTimestampMs = 0;
  bool active = false;
};
