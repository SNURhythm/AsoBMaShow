#include "scene/SettingsAudioVideoModel.h"

#include <chrono>
#include <cmath>
#include <deque>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;

void require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

bool nearlyEqual(double left, double right, double epsilon = 0.0001) {
  return std::abs(left - right) <= epsilon;
}

const ChoiceOption *findOption(const ChoiceControlModel &control,
                               const std::string &value) {
  for (const auto &option : control.options) {
    if (option.persistedValue == value) {
      return &option;
    }
  }
  return nullptr;
}

audio::Capabilities desktopAudioCapabilities() {
  return {
      .canSelectOutputDevice = true,
      .canSelectSampleRate = true,
      .canSelectBufferFrames = true,
      .outputDevices = {{.id = "builtin:output",
                         .name = "Built-in Output",
                         .isDefault = true,
                         .sampleRates = {44100, 48000},
                         .bufferFrames = {0, 128, 256}},
                        {.id = "usb:studio-dac",
                         .name = "Studio DAC",
                         .sampleRates = {48000, 96000},
                         .bufferFrames = {0, 64, 128}}},
  };
}

audio::Capabilities fixedAudioCapabilities() {
  return {
      .canSelectOutputDevice = false,
      .canSelectSampleRate = false,
      .canSelectBufferFrames = false,
      .outputDevices = {{.id = "system:default",
                         .name = "System Output",
                         .isDefault = true,
                         .sampleRates = {48000},
                         .bufferFrames = {256}}},
  };
}

display::Capabilities desktopDisplayCapabilities() {
  return {
      .canChangeMode = true,
      .canSelectDisplay = true,
      .canSelectResolution = true,
      .canChangeVsync = true,
      .canSetFrameCap = true,
      .displays = {{.index = 0,
                    .name = "Laptop Display",
                    .resolutions = {{1280, 720, 60},
                                    {1920, 1080, 60},
                                    {1920, 1080, 120}}},
                   {.index = 2,
                    .name = "Projector",
                    .resolutions = {{1920, 1080, 60}}}},
  };
}

display::Capabilities fixedDisplayCapabilities() {
  return {
      .canChangeMode = false,
      .canSelectDisplay = false,
      .canSelectResolution = false,
      .canChangeVsync = false,
      .canSetFrameCap = true,
      .displays = {{.index = 0,
                    .name = "System Display",
                    .resolutions = {{2532, 1170, 60}}}},
  };
}

class FakeAudioRuntime final : public audio::IAudioRuntime {
public:
  audio::Capabilities exposedCapabilities = desktopAudioCapabilities();
  audio::RuntimeState state{
      .request = {.deviceId = "builtin:output",
                  .sampleRate = 48000,
                  .bufferFrames = 256},
      .effectiveSampleRate = 48000,
      .effectiveBufferFrames = 256,
      .effectiveLatencyMs = 999.0,
  };
  bool restartSucceeds = true;
  bool restoreSucceeds = true;
  int restartCalls = 0;
  audio::Volumes volumes;

  audio::Capabilities capabilities() const override {
    return exposedCapabilities;
  }

  audio::RuntimeState runtimeState() const override { return state; }

  bool restart(const audio::StreamRequest &request,
               std::string &errorMessage) override {
    ++restartCalls;
    if (!restartSucceeds) {
      errorMessage = "injected restart failure";
      return false;
    }
    state.request = request;
    state.effectiveSampleRate =
        request.sampleRate == 0 ? 48000 : request.sampleRate;
    state.effectiveBufferFrames =
        request.bufferFrames == 0 ? 256 : request.bufferFrames;
    state.effectiveLatencyMs =
        1000.0 * state.effectiveBufferFrames / state.effectiveSampleRate;
    return true;
  }

  bool restore(const audio::RuntimeState &previous,
               std::string &errorMessage) override {
    if (!restoreSucceeds) {
      errorMessage = "injected restore failure";
      return false;
    }
    state = previous;
    return true;
  }

  void setVolumes(const audio::Volumes &next) override { volumes = next; }
};

class FakePlayback final : public audio::IPlaybackSession {
public:
  audio::PlaybackSnapshot snapshot{
      .active = true, .paused = false, .positionMicros = 1234567};
  int suspendCalls = 0;

  audio::PlaybackSnapshot suspendAndDrain() override {
    ++suspendCalls;
    return snapshot;
  }

  bool restorePlayback(const audio::PlaybackSnapshot &,
                       std::string &) override {
    return true;
  }

  void leavePlaybackStopped() override {}
};

class FakeDisplayBackend final : public display::IDisplayBackend {
public:
  display::Capabilities exposedCapabilities = desktopDisplayCapabilities();
  display::RuntimeState state{.settings = {}};
  std::deque<display::RestoreStatus> restoreStatuses;
  bool applySucceeds = true;
  int applyCalls = 0;
  int restoreCalls = 0;

  display::Capabilities capabilities() const override {
    return exposedCapabilities;
  }

  display::RuntimeState capture() const override { return state; }

  bool apply(const player_settings::VideoSettings &settings,
             std::string &errorMessage) override {
    ++applyCalls;
    state.settings = settings;
    if (!applySucceeds) {
      errorMessage = "injected display apply failure";
      return false;
    }
    return true;
  }

  display::RestoreStatus restore(const display::RuntimeState &previous,
                                 std::string &errorMessage) override {
    ++restoreCalls;
    const auto status = restoreStatuses.empty()
                            ? display::RestoreStatus::Restored
                            : restoreStatuses.front();
    if (!restoreStatuses.empty()) {
      restoreStatuses.pop_front();
    }
    if (status != display::RestoreStatus::Restored) {
      errorMessage = "renderer busy";
      return status;
    }
    state = previous;
    return status;
  }
};

class FakeFrameCapRuntime final : public display::IFrameCapRuntime {
public:
  std::uint32_t cap = 0;

  std::uint32_t currentFrameCap() const override { return cap; }

  bool applyFrameCap(std::uint32_t next, std::string &) override {
    cap = next;
    return true;
  }
};

struct SessionFixture {
  AppSettings settings;
  FakeAudioRuntime audioRuntime;
  FakePlayback playback;
  audio::AudioDeviceManager audioManager{audioRuntime, playback,
                                         settings.audioVideo.audio};
  FakeDisplayBackend displayBackend;
  FakeFrameCapRuntime frameCap;
  display::DisplaySettingsManager displayManager{displayBackend, frameCap,
                                                 settings.audioVideo.video};
  int saves = 0;
  int testSounds = 0;
  SettingsAudioVideoSession session{settings,
                                    audioManager,
                                    displayManager,
                                    {.persist = [this]() { ++saves; },
                                     .playTestSound =
                                         [this]() {
                                           ++testSounds;
                                           return true;
                                         }}};
};

player_settings::VideoSettings riskyDisplayCandidate() {
  player_settings::VideoSettings candidate;
  candidate.mode = player_settings::DisplayMode::ExclusiveFullscreen;
  candidate.displayIndex = 0;
  candidate.width = 1920;
  candidate.height = 1080;
  candidate.vsync = true;
  candidate.frameCap = 120;
  return candidate;
}

void testAudioModelPreservesUnavailableStableIdAndFriendlyLabels() {
  auto intent = player_settings::AudioSettings{
      .outputDeviceId = "missing:device",
      .requestedSampleRate = 96000,
      .requestedBufferFrames = 64,
  };
  const audio::RuntimeState effective{
      .request = {.deviceId = "builtin:output",
                  .sampleRate = 48000,
                  .bufferFrames = 256},
      .effectiveSampleRate = 48000,
      .effectiveBufferFrames = 256,
      .effectiveLatencyMs = 999.0,
  };

  const auto model =
      BuildAudioControlModel(intent, desktopAudioCapabilities(), effective);

  require(!model.devices.options.empty(), "device choices remain visible");
  require(model.devices.options.front().persistedValue == "missing:device",
          "missing stable ID remains the persisted choice");
  require(model.devices.options.front().label == "missing:device (Unavailable)",
          "missing stable ID has an explicit unavailable label");
  require(!model.devices.options.front().available,
          "missing stable ID is not presented as selectable");
  const auto *friendly = findOption(model.devices, "usb:studio-dac");
  require(friendly != nullptr && friendly->label == "Studio DAC",
          "available stable IDs use friendly labels");
  require(model.devices.selectedValue == "missing:device",
          "selection preserves imported unavailable intent");
  require(nearlyEqual(model.effectiveLatencyMs, 1000.0 * 256.0 / 48000.0),
          "latency uses effective frames and rate rather than requested or "
          "backend placeholder values");
  require(model.effectiveSampleRate == 48000 &&
              model.effectiveBufferFrames == 256,
          "effective format is exposed independently from intent");
}

void testAudioModelShowsFixedControlsDisabledWithExplanations() {
  player_settings::AudioSettings intent;
  intent.outputDeviceId = "desktop:imported";
  intent.requestedSampleRate = 96000;
  intent.requestedBufferFrames = 64;
  const audio::RuntimeState effective{
      .request = {.deviceId = "system:default"},
      .effectiveSampleRate = 48000,
      .effectiveBufferFrames = 256,
  };

  const auto model =
      BuildAudioControlModel(intent, fixedAudioCapabilities(), effective);
  require(!model.devices.enabled && !model.sampleRates.enabled &&
              !model.bufferFrames.enabled,
          "fixed stream fields stay visible but disabled");
  require(!model.devices.explanation.empty() &&
              !model.sampleRates.explanation.empty() &&
              !model.bufferFrames.explanation.empty(),
          "every disabled stream field explains the platform limitation");
  require(model.masterVolume.enabled && model.bgmVolume.enabled &&
              model.keysoundVolume.enabled,
          "app-owned volume controls remain enabled on fixed mobile audio");
  require(findOption(model.devices, "desktop:imported") != nullptr,
          "disabled imported stream intent remains visible");
}

void testDisplayModelUsesFriendlyLabelsAndShowsFixedFields() {
  player_settings::VideoSettings intent;
  intent.displayIndex = 9;
  intent.width = 1600;
  intent.height = 900;
  intent.frameCap = 75;

  const auto desktop =
      BuildDisplayControlModel(intent, desktopDisplayCapabilities());
  require(desktop.displays.options.front().persistedValue == "9" &&
              desktop.displays.options.front().label ==
                  "Display 9 (Unavailable)",
          "missing display intent remains the first unavailable choice");
  require(findOption(desktop.displays, "0") != nullptr &&
              findOption(desktop.displays, "0")->label == "Laptop Display",
          "display indices use friendly runtime names");
  require(findOption(desktop.frameCaps, "75") != nullptr,
          "valid custom frame caps remain selectable");

  const auto fixed =
      BuildDisplayControlModel(intent, fixedDisplayCapabilities());
  require(!fixed.modes.enabled && !fixed.displays.enabled &&
              !fixed.resolutions.enabled && !fixed.vsync.enabled,
          "system-managed display fields remain visible but disabled");
  require(!fixed.modes.explanation.empty() &&
              !fixed.displays.explanation.empty() &&
              !fixed.resolutions.explanation.empty() &&
              !fixed.vsync.explanation.empty(),
          "fixed display controls explain why they cannot be changed");
  require(fixed.frameCaps.enabled,
          "app-owned frame limiting remains available on fixed displays");
}

void testVolumeChangesApplyAndPersistImmediatelyDespiteImportedStreamIntent() {
  SessionFixture fixture;
  fixture.audioRuntime.exposedCapabilities = fixedAudioCapabilities();
  fixture.settings.audioVideo.audio.outputDeviceId = "desktop:imported";
  fixture.settings.audioVideo.audio.requestedSampleRate = 96000;
  fixture.settings.audioVideo.audio.requestedBufferFrames = 64;

  const auto result = fixture.session.applyVolumes(0.4F, 0.5F, 0.6F);

  require(result.status == audio::ApplyStatus::Applied ||
              result.status == audio::ApplyStatus::Unsupported,
          "volume-only apply may report a fixed stream but never treats the "
          "safe gain update as a restart failure");
  require(fixture.audioRuntime.restartCalls == 0 &&
              fixture.playback.suspendCalls == 0,
          "volume-only changes never restart or suspend playback");
  require(nearlyEqual(fixture.audioRuntime.volumes.master, 0.4) &&
              nearlyEqual(fixture.audioRuntime.volumes.bgm, 0.5) &&
              nearlyEqual(fixture.audioRuntime.volumes.keysound, 0.6),
          "all app-owned volume groups apply immediately");
  require(fixture.settings.audioVideo.audio.outputDeviceId ==
                  "desktop:imported" &&
              fixture.settings.audioVideo.audio.requestedSampleRate == 96000 &&
              fixture.settings.audioVideo.audio.requestedBufferFrames == 64,
          "volume-only changes do not discard imported stream intent");
  require(nearlyEqual(fixture.settings.audioVideo.audio.masterVolume, 0.4) &&
              fixture.saves == 1,
          "volume-only changes save immediately");
}

void testStreamIntentPersistsOnlyAfterSuccessfulApply() {
  SessionFixture fixture;
  auto candidate = fixture.settings.audioVideo.audio;
  candidate.outputDeviceId = "usb:studio-dac";
  candidate.requestedSampleRate = 96000;
  candidate.requestedBufferFrames = 64;

  fixture.audioRuntime.restartSucceeds = false;
  const auto failed = fixture.session.applyStreamIntent(candidate);
  require(failed.status == audio::ApplyStatus::FailedRolledBack,
          "failed stream restart is reported");
  require(fixture.settings.audioVideo.audio.outputDeviceId.empty() &&
              fixture.saves == 0,
          "failed stream intent neither replaces nor saves working intent");

  fixture.audioRuntime.restartSucceeds = true;
  const auto applied = fixture.session.applyStreamIntent(candidate);
  require(applied.status == audio::ApplyStatus::Applied,
          "working stream request applies");
  require(fixture.settings.audioVideo.audio.outputDeviceId ==
                  "usb:studio-dac" &&
              fixture.settings.audioVideo.audio.requestedSampleRate == 96000 &&
              fixture.settings.audioVideo.audio.requestedBufferFrames == 64 &&
              fixture.saves == 1,
          "successful stream intent replaces and saves persisted intent");
}

void testSettingsTestSoundUsesInjectedKeysoundPath() {
  SessionFixture fixture;
  require(fixture.session.playTestSound(), "test sound callback succeeds");
  require(fixture.testSounds == 1,
          "settings session routes test sound through its keysound callback");
}

void testSettingsTestSoundLoadsPlatformAssetBytes() {
  const path_t soundPath = PATH("assets/audio/sample.wav");
  const std::vector<unsigned char> assetBytes = {0x52, 0x49, 0x46, 0x46};
  int step = 0;

  const bool played = PlaySettingsTestSoundAsset(
      soundPath,
      {.readAssetBytes =
           [&](const path_t &requested) {
             require(requested == soundPath,
                     "platform reader receives asset path");
             require(step == 0, "asset bytes are read before decoding");
             step = 1;
             return std::optional{assetBytes};
           },
       .loadSoundFromMemory =
           [&](const path_t &requested,
               const std::vector<unsigned char> &bytes) {
             require(requested == soundPath,
                     "memory decoder keeps the sound cache key");
             require(bytes == assetBytes,
                     "platform asset bytes are passed to memory decoding");
             require(step == 1, "memory decoding follows platform asset read");
             step = 2;
             return true;
           },
       .playKeysound =
           [&](const path_t &requested) {
             require(requested == soundPath,
                     "playback uses the decoded cache key");
             require(step == 2, "playback starts only after memory decoding");
             step = 3;
             return true;
           }});

  require(played && step == 3,
          "settings test sound uses platform bytes and memory decoding");
}

void testDisplayPreviewPersistsOnlyWhenKept() {
  SessionFixture fixture;
  const auto candidate = riskyDisplayCandidate();
  const auto now = Clock::time_point{};

  const auto preview = fixture.session.beginDisplayPreview(candidate, now);
  require(preview.status == display::ApplyStatus::PreviewPending,
          "risky display changes open a confirmation preview");
  require(fixture.session.hasDisplayPreview() && fixture.saves == 0,
          "previewing does not persist candidate display intent");
  require(fixture.settings.audioVideo.video != candidate,
          "preview leaves active profile intent unchanged");
  require(fixture.session.displayPreviewSecondsRemaining(now) == 15,
          "preview begins with the full fifteen-second countdown");

  const auto kept = fixture.session.keepDisplayPreview();
  require(kept.status == display::ApplyStatus::Applied,
          "Keep confirms the working preview");
  require(!fixture.session.hasDisplayPreview() &&
              fixture.settings.audioVideo.video == candidate &&
              fixture.saves == 1,
          "Keep persists exactly the confirmed candidate");
}

void testSafeFrameCapOnlyChangePersistsWithoutOverlay() {
  SessionFixture fixture;
  auto candidate = fixture.settings.audioVideo.video;
  candidate.frameCap = 120;

  const auto result =
      fixture.session.beginDisplayPreview(candidate, Clock::time_point{});
  require(result.status == display::ApplyStatus::Applied,
          "frame-cap-only change applies without risky display preview");
  require(!fixture.session.hasDisplayPreview() && fixture.saves == 1 &&
              fixture.settings.audioVideo.video.frameCap == 120,
          "safe display change persists immediately without overlay");
}

void testFixedDisplayFrameCapPreservesImportedDisabledIntent() {
  AppSettings settings;
  settings.audioVideo.video.mode = player_settings::DisplayMode::Windowed;
  settings.audioVideo.video.displayIndex = 7;
  settings.audioVideo.video.width = 1600;
  settings.audioVideo.video.height = 900;
  settings.audioVideo.video.vsync = false;

  FakeAudioRuntime audioRuntime;
  FakePlayback playback;
  audio::AudioDeviceManager audioManager(audioRuntime, playback,
                                         settings.audioVideo.audio);
  FakeDisplayBackend displayBackend;
  displayBackend.exposedCapabilities = fixedDisplayCapabilities();
  displayBackend.state.settings.mode =
      player_settings::DisplayMode::ExclusiveFullscreen;
  displayBackend.state.settings.displayIndex = 0;
  displayBackend.state.settings.width = 2532;
  displayBackend.state.settings.height = 1170;
  displayBackend.state.settings.vsync = true;
  FakeFrameCapRuntime frameCap;
  display::DisplaySettingsManager displayManager(displayBackend, frameCap,
                                                 settings.audioVideo.video);
  int saves = 0;
  SettingsAudioVideoSession session(
      settings, audioManager, displayManager,
      {.persist = [&saves]() { ++saves; }, .playTestSound = {}});

  auto candidate = settings.audioVideo.video;
  candidate.frameCap = 120;
  const auto result =
      session.beginDisplayPreview(candidate, Clock::time_point{});

  require(result.status == display::ApplyStatus::Applied,
          "fixed mobile display still accepts its app-owned frame cap");
  require(displayBackend.applyCalls == 0 && frameCap.cap == 120,
          "frame-cap-only update does not submit disabled display fields");
  require(settings.audioVideo.video.mode ==
                  player_settings::DisplayMode::Windowed &&
              settings.audioVideo.video.displayIndex == 7 &&
              settings.audioVideo.video.width == 1600 &&
              settings.audioVideo.video.height == 900 &&
              !settings.audioVideo.video.vsync &&
              settings.audioVideo.video.frameCap == 120 && saves == 1,
          "supported frame cap saves while imported fixed-field intent is "
          "preserved byte-for-byte");
}

void testBorderlessPreviewIgnoresStaleWindowedResolutionIntent() {
  AppSettings settings;
  settings.audioVideo.video.width = 1600;
  settings.audioVideo.video.height = 900;
  FakeAudioRuntime audioRuntime;
  FakePlayback playback;
  audio::AudioDeviceManager audioManager(audioRuntime, playback,
                                         settings.audioVideo.audio);
  FakeDisplayBackend displayBackend;
  FakeFrameCapRuntime frameCap;
  display::DisplaySettingsManager displayManager(displayBackend, frameCap,
                                                 settings.audioVideo.video);
  int saves = 0;
  SettingsAudioVideoSession session(
      settings, audioManager, displayManager,
      {.persist = [&saves]() { ++saves; }, .playTestSound = {}});

  auto candidate = settings.audioVideo.video;
  candidate.mode = player_settings::DisplayMode::BorderlessFullscreen;
  const auto preview =
      session.beginDisplayPreview(candidate, Clock::time_point{});

  require(preview.status == display::ApplyStatus::PreviewPending,
          "borderless preview does not reject an irrelevant stale windowed "
          "resolution");
  require(displayBackend.state.settings.mode ==
              player_settings::DisplayMode::BorderlessFullscreen,
          "borderless runtime candidate is applied");
  require(session.keepDisplayPreview().status == display::ApplyStatus::Applied,
          "canonical borderless runtime can be confirmed");
  require(settings.audioVideo.video.width == 1600 &&
              settings.audioVideo.video.height == 900 && saves == 1,
          "confirmation preserves separate imported windowed-size intent");
}

void testDisplayTimeoutFocusLossAndTabExitRevertWithoutSaving() {
  {
    SessionFixture fixture;
    const auto now = Clock::time_point{};
    fixture.session.beginDisplayPreview(riskyDisplayCandidate(), now);
    const auto result = fixture.session.tick(
        now + display::DisplaySettingsManager::kConfirmationTimeout);
    require(result.has_value() &&
                result->status == display::ApplyStatus::Applied,
            "timeout completes rollback at the exact manager deadline");
    require(!fixture.session.hasDisplayPreview() && fixture.saves == 0 &&
                fixture.displayBackend.state.settings ==
                    player_settings::VideoSettings{},
            "timeout restores runtime and leaves persisted intent untouched");
  }

  {
    SessionFixture fixture;
    fixture.session.beginDisplayPreview(riskyDisplayCandidate(),
                                        Clock::time_point{});
    const auto result = fixture.session.onFocusLost();
    require(result.has_value() &&
                result->status == display::ApplyStatus::Applied,
            "focus loss completes immediate rollback");
    require(!fixture.session.hasDisplayPreview() && fixture.saves == 0,
            "focus loss closes overlay without saving candidate");
  }

  {
    SessionFixture fixture;
    fixture.session.beginDisplayPreview(riskyDisplayCandidate(),
                                        Clock::time_point{});
    const auto result = fixture.session.leaveDisplayTab();
    require(result.status == display::ApplyStatus::Applied &&
                !fixture.session.hasDisplayPreview() && fixture.saves == 0,
            "leaving Display explicitly restores the working configuration");
  }
}

void testCleanupDrainsRetryableDisplayRollback() {
  SessionFixture fixture;
  fixture.session.beginDisplayPreview(riskyDisplayCandidate(),
                                      Clock::time_point{});
  fixture.displayBackend.restoreStatuses = {
      display::RestoreStatus::RetryableFailure,
      display::RestoreStatus::Restored,
  };

  const auto result = fixture.session.cleanup();
  require(result.status == display::ApplyStatus::Applied,
          "cleanup drains a transiently busy rollback");
  require(fixture.displayBackend.restoreCalls == 2 &&
              !fixture.session.hasDisplayPreview() && fixture.saves == 0,
          "cleanup retries until runtime is restored and never saves preview");
}

void testFailedDisplayApplyBlocksUntilRetryableRollbackFinishes() {
  SessionFixture fixture;
  fixture.displayBackend.applySucceeds = false;
  fixture.displayBackend.restoreStatuses = {
      display::RestoreStatus::RetryableFailure,
      display::RestoreStatus::Restored,
  };
  const auto now = Clock::time_point{};

  const auto failed =
      fixture.session.beginDisplayPreview(riskyDisplayCandidate(), now);
  require(failed.status == display::ApplyStatus::RollbackPending,
          "partial display failure reports pending recovery");
  require(fixture.session.hasDisplayPreview(),
          "pending recovery keeps the blocking overlay state active");
  require(!fixture.session.displayPreviewCandidate().has_value(),
          "pending recovery is not a confirmable display candidate");
  require(!fixture.session.reconcileDisplayPreview() &&
              fixture.session.hasDisplayPreview(),
          "reconciliation retains blocking state while rollback is pending");
  require(fixture.saves == 0,
          "failed display candidate is never persisted during recovery");

  const auto keep = fixture.session.keepDisplayPreview();
  require(keep.status == display::ApplyStatus::RollbackPending &&
              fixture.displayBackend.restoreCalls == 1,
          "Keep cannot confirm or interfere with rollback recovery");

  const auto recovered = fixture.session.tick(now);
  require(recovered.has_value() &&
              recovered->status == display::ApplyStatus::FailedRolledBack,
          "session tick retries and completes the pending rollback");
  require(!fixture.session.hasDisplayPreview() && fixture.saves == 0 &&
              fixture.displayBackend.state.settings ==
                  player_settings::VideoSettings{},
          "blocking state ends only after runtime restoration without saving");
}
} // namespace

int main() {
  testAudioModelPreservesUnavailableStableIdAndFriendlyLabels();
  testAudioModelShowsFixedControlsDisabledWithExplanations();
  testDisplayModelUsesFriendlyLabelsAndShowsFixedFields();
  testVolumeChangesApplyAndPersistImmediatelyDespiteImportedStreamIntent();
  testStreamIntentPersistsOnlyAfterSuccessfulApply();
  testSettingsTestSoundUsesInjectedKeysoundPath();
  testSettingsTestSoundLoadsPlatformAssetBytes();
  testDisplayPreviewPersistsOnlyWhenKept();
  testSafeFrameCapOnlyChangePersistsWithoutOverlay();
  testFixedDisplayFrameCapPreservesImportedDisabledIntent();
  testBorderlessPreviewIgnoresStaleWindowedResolutionIntent();
  testDisplayTimeoutFocusLossAndTabExitRevertWithoutSaving();
  testCleanupDrainsRetryableDisplayRollback();
  testFailedDisplayApplyBlocksUntilRetryableRollbackFinishes();
  return 0;
}
