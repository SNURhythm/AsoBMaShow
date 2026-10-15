#pragma once

#include <algorithm>
#include <array>
#include <cmath>

class PreviewGlow {
public:
  static constexpr std::size_t kWaveformSize = 32;
  float update(float level, float dt) {
    const float target = std::isfinite(level) ? std::sqrt(std::clamp(level * 2, 0.0f, 1.0f)) : 0;
    const float elapsed = std::isfinite(dt) ? std::clamp(dt, 0.0f, 0.25f) : 0;
    const float timeConstant = target > value_ ? 0.035f : 0.22f;
    value_ += (target - value_) * (1 - std::exp(-elapsed / timeConstant));
    // Retain about one second of the actual preview envelope. Different
    // sections of the halo follow different points in this history.
    historyTime_ += elapsed;
    while (historyTime_ >= kHistoryStep) {
      std::move(history_.begin() + 1, history_.end(), history_.begin());
      history_.back() = target;
      historyTime_ -= kHistoryStep;
    }
    const float phase = historyTime_ / kHistoryStep;
    for (std::size_t i = 0; i < waveform_.size(); ++i) {
      const float next = i + 1 < history_.size() ? history_[i + 1] : target;
      waveform_[i] = history_[i] + (next - history_[i]) * phase;
    }
    if (value_ < 0.001f) value_ = 0;
    return value_;
  }
  const std::array<float, kWaveformSize> &waveform() const { return waveform_; }
  void reset() {
    value_ = 0;
    historyTime_ = 0;
    history_.fill(0);
    waveform_.fill(0);
  }
private:
  static constexpr float kHistoryStep = 1.0f / 30;
  float value_ = 0;
  float historyTime_ = 0;
  std::array<float, kWaveformSize> history_{};
  std::array<float, kWaveformSize> waveform_{};
};
