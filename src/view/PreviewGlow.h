#pragma once

#include <algorithm>
#include <cmath>

class PreviewGlow {
public:
  float update(float level, float dt) {
    const float target = std::isfinite(level) ? std::sqrt(std::clamp(level * 2, 0.0f, 1.0f)) : 0;
    const float elapsed = std::isfinite(dt) ? std::clamp(dt, 0.0f, 0.25f) : 0;
    const float timeConstant = target > value_ ? 0.035f : 0.22f;
    value_ += (target - value_) * (1 - std::exp(-elapsed / timeConstant));
    if (value_ < 0.001f) value_ = 0;
    return value_;
  }
  void reset() { value_ = 0; }
private:
  float value_ = 0;
};
