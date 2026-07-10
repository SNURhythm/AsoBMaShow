#pragma once

#include "../AppSettings.h"
#include "../audio/AudioDeviceManager.h"
#include "../video/DisplaySettingsManager.h"

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>

struct ChoiceOption {
  std::string persistedValue;
  std::string label;
  bool available = true;
  bool operator==(const ChoiceOption &) const = default;
};

struct ChoiceControlModel {
  std::string selectedValue;
  std::vector<ChoiceOption> options;
  bool enabled = true;
  std::string explanation;
  bool operator==(const ChoiceControlModel &) const = default;
};

struct VolumeControlModel {
  float value = 1.0F;
  bool enabled = true;
  std::string explanation;
  bool operator==(const VolumeControlModel &) const = default;
};

struct AudioControlModel {
  ChoiceControlModel devices;
  ChoiceControlModel sampleRates;
  ChoiceControlModel bufferFrames;
  VolumeControlModel masterVolume;
  VolumeControlModel bgmVolume;
  VolumeControlModel keysoundVolume;
  std::string effectiveDeviceLabel;
  std::uint32_t effectiveSampleRate = 0;
  std::uint32_t effectiveBufferFrames = 0;
  double effectiveLatencyMs = 0.0;
  bool operator==(const AudioControlModel &) const = default;
};

struct DisplayControlModel {
  ChoiceControlModel modes;
  ChoiceControlModel displays;
  ChoiceControlModel resolutions;
  ChoiceControlModel vsync;
  ChoiceControlModel frameCaps;
  bool operator==(const DisplayControlModel &) const = default;
};

AudioControlModel
BuildAudioControlModel(const player_settings::AudioSettings &intent,
                       const audio::Capabilities &capabilities,
                       const audio::RuntimeState &effective);

DisplayControlModel
BuildDisplayControlModel(const player_settings::VideoSettings &intent,
                         const display::Capabilities &capabilities);

class SettingsAudioVideoSession {
public:
  struct Callbacks {
    std::function<void()> persist;
    std::function<bool()> playTestSound;
  };

  SettingsAudioVideoSession(AppSettings &settings,
                            audio::AudioDeviceManager &audioManager,
                            display::DisplaySettingsManager &displayManager,
                            Callbacks callbacks);

  audio::ApplyResult applyVolumes(float master, float bgm, float keysound);
  audio::ApplyResult
  applyStreamIntent(const player_settings::AudioSettings &candidate);
  bool playTestSound();

  display::ApplyResult
  beginDisplayPreview(const player_settings::VideoSettings &candidate,
                      std::chrono::steady_clock::time_point now);
  display::ApplyResult keepDisplayPreview();
  display::ApplyResult revertDisplayPreview();
  display::ApplyResult leaveDisplayTab();
  display::ApplyResult cleanup();
  std::optional<display::ApplyResult>
  tick(std::chrono::steady_clock::time_point now);
  std::optional<display::ApplyResult> onFocusLost();
  bool reconcileDisplayPreview();

  [[nodiscard]] bool hasDisplayPreview() const;
  [[nodiscard]] int displayPreviewSecondsRemaining(
      std::chrono::steady_clock::time_point now) const;
  [[nodiscard]] const std::optional<player_settings::VideoSettings> &
  displayPreviewCandidate() const;

private:
  void persist();
  void finishDisplayPreviewIfResolved();

  AppSettings &settings_;
  audio::AudioDeviceManager &audioManager_;
  display::DisplaySettingsManager &displayManager_;
  Callbacks callbacks_;
  std::optional<player_settings::VideoSettings> displayPreviewCandidate_;
  std::chrono::steady_clock::time_point displayPreviewDeadline_{};
};
