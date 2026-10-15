#pragma once

#include "../audio/AudioSpectrum.h"
#include <algorithm>
#include <cmath>

class PreviewGlow {
public:
  float update(float level, const audio::Spectrum &spectrum, float dt) {
    const float target = std::isfinite(level) ? std::sqrt(std::clamp(level * 2, 0.0f, 1.0f)) : 0;
    const float elapsed = std::isfinite(dt) ? std::clamp(dt, 0.0f, 0.25f) : 0;
    value_ += (target - value_) * (1 - std::exp(-elapsed / (target > value_ ? 0.012f : 0.10f)));
    for (std::size_t i = 0; i < bands_.size(); ++i) {
      const float band = target > 0 && std::isfinite(spectrum[i]) ? std::clamp(spectrum[i], 0.0f, 1.0f) : 0;
      // Frequency positions never move. Only a short attack/release filter
      // suppresses block-boundary flicker in the current FFT magnitudes.
      bands_[i] += (band - bands_[i]) *
          (1 - std::exp(-elapsed / (band > bands_[i] ? 0.006f : 0.10f)));
      if (bands_[i] < 0.001f) bands_[i] = 0;
    }
    if (value_ < 0.001f) value_ = 0;
    return value_;
  }
  const audio::Spectrum &spectrum() const { return bands_; }
  float bass() const { return *std::max_element(bands_.begin(), bands_.begin() + 4); }
  void reset() {
    value_ = 0;
    bands_.fill(0);
  }
private:
  float value_ = 0;
  audio::Spectrum bands_{};
};
