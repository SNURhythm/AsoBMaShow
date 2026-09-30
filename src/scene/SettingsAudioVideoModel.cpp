#include "../i18n/Localization.h"
#include "SettingsAudioVideoModel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <set>
#include <utility>

namespace {
using player_settings::DisplayMode;

template <typename Range, typename Predicate>
bool anyOf(const Range &range, Predicate predicate) {
  return std::ranges::any_of(range, std::move(predicate));
}

void addChoiceIfMissing(ChoiceControlModel &control, ChoiceOption option,
                        bool atFront = false) {
  const bool exists = anyOf(control.options, [&](const ChoiceOption &current) {
    return current.persistedValue == option.persistedValue;
  });
  if (exists) {
    return;
  }
  if (atFront) {
    control.options.insert(control.options.begin(), std::move(option));
  } else {
    control.options.push_back(std::move(option));
  }
}

const audio::DeviceInfo *
findAudioDevice(const audio::Capabilities &capabilities,
                const std::string &deviceId) {
  if (!deviceId.empty()) {
    const auto found = std::ranges::find_if(
        capabilities.outputDevices,
        [&](const audio::DeviceInfo &device) { return device.id == deviceId; });
    return found == capabilities.outputDevices.end() ? nullptr : &*found;
  }
  const auto defaultDevice = std::ranges::find_if(
      capabilities.outputDevices,
      [](const audio::DeviceInfo &device) { return device.isDefault; });
  if (defaultDevice != capabilities.outputDevices.end()) {
    return &*defaultDevice;
  }
  return capabilities.outputDevices.empty() ? nullptr
                                            : &capabilities.outputDevices[0];
}

std::string deviceLabel(const audio::Capabilities &capabilities,
                        const std::string &deviceId) {
  if (deviceId.empty()) {
    return i18n::tr("settings.audio_video.system_default.label");
  }
  if (const auto *device = findAudioDevice(capabilities, deviceId)) {
    return device->name.empty() ? device->id : device->name;
  }
  return deviceId + i18n::tr("settings.audio_video.unavailable.suffix");
}

ChoiceControlModel
buildUnsignedChoices(std::uint32_t selected,
                     const std::vector<std::uint32_t> &available, bool enabled,
                     i18n::Text explanation, const i18n::Text &automaticLabel,
                     const char *suffix) {
  ChoiceControlModel control{.selectedValue = std::to_string(selected),
                             .enabled = enabled,
                             .explanation = std::move(explanation)};
  const bool selectedAvailable =
      selected == 0 ||
      std::ranges::find(available, selected) != available.end();
  if (!selectedAvailable) {
    addChoiceIfMissing(
        control,
        {.persistedValue = std::to_string(selected),
         .label = i18n::message("settings.audio_video.option.unavailable",
                                {{"value", std::to_string(selected) + suffix}}),
         .available = false},
        true);
  }
  addChoiceIfMissing(control, {.persistedValue = "0", .label = automaticLabel});
  for (const auto value : available) {
    addChoiceIfMissing(control, {.persistedValue = std::to_string(value),
                                 .label = std::to_string(value) + suffix});
  }
  return control;
}

const display::DisplayInfo *
findDisplay(const display::Capabilities &capabilities, int index) {
  const auto found = std::ranges::find_if(
      capabilities.displays, [index](const display::DisplayInfo &display) {
        return display.index == index;
      });
  return found == capabilities.displays.end() ? nullptr : &*found;
}

std::string modeValue(DisplayMode mode) {
  switch (mode) {
  case DisplayMode::Windowed:
    return "windowed";
  case DisplayMode::BorderlessFullscreen:
    return "borderless";
  case DisplayMode::ExclusiveFullscreen:
    return "exclusive";
  }
  return "windowed";
}

std::string resolutionValue(int width, int height) {
  return std::to_string(width) + "x" + std::to_string(height);
}

float sanitizeVolume(float value) {
  if (!std::isfinite(value)) {
    return 1.0F;
  }
  return std::clamp(value, 0.0F, 1.0F);
}
} // namespace

AudioControlModel
BuildAudioControlModel(const player_settings::AudioSettings &intent,
                       const audio::Capabilities &capabilities,
                       const audio::RuntimeState &effective) {
  AudioControlModel model;
  model.devices.selectedValue = intent.outputDeviceId;
  const bool persistedDeviceAvailable =
      intent.outputDeviceId.empty() ||
      findAudioDevice(capabilities, intent.outputDeviceId) != nullptr;
  if (!persistedDeviceAvailable) {
    addChoiceIfMissing(model.devices,
                       {.persistedValue = intent.outputDeviceId,
                        .label = i18n::message("settings.audio_video.option.unavailable",
                                               {{"value", intent.outputDeviceId}}),
                        .available = false},
                       true);
  }
  addChoiceIfMissing(model.devices,
                     {.persistedValue = "", .label = i18n::message("settings.audio_video.system_default.label")});
  for (const auto &device : capabilities.outputDevices) {
    addChoiceIfMissing(
        model.devices,
        {.persistedValue = device.id,
         .label = device.name.empty() ? device.id : device.name});
  }
  model.devices.enabled =
      capabilities.canSelectOutputDevice && !capabilities.outputDevices.empty();
  if (!capabilities.canSelectOutputDevice) {
    model.devices.explanation = i18n::message("settings.audio_video.managed_by_system.message");
  } else if (capabilities.outputDevices.empty()) {
    model.devices.explanation = i18n::message("settings.audio_video.no_output_available.message");
  }

  const auto *selectedDevice =
      findAudioDevice(capabilities, intent.outputDeviceId);
  const std::vector<std::uint32_t> noValues;
  const auto &sampleRates =
      selectedDevice == nullptr ? noValues : selectedDevice->sampleRates;
  const auto &bufferFrames =
      selectedDevice == nullptr ? noValues : selectedDevice->bufferFrames;

  i18n::Text sampleRateExplanation;
  bool sampleRateEnabled = capabilities.canSelectSampleRate;
  if (!capabilities.canSelectSampleRate) {
    sampleRateExplanation = i18n::message("settings.audio_video.managed_by_system.message");
  } else if (selectedDevice == nullptr) {
    sampleRateEnabled = false;
    sampleRateExplanation = i18n::message("settings.audio_video.choose_output_first.message");
  }
  model.sampleRates = buildUnsignedChoices(
      intent.requestedSampleRate, sampleRates, sampleRateEnabled,
      std::move(sampleRateExplanation), i18n::message("settings.audio_video.automatic.label"), " Hz");

  i18n::Text bufferExplanation;
  bool bufferEnabled = capabilities.canSelectBufferFrames;
  if (!capabilities.canSelectBufferFrames) {
    bufferExplanation = i18n::message("settings.audio_video.managed_by_system.message");
  } else if (selectedDevice == nullptr) {
    bufferEnabled = false;
    bufferExplanation = i18n::message("settings.audio_video.choose_output_first.message");
  }
  model.bufferFrames = buildUnsignedChoices(
      intent.requestedBufferFrames, bufferFrames, bufferEnabled,
      std::move(bufferExplanation), i18n::message("settings.audio_video.automatic.label"), " frames");

  model.masterVolume.value = intent.masterVolume;
  model.bgmVolume.value = intent.bgmVolume;
  model.keysoundVolume.value = intent.keysoundVolume;
  model.effectiveDeviceLabel =
      deviceLabel(capabilities, effective.request.deviceId);
  model.effectiveSampleRate = effective.effectiveSampleRate;
  model.effectiveBufferFrames = effective.effectiveBufferFrames;
  if (effective.effectiveSampleRate > 0 &&
      effective.effectiveBufferFrames > 0) {
    model.effectiveLatencyMs =
        1000.0 * static_cast<double>(effective.effectiveBufferFrames) /
        static_cast<double>(effective.effectiveSampleRate);
  }
  return model;
}

DisplayControlModel
BuildDisplayControlModel(const player_settings::VideoSettings &intent,
                         const display::Capabilities &capabilities) {
  DisplayControlModel model;
  model.modes.selectedValue = modeValue(intent.mode);
  model.modes.options = {
      {.persistedValue = "windowed", .label = i18n::message("settings.audio_video.windowed.label")},
      {.persistedValue = "borderless", .label = i18n::message("settings.audio_video.borderless_fullscreen.label")},
      {.persistedValue = "exclusive", .label = i18n::message("settings.audio_video.exclusive_fullscreen.label")},
  };
  model.modes.enabled = capabilities.canChangeMode;
  if (!model.modes.enabled) {
    model.modes.explanation = i18n::message("settings.audio_video.managed_by_system.message");
  }

  model.displays.selectedValue = std::to_string(intent.displayIndex);
  if (findDisplay(capabilities, intent.displayIndex) == nullptr) {
    addChoiceIfMissing(model.displays,
                       {.persistedValue = std::to_string(intent.displayIndex),
                        .label = i18n::message("settings.audio_video.option.unavailable",
                            {{"value", i18n::message("settings.audio_video.display.label",
                                 {{"number", std::to_string(intent.displayIndex)}})}}),
                        .available = false},
                       true);
  }
  for (const auto &display : capabilities.displays) {
    addChoiceIfMissing(
        model.displays,
        {.persistedValue = std::to_string(display.index),
         .label = display.name.empty()
                      ? i18n::message("settings.audio_video.display.label",
                                      {{"number", std::to_string(display.index)}})
                      : display.name});
  }
  model.displays.enabled =
      capabilities.canSelectDisplay && !capabilities.displays.empty();
  if (!capabilities.canSelectDisplay) {
    model.displays.explanation = i18n::message("settings.audio_video.managed_by_system.message");
  } else if (capabilities.displays.empty()) {
    model.displays.explanation = i18n::message("settings.audio_video.no_display_available.message");
  }

  model.resolutions.selectedValue =
      resolutionValue(intent.width, intent.height);
  const auto *selectedDisplay = findDisplay(capabilities, intent.displayIndex);
  bool selectedResolutionAvailable = false;
  std::set<std::pair<int, int>> seenResolutions;
  if (selectedDisplay != nullptr) {
    for (const auto &resolution : selectedDisplay->resolutions) {
      if (!seenResolutions.emplace(resolution.width, resolution.height)
               .second) {
        continue;
      }
      selectedResolutionAvailable =
          selectedResolutionAvailable || (resolution.width == intent.width &&
                                          resolution.height == intent.height);
      const std::string value =
          resolutionValue(resolution.width, resolution.height);
      addChoiceIfMissing(model.resolutions,
                         {.persistedValue = value, .label = value});
    }
  }
  if (!selectedResolutionAvailable) {
    const std::string value = resolutionValue(intent.width, intent.height);
    addChoiceIfMissing(model.resolutions,
                       {.persistedValue = value,
                        .label = i18n::message("settings.audio_video.option.unavailable",
                                               {{"value", value}}),
                        .available = false},
                       true);
  }
  model.resolutions.enabled =
      capabilities.canSelectResolution && selectedDisplay != nullptr;
  if (!capabilities.canSelectResolution) {
    model.resolutions.explanation = i18n::message("settings.audio_video.managed_by_system.message");
  } else if (selectedDisplay == nullptr) {
    model.resolutions.explanation = i18n::message("settings.audio_video.choose_display_first.message");
  }

  model.vsync.selectedValue = intent.vsync ? "on" : "off";
  model.vsync.options = {{.persistedValue = "off", .label = i18n::message("settings.audio_video.off.label")},
                         {.persistedValue = "on", .label = i18n::message("settings.audio_video.on.label")}};
  model.vsync.enabled = capabilities.canChangeVsync;
  if (!model.vsync.enabled) {
    model.vsync.explanation = i18n::message("settings.audio_video.managed_by_system.message");
  }

  model.frameCaps.selectedValue = std::to_string(intent.frameCap);
  constexpr std::array<std::uint32_t, 12> kCommonFrameCaps = {
      0, 1, 5, 15, 30, 60, 75, 90, 120, 144, 165, 240};
  if (std::ranges::find(kCommonFrameCaps, intent.frameCap) ==
      kCommonFrameCaps.end()) {
    addChoiceIfMissing(model.frameCaps,
                       {.persistedValue = std::to_string(intent.frameCap),
                        .label = std::to_string(intent.frameCap) + " FPS"},
                       true);
  }
  for (const auto cap : kCommonFrameCaps) {
    addChoiceIfMissing(
        model.frameCaps,
        {.persistedValue = std::to_string(cap),
         .label = cap == 0 ? i18n::message("settings.audio_video.uncapped.label") : std::to_string(cap) + " FPS"});
  }
  model.frameCaps.enabled = capabilities.canSetFrameCap;
  if (!model.frameCaps.enabled) {
    model.frameCaps.explanation = i18n::message("settings.audio_video.unavailable_on_platform.message");
  }
  return model;
}

bool PlaySettingsTestSound(const SettingsTestSoundCallbacks &callbacks) {
  if (!callbacks.loadGeneratedSound || !callbacks.playSound) {
    return false;
  }
  // An original C4–E4–G4 sine-wave arpeggio, generated without audio assets.
  constexpr int sampleRate = 48000;
  constexpr int channels = 2;
  constexpr int framesPerNote = 14400; // 300 ms, including a 50 ms gap.
  constexpr int soundingFrames = 12000;
  constexpr int attackFrames = 480;
  constexpr int releaseFrames = 1920;
  constexpr std::array frequencies{261.625565, 329.627557, 391.995436};
  std::vector<short> pcm(frequencies.size() * framesPerNote * channels, 0);
  for (std::size_t note = 0; note < frequencies.size(); ++note) {
    for (int frame = 0; frame < soundingFrames; ++frame) {
      const double envelope = std::min(
          {1.0, static_cast<double>(frame) / attackFrames,
           static_cast<double>(soundingFrames - 1 - frame) / releaseFrames});
      const double phase = 2.0 * std::numbers::pi * frequencies[note] *
                           static_cast<double>(frame) / sampleRate;
      const auto sample = static_cast<short>(
          std::sin(phase) * envelope * 0.25 * std::numeric_limits<short>::max());
      const auto offset = (note * framesPerNote + frame) * channels;
      pcm[offset] = sample;
      pcm[offset + 1] = sample;
    }
  }
  static const path_t key = PATH("@settings_test_c_major");
  if (!callbacks.loadGeneratedSound(key, std::move(pcm), channels, sampleRate)) {
    return false;
  }
  return callbacks.playSound(key);
}

SettingsAudioVideoSession::SettingsAudioVideoSession(
    AppSettings &settings, audio::AudioDeviceManager &audioManager,
    display::DisplaySettingsManager &displayManager, Callbacks callbacks)
    : settings_(settings), audioManager_(audioManager),
      displayManager_(displayManager), callbacks_(std::move(callbacks)) {}

audio::ApplyResult SettingsAudioVideoSession::applyVolumes(float master,
                                                           float bgm,
                                                           float keysound) {
  auto candidate = audioManager_.lastWorkingSettings();
  candidate.masterVolume = sanitizeVolume(master);
  candidate.bgmVolume = sanitizeVolume(bgm);
  candidate.keysoundVolume = sanitizeVolume(keysound);
  audio::ApplyResult result = audioManager_.apply(candidate);

  auto &persisted = settings_.audioVideo.audio;
  persisted.masterVolume = candidate.masterVolume;
  persisted.bgmVolume = candidate.bgmVolume;
  persisted.keysoundVolume = candidate.keysoundVolume;
  persist();
  return result;
}

audio::ApplyResult SettingsAudioVideoSession::applyStreamIntent(
    const player_settings::AudioSettings &candidateIntent) {
  auto candidate = settings_.audioVideo.audio;
  candidate.outputDeviceId = candidateIntent.outputDeviceId;
  candidate.requestedSampleRate = candidateIntent.requestedSampleRate;
  candidate.requestedBufferFrames = candidateIntent.requestedBufferFrames;
  audio::ApplyResult result = audioManager_.apply(candidate);
  if (result.status != audio::ApplyStatus::Applied) {
    return result;
  }

  auto &persisted = settings_.audioVideo.audio;
  persisted.outputDeviceId = candidate.outputDeviceId;
  persisted.requestedSampleRate = candidate.requestedSampleRate;
  persisted.requestedBufferFrames = candidate.requestedBufferFrames;
  persist();
  return result;
}

bool SettingsAudioVideoSession::playTestSound() {
  return callbacks_.playTestSound && callbacks_.playTestSound();
}

display::ApplyResult SettingsAudioVideoSession::beginDisplayPreview(
    const player_settings::VideoSettings &candidate,
    std::chrono::steady_clock::time_point now) {
  const auto capabilities = displayManager_.capabilities();
  const auto working = displayManager_.lastWorkingSettings();
  auto runtimeCandidate = candidate;
  auto persistedCandidate = settings_.audioVideo.video;
  if (capabilities.canChangeMode) {
    persistedCandidate.mode = candidate.mode;
  } else {
    runtimeCandidate.mode = working.mode;
  }
  if (capabilities.canSelectDisplay) {
    persistedCandidate.displayIndex = candidate.displayIndex;
  } else {
    runtimeCandidate.displayIndex = working.displayIndex;
  }
  if (capabilities.canSelectResolution) {
    persistedCandidate.width = candidate.width;
    persistedCandidate.height = candidate.height;
  } else {
    runtimeCandidate.width = working.width;
    runtimeCandidate.height = working.height;
  }
  if (capabilities.canChangeVsync) {
    persistedCandidate.vsync = candidate.vsync;
  } else {
    runtimeCandidate.vsync = working.vsync;
  }
  if (capabilities.canSetFrameCap) {
    persistedCandidate.frameCap = candidate.frameCap;
  } else {
    runtimeCandidate.frameCap = working.frameCap;
  }
  if (runtimeCandidate.mode == DisplayMode::BorderlessFullscreen) {
    if (const auto *selectedDisplay =
            findDisplay(capabilities, runtimeCandidate.displayIndex);
        selectedDisplay != nullptr && !selectedDisplay->resolutions.empty()) {
      const bool listed = std::ranges::any_of(
          selectedDisplay->resolutions,
          [&](const display::Resolution &resolution) {
            return resolution.width == runtimeCandidate.width &&
                   resolution.height == runtimeCandidate.height;
          });
      if (!listed) {
        runtimeCandidate.width = selectedDisplay->resolutions.front().width;
        runtimeCandidate.height = selectedDisplay->resolutions.front().height;
      }
    }
  }

  display::ApplyResult result =
      displayManager_.beginPreview(runtimeCandidate, now);
  if (result.status == display::ApplyStatus::PreviewPending) {
    displayPreviewCandidate_ = persistedCandidate;
    displayPreviewDeadline_ =
        now + display::DisplaySettingsManager::kConfirmationTimeout;
    displayPreviewBlocking_ = true;
  } else {
    displayPreviewCandidate_.reset();
    displayPreviewBlocking_ = displayManager_.hasPendingPreview();
    if (result.status == display::ApplyStatus::Applied) {
      settings_.audioVideo.video = persistedCandidate;
      persist();
    }
  }
  return result;
}

display::ApplyResult SettingsAudioVideoSession::keepDisplayPreview() {
  display::ApplyResult result = displayManager_.confirmPreview();
  noteDisplayRollbackRecovery(result);
  if (result.status == display::ApplyStatus::Applied &&
      displayPreviewCandidate_.has_value()) {
    settings_.audioVideo.video = *displayPreviewCandidate_;
    displayPreviewCandidate_.reset();
    persist();
  }
  finishDisplayPreviewIfResolved();
  return result;
}

display::ApplyResult SettingsAudioVideoSession::revertDisplayPreview() {
  displayPreviewCandidate_.reset();
  display::ApplyResult result =
      displayManager_.cancelPreview(display::RollbackReason::Cancelled);
  noteDisplayRollbackRecovery(result);
  finishDisplayPreviewIfResolved();
  return result;
}

display::ApplyResult SettingsAudioVideoSession::leaveDisplayTab() {
  return revertDisplayPreview();
}

display::ApplyResult SettingsAudioVideoSession::cleanup() {
  displayPreviewCandidate_.reset();
  display::ApplyResult result = displayManager_.shutdown();
  noteDisplayRollbackRecovery(result);
  finishDisplayPreviewIfResolved();
  return result;
}

std::optional<display::ApplyResult>
SettingsAudioVideoSession::tick(std::chrono::steady_clock::time_point now) {
  auto result = displayManager_.tick(now);
  if (result.has_value()) {
    noteDisplayRollbackRecovery(*result);
  }
  finishDisplayPreviewIfResolved();
  return result;
}

std::optional<display::ApplyResult> SettingsAudioVideoSession::onFocusLost() {
  auto result = displayManager_.onFocusLost();
  if (result.has_value()) {
    noteDisplayRollbackRecovery(*result);
  }
  finishDisplayPreviewIfResolved();
  return result;
}

bool SettingsAudioVideoSession::reconcileDisplayPreview() {
  const bool hadPreview = displayPreviewBlocking_;
  finishDisplayPreviewIfResolved();
  return hadPreview && !displayPreviewBlocking_;
}

bool SettingsAudioVideoSession::hasDisplayPreview() const {
  return displayPreviewBlocking_;
}

int SettingsAudioVideoSession::displayPreviewSecondsRemaining(
    std::chrono::steady_clock::time_point now) const {
  if (!displayPreviewCandidate_.has_value() || now >= displayPreviewDeadline_) {
    return 0;
  }
  const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
      displayPreviewDeadline_ - now);
  return static_cast<int>((remaining.count() + 999) / 1000);
}

const std::optional<player_settings::VideoSettings> &
SettingsAudioVideoSession::displayPreviewCandidate() const {
  return displayPreviewCandidate_;
}

void SettingsAudioVideoSession::persist() {
  settings_.audioVideo.sanitize();
  if (callbacks_.persist) {
    callbacks_.persist();
  }
}

void SettingsAudioVideoSession::noteDisplayRollbackRecovery(
    const display::ApplyResult &result) {
  if (result.status != display::ApplyStatus::RollbackPending) {
    return;
  }
  displayPreviewCandidate_.reset();
  displayPreviewBlocking_ = true;
}

void SettingsAudioVideoSession::finishDisplayPreviewIfResolved() {
  if (displayManager_.hasPendingPreview()) {
    displayPreviewBlocking_ = true;
    return;
  }
  displayPreviewCandidate_.reset();
  displayPreviewBlocking_ = false;
}
