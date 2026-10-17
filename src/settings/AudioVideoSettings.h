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

constexpr bool isValidMsaaSamples(int samples) {
  return samples == -1 || samples == 0 || samples == 2 || samples == 4 ||
         samples == 8 || samples == 16;
}

struct VideoSettings {
  DisplayMode mode = DisplayMode::Windowed;
  int displayIndex = 0;
  int width = 1280;
  int height = 720;
  bool vsync = false;
  std::uint32_t frameCap = 0;
  // -1 preserves the platform policy; 0 disables multisampling.
  int msaaSamples = -1;
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
