#pragma once

#include "AudioSpectrum.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>

namespace audio {
// One callback producer, one UI consumer. Three buffers transfer ownership;
// neither side can overwrite the other's working buffer. FFT work happens
// exclusively on the UI thread, never in the audio callback.
class PreviewSpectrumAnalyzer {
public:
  static constexpr std::size_t kSize = 4096;
  PreviewSpectrumAnalyzer() {
    constexpr float pi = 3.14159265358979323846f;
    for (std::size_t i = 0; i < kSize; ++i)
      window_[i] = 0.5f - 0.5f * std::cos(2 * pi * i / kSize);
    for (std::size_t i = 0; i < kSize / 2; ++i) {
      const float angle = -2 * pi * i / kSize;
      twiddles_[i] = {std::cos(angle), std::sin(angle)};
    }
  }

  // Producer only. Stop may reset after the backend has quiesced callbacks.
  void reset() noexcept {
    left_.fill(0);
    right_.fill(0);
    cursor_ = pending_ = 0;
    publish(0);
  }

  void push(const short *pcm, std::uint32_t frames, int channels,
            std::uint32_t sampleRate) noexcept {
    for (std::uint32_t i = 0; i < frames; ++i) {
      left_[cursor_] = pcm[static_cast<std::size_t>(i) * channels];
      right_[cursor_] = pcm[static_cast<std::size_t>(i) * channels + (channels > 1 ? 1 : 0)];
      cursor_ = (cursor_ + 1) % kSize;
    }
    pending_ += frames;
    if (pending_ >= 512) {
      pending_ %= 512;
      publish(sampleRate);
    }
  }

  // UI thread only. If several blocks arrived, analyze the newest one; there
  // is no playback queue or scrolling history in the displayed spectrum.
  Spectrum read() {
    if (!(middle_.load(std::memory_order_acquire) & kDirty)) return cached_;
    front_ = middle_.exchange(front_, std::memory_order_acq_rel) & kIndexMask;
    const auto &block = blocks_[front_];
    cached_.fill(0);
    if (!block.sampleRate) return cached_;
    std::array<float, kSize / 2> power{};
    std::array<std::complex<float>, kSize> bins;
    for (const auto *channel : {&block.left, &block.right}) {
      double mean = 0;
      for (const auto sample : *channel) mean += sample;
      mean /= kSize;
      for (std::size_t i = 0; i < kSize; ++i)
        bins[i] = {static_cast<float>(((*channel)[i] - mean) / 32768.0) * window_[i], 0};
      transform(bins);
      for (std::size_t i = 1; i < power.size(); ++i)
        power[i] += std::norm(bins[i]) * 0.5f;
    }
    // Independent channel powers retain opposite-phase stereo energy. Hann
    // coherent gain is 1/2; the positive-frequency sine amplitude is 4/N.
    for (std::size_t band = 0; band < cached_.size(); ++band) {
      const float low = 32 * std::pow(500.0f, float(band) / cached_.size());
      const float high = 32 * std::pow(500.0f, float(band + 1) / cached_.size());
      const auto first = std::max<std::size_t>(1, std::floor(low * kSize / block.sampleRate));
      const auto last = std::min<std::size_t>(kSize / 2 - 1, std::ceil(high * kSize / block.sampleRate));
      float peak = 0;
      for (auto bin = first; bin <= last; ++bin) peak = std::max(peak, power[bin]);
      const float amplitude = std::sqrt(peak) * (4.0f / kSize);
      if (amplitude > 0)
        cached_[band] = std::clamp((20 * std::log10(amplitude) + 66) / 60, 0.0f, 1.0f);
    }
    return cached_;
  }

private:
  struct Block {
    std::array<short, kSize> left{}, right{};
    std::uint32_t sampleRate = 0;
  };
  static constexpr unsigned kDirty = 4, kIndexMask = 3;
  static_assert(std::atomic<unsigned>::is_always_lock_free);
  std::array<Block, 3> blocks_;
  std::atomic<unsigned> middle_{2};
  unsigned back_ = 0; // Producer-owned.
  unsigned front_ = 1; // Consumer-owned.
  std::array<short, kSize> left_{}, right_{};
  std::size_t cursor_ = 0, pending_ = 0;
  std::array<float, kSize> window_{};
  std::array<std::complex<float>, kSize / 2> twiddles_{};
  Spectrum cached_{};

  void publish(std::uint32_t sampleRate) noexcept {
    auto &block = blocks_[back_];
    std::copy(left_.begin() + cursor_, left_.end(), block.left.begin());
    std::copy_n(left_.begin(), cursor_, block.left.begin() + kSize - cursor_);
    std::copy(right_.begin() + cursor_, right_.end(), block.right.begin());
    std::copy_n(right_.begin(), cursor_, block.right.begin() + kSize - cursor_);
    block.sampleRate = sampleRate;
    back_ = middle_.exchange(back_ | kDirty, std::memory_order_acq_rel) & kIndexMask;
  }

  void transform(std::array<std::complex<float>, kSize> &bins) const {
    for (std::size_t i = 1, reversed = 0; i < kSize; ++i) {
      auto bit = kSize / 2;
      while (reversed & bit) { reversed ^= bit; bit >>= 1; }
      reversed ^= bit;
      if (i < reversed) std::swap(bins[i], bins[reversed]);
    }
    for (std::size_t length = 2; length <= kSize; length *= 2) {
      const auto half = length / 2;
      for (std::size_t first = 0; first < kSize; first += length) {
        for (std::size_t i = 0; i < half; ++i) {
          const auto even = bins[first + i];
          const auto odd = bins[first + i + half] * twiddles_[i * (kSize / length)];
          bins[first + i] = even + odd;
          bins[first + i + half] = even - odd;
        }
      }
    }
  }
};
}
