#include "video/DisplaySettingsManager.h"

#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {
using Clock = std::chrono::steady_clock;
using player_settings::DisplayMode;
using player_settings::VideoSettings;

void require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

display::Capabilities desktopCapabilities() {
  return {
      .canChangeMode = true,
      .canSelectDisplay = true,
      .canSelectResolution = true,
      .canChangeVsync = true,
      .canSetFrameCap = true,
      .displays = {{.index = 0,
                    .name = "Primary",
                    .resolutions = {{1280, 720, 60}, {1920, 1080, 60}}},
                   {.index = 1,
                    .name = "Secondary",
                    .resolutions = {{1920, 1080, 60}}}},
  };
}

VideoSettings initialSettings() { return {}; }

VideoSettings previewSettings() {
  VideoSettings value;
  value.mode = DisplayMode::ExclusiveFullscreen;
  value.width = 1920;
  value.height = 1080;
  value.vsync = true;
  value.frameCap = 120;
  return value;
}

class FakeBackend final : public display::IDisplayBackend {
public:
  display::Capabilities exposedCapabilities = desktopCapabilities();
  display::RuntimeState state{.settings = initialSettings(),
                              .windowX = 100,
                              .windowY = 200,
                              .sdlWindowFlags = 0,
                              .bgfxResetFlags = 0x40};
  bool applySucceeds = true;
  bool restoreSucceeds = true;
  mutable int capabilitiesCalls = 0;
  mutable int captureCalls = 0;
  int applyCalls = 0;
  int restoreCalls = 0;

  display::Capabilities capabilities() const override {
    ++capabilitiesCalls;
    return exposedCapabilities;
  }

  display::RuntimeState capture() const override {
    ++captureCalls;
    return state;
  }

  bool apply(const VideoSettings &settings,
             std::string &errorMessage) override {
    ++applyCalls;
    state.settings = settings;
    if (!applySucceeds) {
      errorMessage = "injected apply failure";
      return false;
    }
    return true;
  }

  bool restore(const display::RuntimeState &snapshot,
               std::string &errorMessage) override {
    ++restoreCalls;
    if (!restoreSucceeds) {
      errorMessage = "injected restore failure";
      return false;
    }
    state = snapshot;
    return true;
  }
};

void testPreviewConfirmation() {
  FakeBackend backend;
  display::DisplaySettingsManager manager(backend, initialSettings());
  const auto now = Clock::time_point{};
  const auto candidate = previewSettings();

  const auto result = manager.beginPreview(candidate, now);
  require(result.status == display::ApplyStatus::PreviewPending,
          "display changes start a preview");
  require(result.effective == candidate, "preview reports candidate state");
  require(manager.hasPendingPreview(), "preview is pending");
  require(backend.applyCalls == 1 && backend.restoreCalls == 0,
          "preview applies once without restoring");
  require(manager.confirmPreview(), "pending preview confirms");
  require(!manager.hasPendingPreview(), "confirmation clears preview");
  require(!manager.tick(now + std::chrono::seconds(30)).has_value(),
          "confirmed preview cannot later time out");
  require(backend.state.settings == candidate,
          "confirmed display state remains active");
}

void testTimeoutAndFocusLossRestoreImmediately() {
  FakeBackend timeoutBackend;
  display::DisplaySettingsManager timeoutManager(timeoutBackend,
                                                 initialSettings());
  const auto now = Clock::time_point{};
  timeoutManager.beginPreview(previewSettings(), now);
  require(!timeoutManager
               .tick(now +
                     display::DisplaySettingsManager::kConfirmationTimeout -
                     std::chrono::milliseconds(1))
               .has_value(),
          "preview remains pending before deadline");
  const auto timeout = timeoutManager.tick(
      now + display::DisplaySettingsManager::kConfirmationTimeout);
  require(timeout.has_value(), "preview expires at the exact deadline");
  require(timeout->status == display::ApplyStatus::Applied,
          "successful timeout rollback is a completed apply result");
  require(timeout->effective == initialSettings(),
          "timeout reports restored settings");
  require(timeoutBackend.restoreCalls == 1, "timeout restores exactly once");
  require(timeoutBackend.state.settings == initialSettings(),
          "timeout restores backend state");

  FakeBackend focusBackend;
  display::DisplaySettingsManager focusManager(focusBackend, initialSettings());
  focusManager.beginPreview(previewSettings(), now);
  focusManager.onFocusLost();
  require(!focusManager.hasPendingPreview(),
          "focus loss clears the preview immediately");
  require(focusBackend.restoreCalls == 1, "focus loss restores exactly once");
  require(focusBackend.state.settings == initialSettings(),
          "focus loss restores backend state");
}

void testApplyFailureRollsBackAndReportsRestoreFailure() {
  FakeBackend rollbackBackend;
  rollbackBackend.applySucceeds = false;
  display::DisplaySettingsManager rollbackManager(rollbackBackend,
                                                  initialSettings());
  const auto rolledBack =
      rollbackManager.beginPreview(previewSettings(), Clock::time_point{});
  require(rolledBack.status == display::ApplyStatus::FailedRolledBack,
          "apply failure reports successful rollback");
  require(rolledBack.effective == initialSettings(),
          "rolled-back failure reports old effective settings");
  require(rollbackBackend.applyCalls == 1 && rollbackBackend.restoreCalls == 1,
          "failed apply restores captured runtime state");
  require(rollbackBackend.state.settings == initialSettings(),
          "failed apply leaves original backend state");

  FakeBackend brokenRestoreBackend;
  brokenRestoreBackend.applySucceeds = false;
  brokenRestoreBackend.restoreSucceeds = false;
  display::DisplaySettingsManager brokenRestoreManager(brokenRestoreBackend,
                                                       initialSettings());
  const auto unrecoverable =
      brokenRestoreManager.beginPreview(previewSettings(), Clock::time_point{});
  require(unrecoverable.status == display::ApplyStatus::FailedUnrecoverable,
          "double failure is reported as unrecoverable");
  require(!unrecoverable.message.empty(),
          "double failure retains a diagnostic");
}

void testUnsupportedFieldsRejectWithoutBackendCalls() {
  FakeBackend invalidBackend;
  display::DisplaySettingsManager invalidManager(invalidBackend,
                                                 initialSettings());
  auto invalid = initialSettings();
  invalid.mode = static_cast<DisplayMode>(99);
  require(invalidManager.beginPreview(invalid, Clock::time_point{}).status ==
              display::ApplyStatus::Unsupported,
          "invalid display mode is rejected");
  invalid = initialSettings();
  invalid.frameCap = 1;
  require(invalidManager.beginPreview(invalid, Clock::time_point{}).status ==
              display::ApplyStatus::Unsupported,
          "invalid frame cap is rejected");
  require(invalidBackend.captureCalls == 0 && invalidBackend.applyCalls == 0,
          "invalid settings never reach the backend");

  FakeBackend backend;
  backend.exposedCapabilities = {
      .canSetFrameCap = true,
      .displays = {{.index = 0,
                    .name = "Fixed",
                    .resolutions = {{1280, 720, 60}}}},
  };
  display::DisplaySettingsManager manager(backend, initialSettings());
  auto candidate = initialSettings();
  candidate.mode = DisplayMode::BorderlessFullscreen;

  const auto unsupported = manager.beginPreview(candidate, Clock::time_point{});
  require(unsupported.status == display::ApplyStatus::Unsupported,
          "unsupported mode is rejected");
  require(backend.captureCalls == 0 && backend.applyCalls == 0 &&
              backend.restoreCalls == 0,
          "unsupported fields cause no backend mutation calls");

  FakeBackend unknownDisplayBackend;
  display::DisplaySettingsManager unknownDisplayManager(unknownDisplayBackend,
                                                        initialSettings());
  candidate = previewSettings();
  candidate.displayIndex = 99;
  const auto unknownDisplay =
      unknownDisplayManager.beginPreview(candidate, Clock::time_point{});
  require(unknownDisplay.status == display::ApplyStatus::Unsupported,
          "unknown display is rejected");
  require(unknownDisplayBackend.captureCalls == 0 &&
              unknownDisplayBackend.applyCalls == 0,
          "unknown display is rejected before capture");

  FakeBackend unknownResolutionBackend;
  display::DisplaySettingsManager unknownResolutionManager(
      unknownResolutionBackend, initialSettings());
  candidate = previewSettings();
  candidate.width = 1111;
  candidate.height = 777;
  const auto unknownResolution =
      unknownResolutionManager.beginPreview(candidate, Clock::time_point{});
  require(unknownResolution.status == display::ApplyStatus::Unsupported,
          "unknown resolution is rejected");
  require(unknownResolutionBackend.captureCalls == 0 &&
              unknownResolutionBackend.applyCalls == 0,
          "unknown resolution is rejected before capture");

  FakeBackend exclusiveModeBackend;
  exclusiveModeBackend.exposedCapabilities.displays[0].resolutions = {
      {1920, 1080, 60}};
  display::DisplaySettingsManager exclusiveModeManager(exclusiveModeBackend,
                                                       initialSettings());
  candidate = initialSettings();
  candidate.mode = DisplayMode::ExclusiveFullscreen;
  const auto unavailableExclusive =
      exclusiveModeManager.beginPreview(candidate, Clock::time_point{});
  require(unavailableExclusive.status == display::ApplyStatus::Unsupported,
          "exclusive mode validates its unchanged requested size");
  require(exclusiveModeBackend.captureCalls == 0 &&
              exclusiveModeBackend.applyCalls == 0,
          "unavailable exclusive mode is rejected before capture");
}

void testSecondPreviewRestoresFirstAndFrameCapIsSafe() {
  FakeBackend backend;
  display::DisplaySettingsManager manager(backend, initialSettings());
  const auto now = Clock::time_point{};

  auto frameCapOnly = initialSettings();
  frameCapOnly.frameCap = 120;
  const auto capped = manager.beginPreview(frameCapOnly, now);
  require(capped.status == display::ApplyStatus::Applied,
          "frame-cap-only changes apply without a preview");
  require(capped.effective == frameCapOnly,
          "frame-cap-only result becomes effective");
  require(backend.captureCalls == 0 && backend.applyCalls == 0,
          "app-owned frame cap does not touch the display backend");

  auto first = previewSettings();
  first.frameCap = 120;
  require(manager.beginPreview(first, now).status ==
              display::ApplyStatus::PreviewPending,
          "first display preview starts");

  auto second = first;
  second.mode = DisplayMode::BorderlessFullscreen;
  const auto replacement =
      manager.beginPreview(second, now + std::chrono::seconds(1));
  require(replacement.status == display::ApplyStatus::PreviewPending,
          "second display preview starts after rollback");
  require(backend.applyCalls == 2 && backend.restoreCalls == 1,
          "second preview restores the first before applying");
  require(backend.state.settings == second,
          "second preview is the only active candidate");
  require(manager.cancelPreview(display::RollbackReason::Cancelled).status ==
              display::ApplyStatus::Applied,
          "explicit cancellation restores successfully");
  require(backend.state.settings == frameCapOnly,
          "cancellation restores confirmed settings including frame cap");
}

void testCleanupRestoresPendingPreview() {
  FakeBackend backend;
  {
    display::DisplaySettingsManager manager(backend, initialSettings());
    require(
        manager.beginPreview(previewSettings(), Clock::time_point{}).status ==
            display::ApplyStatus::PreviewPending,
        "cleanup regression starts a preview");
  }
  require(backend.restoreCalls == 1,
          "manager cleanup restores a pending preview");
  require(backend.state.settings == initialSettings(),
          "manager cleanup leaves the confirmed backend state");
}

void testSecondPreviewStopsWhenFirstCannotRestore() {
  FakeBackend backend;
  display::DisplaySettingsManager manager(backend, initialSettings());
  require(manager.beginPreview(previewSettings(), Clock::time_point{}).status ==
              display::ApplyStatus::PreviewPending,
          "failed replacement regression starts a preview");
  backend.restoreSucceeds = false;

  auto replacement = previewSettings();
  replacement.mode = DisplayMode::BorderlessFullscreen;
  const auto result = manager.beginPreview(
      replacement, Clock::time_point{} + std::chrono::seconds(1));
  require(result.status == display::ApplyStatus::FailedUnrecoverable,
          "failed first rollback blocks the second preview");
  require(backend.applyCalls == 1,
          "second candidate is never applied after rollback failure");
  require(!manager.hasPendingPreview(),
          "unrecoverable rollback does not retain a false pending state");
}
} // namespace

int main() {
  testPreviewConfirmation();
  testTimeoutAndFocusLossRestoreImmediately();
  testApplyFailureRollsBackAndReportsRestoreFailure();
  testUnsupportedFieldsRejectWithoutBackendCalls();
  testSecondPreviewRestoresFirstAndFrameCapIsSafe();
  testCleanupRestoresPendingPreview();
  testSecondPreviewStopsWhenFirstCannotRestore();
  return 0;
}
