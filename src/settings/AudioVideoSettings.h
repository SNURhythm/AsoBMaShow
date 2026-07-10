#pragma once

#include <cstdint>
#include <string>

namespace player_settings {
enum class DisplayMode {
  Windowed,
  BorderlessFullscreen,
  ExclusiveFullscreen,
};

struct AudioSettings {
  std::string outputDeviceId;
  std::uint32_t requestedSampleRate = 0;
  std::uint32_t requestedBufferFrames = 0;
  float masterVolume = 1.0f;
  float bgmVolume = 1.0f;
  float keysoundVolume = 1.0f;
  bool operator==(const AudioSettings &) const = default;
};

struct VideoSettings {
  DisplayMode mode = DisplayMode::Windowed;
  int displayIndex = 0;
  int width = 1280;
  int height = 720;
  bool vsync = false;
  std::uint32_t frameCap = 0;
  bool operator==(const VideoSettings &) const = default;
};

struct AudioVideoSettings {
  AudioSettings audio;
  VideoSettings video;

  void sanitize();
  bool operator==(const AudioVideoSettings &) const = default;
};

AudioVideoSettings defaultAudioVideoSettingsForPlatform();
} // namespace player_settings
