#pragma once

#include "PreviewSpectrumAnalyzer.h"

namespace audio {
// Optional post-mix metering. Only the audio callback writes the analyzer;
// enabling/disabling never resets its storage from the control thread.
class AudioOutputMeter {
public:
  void setEnabled(bool enabled) noexcept {
    auto previous = requested_.load(std::memory_order_relaxed);
    while (!requested_.compare_exchange_weak(previous,
        ((previous + 2) & ~std::uint64_t{1}) | (enabled ? 1 : 0),
        std::memory_order_release, std::memory_order_relaxed)) {}
  }

  template <typename Sample>
  void push(const Sample *pcm, std::uint32_t frames, int channels,
            std::uint32_t sampleRate) noexcept {
    const auto requested = requested_.load(std::memory_order_acquire);
    if (!(requested & 1)) return;
    if (producerGeneration_ != requested) {
      spectrum_.reset();
      producerGeneration_ = requested;
    }
    double energy = 0;
    const auto samples = static_cast<std::size_t>(frames) * channels;
    for (std::size_t i = 0; i < samples; ++i) {
      double sample;
      if constexpr (std::is_same_v<Sample, float>)
        sample = static_cast<short>(std::clamp(pcm[i], -1.0F, 1.0F) * 32767.0F) / 32768.0;
      else sample = pcm[i] / 32768.0;
      energy += sample * sample;
    }
    level_.store(samples ? static_cast<float>(std::sqrt(energy / samples)) : 0,
                 std::memory_order_relaxed);
    spectrum_.push(pcm, frames, channels, sampleRate);
    published_.store(requested, std::memory_order_release);
  }

  void pushSilence(std::uint32_t frames, int channels, std::uint32_t sampleRate) noexcept {
    if (!(requested_.load(std::memory_order_acquire) & 1)) return;
    const std::array<short, 512 * 2> silence{};
    while (frames) {
      const auto count = std::min<std::uint32_t>(frames, 512);
      push(silence.data(), count, channels, sampleRate);
      frames -= count;
    }
  }

  [[nodiscard]] float level() const noexcept {
    return current() ? level_.load(std::memory_order_relaxed) : 0;
  }

  // Single UI consumer, like PreviewStreamPlayer::GetOutputSpectrum.
  Spectrum spectrum() { return current() ? spectrum_.read() : Spectrum{}; }

private:
  bool current() const noexcept {
    const auto requested = requested_.load(std::memory_order_acquire);
    return (requested & 1) && published_.load(std::memory_order_acquire) == requested;
  }
  std::atomic<std::uint64_t> requested_{0}, published_{0};
  std::uint64_t producerGeneration_ = 0;
  std::atomic<float> level_{0};
  PreviewSpectrumAnalyzer spectrum_;
};
}
