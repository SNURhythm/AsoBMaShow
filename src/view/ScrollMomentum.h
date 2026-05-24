#pragma once

#include <cmath>

class ScrollMomentum {
public:
  void stop() {
    velocity = 0.0f;
    active = false;
  }

  void recordDragDelta(float delta) {
    active = false;
    const float sampledVelocity = delta * kReleaseVelocityScale;
    velocity =
        velocity * kDragSmoothing + sampledVelocity * (1.0f - kDragSmoothing);
  }

  void release() {
    if (std::fabs(velocity) < kMinimumReleaseVelocity) {
      stop();
      return;
    }
    active = true;
  }

  bool step(float &delta) {
    if (!active) {
      return false;
    }

    delta = velocity;
    velocity *= kFriction;
    if (std::fabs(velocity) < kStopVelocity) {
      stop();
    }
    return true;
  }

private:
  static constexpr float kDragSmoothing = 0.25f;
  static constexpr float kReleaseVelocityScale = 1.2f;
  static constexpr float kFriction = 0.95f;
  static constexpr float kMinimumReleaseVelocity = 2.0f;
  static constexpr float kStopVelocity = 0.05f;

  float velocity = 0.0f;
  bool active = false;
};
