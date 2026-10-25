#pragma once

#include "../bms_parser.hpp"
#include "DecodedAssetCache.h"
#include <cmath>

namespace audio::bmson {
struct FrameRange { std::size_t start = 0, count = 0; };
inline FrameRange sourceRange(long long startMicros, long long durationMicros,
                              int rate, std::size_t frames) {
  const auto frameAt = [&](long double time) {
    return static_cast<std::size_t>(std::clamp(std::floor(time * rate / 1000000.0L),
                                               0.0L, static_cast<long double>(frames)));
  };
  const auto first = frameAt(std::max(0LL, startMicros));
  const auto end = durationMicros < 0 ? frames : frameAt(
      static_cast<long double>(std::max(0LL, startMicros)) + durationMicros);
  return {first, end > first ? end - first : 0};
}
using Decoded = DecodedAssetCache::DecodedSound;
// Loader-only: each independently playable slice shares immutable source PCM.
inline std::shared_ptr<const Decoded> prepare(
    const bms_parser::WavSlice &slice, const std::shared_ptr<const Decoded> &source,
    const std::atomic_bool &cancelled) {
  if (cancelled || !source || source->info.channels <= 0 || source->info.samplerate <= 0)
    return {};
  const auto range = sourceRange(slice.StartMicros, slice.DurationMicros,
      source->info.samplerate, source->pcm.size() / source->info.channels);
  if (range.count == 0) return {};
  auto result = std::make_shared<Decoded>();
  result->info = source->info;
  result->info.frames = range.count;
  result->pcm = source->pcm.subspan(range.start * source->info.channels,
                                   range.count * source->info.channels);
  return result;
}
} // namespace audio::bmson
