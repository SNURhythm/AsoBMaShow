#include "../src/AppSettings.h"
#include "../src/settings/AudioVideoSettings.h"
#include "../src/targets.h"

#include <iostream>
#include <limits>

#define ASSERT_TRUE(value, label)                                              \
  do {                                                                         \
    if (!(value)) {                                                            \
      std::cerr << label << " expected true" << std::endl;                     \
      return false;                                                            \
    }                                                                          \
  } while (false)

namespace {
using player_settings::AudioVideoSettings;
using player_settings::DisplayMode;

bool defaultsMatchLegacyBehavior() {
  const AudioVideoSettings value;

  ASSERT_TRUE(value.audio.outputDeviceId.empty(), "default output device");
  ASSERT_TRUE(value.audio.requestedSampleRate == 0,
              "default requested sample rate");
  ASSERT_TRUE(value.audio.requestedBufferFrames == 0,
              "default requested buffer frames");
  ASSERT_TRUE(value.audio.masterVolume == 1.0f, "default master volume");
  ASSERT_TRUE(value.audio.bgmVolume == 1.0f, "default BGM volume");
  ASSERT_TRUE(value.audio.keysoundVolume == 1.0f, "default keysound volume");
  ASSERT_TRUE(value.video.mode == DisplayMode::Windowed,
              "default display mode");
  ASSERT_TRUE(value.video.displayIndex == 0, "default display index");
  ASSERT_TRUE(value.video.width == 1280, "default width");
  ASSERT_TRUE(value.video.height == 720, "default height");
  ASSERT_TRUE(!value.video.vsync, "default VSync");
  ASSERT_TRUE(value.video.frameCap == 0, "default frame cap");

  return true;
}

bool sanitizeRejectsOutOfRangeValues() {
  AudioVideoSettings below;
  below.audio.outputDeviceId = "missing:device";
  below.audio.requestedSampleRate = 7999;
  below.audio.requestedBufferFrames = 15;
  below.audio.masterVolume = -0.01f;
  below.audio.bgmVolume = -1.0f;
  below.audio.keysoundVolume = -10.0f;
  below.video.mode = static_cast<DisplayMode>(99);
  below.video.displayIndex = -1;
  below.video.width = 0;
  below.video.height = -1;
  below.video.frameCap = 1;
  below.sanitize();

  ASSERT_TRUE(below.audio.outputDeviceId == "missing:device",
              "unavailable output device intent");
  ASSERT_TRUE(below.audio.requestedSampleRate == 0,
              "sample rate below minimum");
  ASSERT_TRUE(below.audio.requestedBufferFrames == 0,
              "buffer frames below minimum");
  ASSERT_TRUE(below.audio.masterVolume == 0.0f, "master volume below minimum");
  ASSERT_TRUE(below.audio.bgmVolume == 0.0f, "BGM volume below minimum");
  ASSERT_TRUE(below.audio.keysoundVolume == 0.0f,
              "keysound volume below minimum");
  ASSERT_TRUE(below.video.mode == DisplayMode::Windowed,
              "invalid display mode");
  ASSERT_TRUE(below.video.displayIndex == 0, "negative display index");
  ASSERT_TRUE(below.video.width == 1280, "invalid width");
  ASSERT_TRUE(below.video.height == 720, "invalid height");
  ASSERT_TRUE(below.video.frameCap == 1, "minimum frame cap");

  AudioVideoSettings above;
  above.audio.requestedSampleRate = 384001;
  above.audio.requestedBufferFrames = 8193;
  above.audio.masterVolume = 1.01f;
  above.audio.bgmVolume = 2.0f;
  above.audio.keysoundVolume = 10.0f;
  above.video.frameCap = 1001;
  above.sanitize();

  ASSERT_TRUE(above.audio.requestedSampleRate == 0,
              "sample rate above maximum");
  ASSERT_TRUE(above.audio.requestedBufferFrames == 0,
              "buffer frames above maximum");
  ASSERT_TRUE(above.audio.masterVolume == 1.0f, "master volume above maximum");
  ASSERT_TRUE(above.audio.bgmVolume == 1.0f, "BGM volume above maximum");
  ASSERT_TRUE(above.audio.keysoundVolume == 1.0f,
              "keysound volume above maximum");
  ASSERT_TRUE(above.video.frameCap == 0, "frame cap above maximum");

  return true;
}

bool sanitizeAcceptsBoundaryAndUnsupportedValues() {
  AudioVideoSettings minimum;
  minimum.audio.requestedSampleRate = 8000;
  minimum.audio.requestedBufferFrames = 16;
  minimum.audio.masterVolume = 0.0f;
  minimum.audio.bgmVolume = 0.0f;
  minimum.audio.keysoundVolume = 0.0f;
  minimum.video.frameCap = 1;
  minimum.sanitize();

  ASSERT_TRUE(minimum.audio.requestedSampleRate == 8000, "minimum sample rate");
  ASSERT_TRUE(minimum.audio.requestedBufferFrames == 16,
              "minimum buffer frames");
  ASSERT_TRUE(minimum.audio.masterVolume == 0.0f, "minimum master volume");
  ASSERT_TRUE(minimum.audio.bgmVolume == 0.0f, "minimum BGM volume");
  ASSERT_TRUE(minimum.audio.keysoundVolume == 0.0f, "minimum keysound volume");
  ASSERT_TRUE(minimum.video.frameCap == 1, "minimum frame cap");

  AudioVideoSettings maximum;
  maximum.audio.outputDeviceId = "backend:unavailable-but-valid";
  maximum.audio.requestedSampleRate = 384000;
  maximum.audio.requestedBufferFrames = 8192;
  maximum.audio.masterVolume = 1.0f;
  maximum.audio.bgmVolume = 1.0f;
  maximum.audio.keysoundVolume = 1.0f;
  maximum.video.mode = DisplayMode::ExclusiveFullscreen;
  maximum.video.displayIndex = 99;
  maximum.video.width = 7680;
  maximum.video.height = 4320;
  maximum.video.frameCap = 1000;
  maximum.sanitize();

  ASSERT_TRUE(maximum.audio.outputDeviceId == "backend:unavailable-but-valid",
              "valid unavailable output device");
  ASSERT_TRUE(maximum.audio.requestedSampleRate == 384000,
              "maximum sample rate");
  ASSERT_TRUE(maximum.audio.requestedBufferFrames == 8192,
              "maximum buffer frames");
  ASSERT_TRUE(maximum.audio.masterVolume == 1.0f, "maximum master volume");
  ASSERT_TRUE(maximum.audio.bgmVolume == 1.0f, "maximum BGM volume");
  ASSERT_TRUE(maximum.audio.keysoundVolume == 1.0f, "maximum keysound volume");
  ASSERT_TRUE(maximum.video.mode == DisplayMode::ExclusiveFullscreen,
              "valid exclusive display mode");
  ASSERT_TRUE(maximum.video.displayIndex == 99,
              "unavailable display index intent");
  ASSERT_TRUE(maximum.video.width == 7680, "unavailable display width intent");
  ASSERT_TRUE(maximum.video.height == 4320,
              "unavailable display height intent");
  ASSERT_TRUE(maximum.video.frameCap == 1000, "maximum frame cap");

  return true;
}

bool sanitizeReplacesNonFiniteVolumes() {
  AudioVideoSettings value;
  value.audio.masterVolume = std::numeric_limits<float>::quiet_NaN();
  value.audio.bgmVolume = std::numeric_limits<float>::infinity();
  value.audio.keysoundVolume = -std::numeric_limits<float>::infinity();
  value.sanitize();

  ASSERT_TRUE(value.audio.masterVolume == 1.0f, "non-finite master volume");
  ASSERT_TRUE(value.audio.bgmVolume == 1.0f, "non-finite BGM volume");
  ASSERT_TRUE(value.audio.keysoundVolume == 1.0f, "non-finite keysound volume");

  return true;
}

bool platformFactoryMatchesCurrentPlatformDefaults() {
  const AudioVideoSettings value =
      player_settings::defaultAudioVideoSettingsForPlatform();

#if TARGET_OS_IPHONE || TARGET_OS_ANDROID
  ASSERT_TRUE(value.video.mode == DisplayMode::ExclusiveFullscreen,
              "mobile fullscreen default");
  ASSERT_TRUE(value.video.vsync, "mobile VSync default");
#else
  ASSERT_TRUE(value == AudioVideoSettings{}, "desktop legacy defaults");
#endif

  return true;
}

bool appSettingsSanitizesAudioVideoSettings() {
  AppSettings value;
  value.audioVideo.audio.outputDeviceId = "missing:app-device";
  value.audioVideo.audio.requestedBufferFrames = 15;
  value.audioVideo.video.frameCap = 1;
  value.noteStartPositionPercent = 100;
  value.sanitize();

  ASSERT_TRUE(value.audioVideo.audio.outputDeviceId == "missing:app-device",
              "AppSettings preserves device intent");
  ASSERT_TRUE(value.audioVideo.audio.requestedBufferFrames == 0,
              "AppSettings sanitizes buffer frames");
  ASSERT_TRUE(value.audioVideo.video.frameCap == 1,
              "AppSettings preserves minimum frame cap");
  ASSERT_TRUE(value.noteStartPositionPercent == 100,
              "AppSettings accepts full lane cover");

  value.noteStartPositionPercent = 101;
  value.sanitize();
  ASSERT_TRUE(value.noteStartPositionPercent == 100,
              "AppSettings clamps lane cover above 100 percent");

  return true;
}
} // namespace

int main() {
  if (!defaultsMatchLegacyBehavior() || !sanitizeRejectsOutOfRangeValues() ||
      !sanitizeAcceptsBoundaryAndUnsupportedValues() ||
      !sanitizeReplacesNonFiniteVolumes() ||
      !platformFactoryMatchesCurrentPlatformDefaults() ||
      !appSettingsSanitizesAudioVideoSettings()) {
    return 1;
  }
  return 0;
}
