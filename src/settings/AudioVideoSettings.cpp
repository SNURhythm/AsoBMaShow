#include "AudioVideoSettings.h"

#include "../targets.h"

#include <algorithm>
#include <cmath>

namespace player_settings {
namespace {
constexpr std::uint32_t kMinSampleRate = 8000;
constexpr std::uint32_t kMaxSampleRate = 384000;
constexpr std::uint32_t kMinBufferFrames = 16;
constexpr std::uint32_t kMaxBufferFrames = 8192;
constexpr std::uint32_t kMinFrameCap = 1;
constexpr std::uint32_t kMaxFrameCap = 1000;

std::uint32_t sanitizeOptionalRange(std::uint32_t value, std::uint32_t minimum,
                                    std::uint32_t maximum) {
  if (value == 0 || (value >= minimum && value <= maximum)) {
    return value;
  }
  return 0;
}

float sanitizeVolume(float value) {
  if (!std::isfinite(value)) {
    return 1.0f;
  }
  return std::clamp(value, 0.0f, 1.0f);
}
} // namespace

void AudioVideoSettings::sanitize() {
  audio.requestedSampleRate = sanitizeOptionalRange(
      audio.requestedSampleRate, kMinSampleRate, kMaxSampleRate);
  audio.requestedBufferFrames = sanitizeOptionalRange(
      audio.requestedBufferFrames, kMinBufferFrames, kMaxBufferFrames);
  audio.masterVolume = sanitizeVolume(audio.masterVolume);
  audio.bgmVolume = sanitizeVolume(audio.bgmVolume);
  audio.keysoundVolume = sanitizeVolume(audio.keysoundVolume);

  switch (video.mode) {
  case DisplayMode::Windowed:
  case DisplayMode::BorderlessFullscreen:
  case DisplayMode::ExclusiveFullscreen:
    break;
  default:
    video.mode = DisplayMode::Windowed;
    break;
  }
  video.displayIndex = std::max(0, video.displayIndex);
  if (video.width <= 0) {
    video.width = 1280;
  }
  if (video.height <= 0) {
    video.height = 720;
  }
  video.frameCap =
      sanitizeOptionalRange(video.frameCap, kMinFrameCap, kMaxFrameCap);
}

AudioVideoSettings defaultAudioVideoSettingsForPlatform() {
  AudioVideoSettings settings;
  if (TARGET_PLATFORM == iOS || TARGET_PLATFORM == Android) {
    settings.video.mode = DisplayMode::ExclusiveFullscreen;
    settings.video.vsync = true;
  }
  return settings;
}
} // namespace player_settings
