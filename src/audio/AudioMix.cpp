#include "AudioMix.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace audio {
namespace {

float ClampVolume(float value) {
  if (!std::isfinite(value)) {
    return 1.0f;
  }
  return std::clamp(value, 0.0f, 1.0f);
}

} // namespace

float EffectiveGain(Bus bus, const Volumes &volumes) {
  const float busVolume = bus == Bus::Bgm ? ClampVolume(volumes.bgm)
                                          : ClampVolume(volumes.keysound);
  return ClampVolume(volumes.master) * busVolume;
}

std::vector<short> ResamplePcm(std::span<const short> source, int channels,
                               int sourceRate, int targetRate) {
  if (source.empty() || channels <= 0 || sourceRate <= 0 || targetRate <= 0) {
    return {};
  }
  if (sourceRate == targetRate) {
    return {source.begin(), source.end()};
  }

  const size_t sourceFrames = source.size() / static_cast<size_t>(channels);
  if (sourceFrames == 0) {
    return {};
  }

  const long double targetFrameEstimate =
      static_cast<long double>(sourceFrames) * targetRate / sourceRate;
  if (targetFrameEstimate <= 0.0L ||
      targetFrameEstimate >
          static_cast<long double>(std::numeric_limits<size_t>::max() /
                                   static_cast<size_t>(channels))) {
    return {};
  }
  const size_t targetFrames = static_cast<size_t>(targetFrameEstimate);
  std::vector<short> output(targetFrames * static_cast<size_t>(channels));

  for (size_t targetFrame = 0; targetFrame < targetFrames; ++targetFrame) {
    const long double sourcePosition =
        static_cast<long double>(targetFrame) * sourceRate / targetRate;
    const size_t leftFrame =
        std::min(static_cast<size_t>(sourcePosition), sourceFrames - 1);
    const size_t rightFrame = std::min(leftFrame + 1, sourceFrames - 1);
    const long double fraction = sourcePosition - leftFrame;

    for (int channel = 0; channel < channels; ++channel) {
      const size_t leftIndex =
          leftFrame * static_cast<size_t>(channels) + channel;
      const size_t rightIndex =
          rightFrame * static_cast<size_t>(channels) + channel;
      const long double interpolated =
          static_cast<long double>(source[leftIndex]) * (1.0L - fraction) +
          static_cast<long double>(source[rightIndex]) * fraction;
      const long rounded = std::lround(interpolated);
      output[targetFrame * static_cast<size_t>(channels) + channel] =
          static_cast<short>(std::clamp(
              rounded, static_cast<long>(std::numeric_limits<short>::min()),
              static_cast<long>(std::numeric_limits<short>::max())));
    }
  }

  return output;
}

} // namespace audio
