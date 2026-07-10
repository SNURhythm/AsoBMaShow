#include "video/DisplaySettingsManager.h"
#include "video/FramePacer.h"

#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
  display::RestoreStatus restoreStatus = display::RestoreStatus::Restored;
  std::vector<display::RestoreStatus> restoreStatuses;
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

  display::RestoreStatus restore(const display::RuntimeState &snapshot,
                                 std::string &errorMessage) override {
    ++restoreCalls;
    if (!restoreStatuses.empty()) {
      restoreStatus = restoreStatuses.front();
      restoreStatuses.erase(restoreStatuses.begin());
    }
    if (restoreStatus != display::RestoreStatus::Restored) {
      errorMessage = "injected restore failure";
      return restoreStatus;
    }
    state = snapshot;
    return display::RestoreStatus::Restored;
  }
};

class FakeFrameCapRuntime final : public display::IFrameCapRuntime {
public:
  std::uint32_t cap = 0;
  bool applySucceeds = true;
  bool failNextApply = false;
  int applyCalls = 0;
  std::vector<std::uint32_t> appliedCaps;

  std::uint32_t currentFrameCap() const override { return cap; }

  bool applyFrameCap(std::uint32_t candidate,
                     std::string &errorMessage) override {
    ++applyCalls;
    appliedCaps.push_back(candidate);
    if (!applySucceeds || std::exchange(failNextApply, false)) {
      errorMessage = "injected frame-cap failure";
      return false;
    }
    cap = candidate;
    return true;
  }
};

void testPreviewConfirmation() {
  FakeBackend backend;
  FakeFrameCapRuntime frameCapRuntime;
  display::DisplaySettingsManager manager(backend, frameCapRuntime,
                                          initialSettings());
  const auto now = Clock::time_point{};
  const auto candidate = previewSettings();

  const auto result = manager.beginPreview(candidate, now);
  require(result.status == display::ApplyStatus::PreviewPending,
          "display changes start a preview");
  require(result.effective == candidate, "preview reports candidate state");
  require(manager.hasPendingPreview(), "preview is pending");
  require(backend.applyCalls == 1 && backend.restoreCalls == 0,
          "preview applies once without restoring");
  require(manager.confirmPreview().status == display::ApplyStatus::Applied,
          "pending preview confirms");
  require(!manager.hasPendingPreview(), "confirmation clears preview");
  require(!manager.tick(now + std::chrono::seconds(30)).has_value(),
          "confirmed preview cannot later time out");
  require(backend.state.settings == candidate,
          "confirmed display state remains active");
  require(frameCapRuntime.cap == candidate.frameCap,
          "confirmation preserves the candidate frame cap");
}

void testTimeoutAndFocusLossRestoreImmediately() {
  FakeBackend timeoutBackend;
  FakeFrameCapRuntime timeoutFrameCap;
  display::DisplaySettingsManager timeoutManager(
      timeoutBackend, timeoutFrameCap, initialSettings());
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
  require(timeoutFrameCap.cap == 0,
          "timeout restores the captured effective frame cap");

  FakeBackend focusBackend;
  FakeFrameCapRuntime focusFrameCap;
  display::DisplaySettingsManager focusManager(focusBackend, focusFrameCap,
                                               initialSettings());
  focusManager.beginPreview(previewSettings(), now);
  const auto focusResult = focusManager.onFocusLost();
  require(focusResult.has_value() &&
              focusResult->status == display::ApplyStatus::Applied,
          "focus loss exposes the completed rollback result");
  require(!focusManager.hasPendingPreview(),
          "focus loss clears the preview immediately");
  require(focusBackend.restoreCalls == 1, "focus loss restores exactly once");
  require(focusBackend.state.settings == initialSettings(),
          "focus loss restores backend state");
  require(focusFrameCap.cap == 0,
          "focus loss restores the captured effective frame cap");
}

void testApplyFailureRollsBackAndReportsRestoreFailure() {
  FakeBackend rollbackBackend;
  rollbackBackend.applySucceeds = false;
  FakeFrameCapRuntime rollbackFrameCap;
  display::DisplaySettingsManager rollbackManager(
      rollbackBackend, rollbackFrameCap, initialSettings());
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
  brokenRestoreBackend.restoreStatus = display::RestoreStatus::Failed;
  FakeFrameCapRuntime brokenRestoreFrameCap;
  display::DisplaySettingsManager brokenRestoreManager(
      brokenRestoreBackend, brokenRestoreFrameCap, initialSettings());
  const auto unrecoverable =
      brokenRestoreManager.beginPreview(previewSettings(), Clock::time_point{});
  require(unrecoverable.status == display::ApplyStatus::FailedUnrecoverable,
          "double failure is reported as unrecoverable");
  require(!unrecoverable.message.empty(),
          "double failure retains a diagnostic");
}

void testUnsupportedFieldsRejectWithoutBackendCalls() {
  FakeBackend invalidBackend;
  FakeFrameCapRuntime invalidFrameCap;
  display::DisplaySettingsManager invalidManager(
      invalidBackend, invalidFrameCap, initialSettings());
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
  require(invalidBackend.captureCalls >= 3 && invalidBackend.applyCalls == 0,
          "invalid settings recapture runtime but never mutate it");

  FakeBackend backend;
  backend.exposedCapabilities = {
      .canSetFrameCap = true,
      .displays = {{.index = 0,
                    .name = "Fixed",
                    .resolutions = {{1280, 720, 60}}}},
  };
  FakeFrameCapRuntime frameCapRuntime;
  display::DisplaySettingsManager manager(backend, frameCapRuntime,
                                          initialSettings());
  auto candidate = initialSettings();
  candidate.mode = DisplayMode::BorderlessFullscreen;

  const auto unsupported = manager.beginPreview(candidate, Clock::time_point{});
  require(unsupported.status == display::ApplyStatus::Unsupported,
          "unsupported mode is rejected");
  require(backend.captureCalls >= 2 && backend.applyCalls == 0 &&
              backend.restoreCalls == 0,
          "unsupported fields cause no backend mutation calls");

  FakeBackend unknownDisplayBackend;
  FakeFrameCapRuntime unknownDisplayFrameCap;
  display::DisplaySettingsManager unknownDisplayManager(
      unknownDisplayBackend, unknownDisplayFrameCap, initialSettings());
  candidate = previewSettings();
  candidate.displayIndex = 99;
  const auto unknownDisplay =
      unknownDisplayManager.beginPreview(candidate, Clock::time_point{});
  require(unknownDisplay.status == display::ApplyStatus::Unsupported,
          "unknown display is rejected");
  require(unknownDisplayBackend.captureCalls >= 2 &&
              unknownDisplayBackend.applyCalls == 0,
          "unknown display is rejected before capture");

  FakeBackend unknownResolutionBackend;
  FakeFrameCapRuntime unknownResolutionFrameCap;
  display::DisplaySettingsManager unknownResolutionManager(
      unknownResolutionBackend, unknownResolutionFrameCap, initialSettings());
  candidate = previewSettings();
  candidate.width = 1111;
  candidate.height = 777;
  const auto unknownResolution =
      unknownResolutionManager.beginPreview(candidate, Clock::time_point{});
  require(unknownResolution.status == display::ApplyStatus::Unsupported,
          "unknown resolution is rejected");
  require(unknownResolutionBackend.captureCalls >= 2 &&
              unknownResolutionBackend.applyCalls == 0,
          "unknown resolution is rejected before capture");

  FakeBackend exclusiveModeBackend;
  exclusiveModeBackend.exposedCapabilities.displays[0].resolutions = {
      {1920, 1080, 60}};
  FakeFrameCapRuntime exclusiveModeFrameCap;
  display::DisplaySettingsManager exclusiveModeManager(
      exclusiveModeBackend, exclusiveModeFrameCap, initialSettings());
  candidate = initialSettings();
  candidate.mode = DisplayMode::ExclusiveFullscreen;
  const auto unavailableExclusive =
      exclusiveModeManager.beginPreview(candidate, Clock::time_point{});
  require(unavailableExclusive.status == display::ApplyStatus::Unsupported,
          "exclusive mode validates its unchanged requested size");
  require(exclusiveModeBackend.captureCalls >= 2 &&
              exclusiveModeBackend.applyCalls == 0,
          "unavailable exclusive mode is rejected before capture");
}

void testSecondPreviewRestoresFirstAndFrameCapIsSafe() {
  FakeBackend backend;
  FakeFrameCapRuntime frameCapRuntime;
  display::DisplaySettingsManager manager(backend, frameCapRuntime,
                                          initialSettings());
  const auto now = Clock::time_point{};

  auto frameCapOnly = initialSettings();
  frameCapOnly.frameCap = 120;
  const auto capped = manager.beginPreview(frameCapOnly, now);
  require(capped.status == display::ApplyStatus::Applied,
          "frame-cap-only changes apply without a preview");
  require(capped.effective == frameCapOnly,
          "frame-cap-only result becomes effective");
  require(backend.captureCalls >= 3 && backend.applyCalls == 0,
          "app-owned frame cap does not touch the display backend");
  require(frameCapRuntime.cap == 120 && frameCapRuntime.applyCalls == 1,
          "frame-cap-only apply mutates the runtime immediately");

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
  require(frameCapRuntime.cap == 120,
          "cancellation restores the confirmed runtime cap");
}

void testCleanupRestoresPendingPreview() {
  FakeBackend backend;
  FakeFrameCapRuntime frameCapRuntime;
  {
    display::DisplaySettingsManager manager(backend, frameCapRuntime,
                                            initialSettings());
    require(
        manager.beginPreview(previewSettings(), Clock::time_point{}).status ==
            display::ApplyStatus::PreviewPending,
        "cleanup regression starts a preview");
  }
  require(backend.restoreCalls == 1,
          "manager cleanup restores a pending preview");
  require(backend.state.settings == initialSettings(),
          "manager cleanup leaves the confirmed backend state");
  require(frameCapRuntime.cap == 0,
          "manager cleanup restores the captured frame cap");
}

void testCleanupRetriesTransientRollback() {
  FakeBackend backend;
  backend.restoreStatuses = {display::RestoreStatus::RetryableFailure,
                             display::RestoreStatus::Restored};
  FakeFrameCapRuntime frameCapRuntime;
  {
    display::DisplaySettingsManager manager(backend, frameCapRuntime,
                                            initialSettings());
    require(
        manager.beginPreview(previewSettings(), Clock::time_point{}).status ==
            display::ApplyStatus::PreviewPending,
        "retrying cleanup regression starts a preview");
  }
  require(backend.restoreCalls == 2,
          "manager cleanup retries a transient display rollback");
  require(backend.state.settings == initialSettings() &&
              frameCapRuntime.cap == 0,
          "retrying cleanup restores the complete captured runtime");
}

void testSecondPreviewStopsWhenFirstCannotRestore() {
  FakeBackend backend;
  FakeFrameCapRuntime frameCapRuntime;
  display::DisplaySettingsManager manager(backend, frameCapRuntime,
                                          initialSettings());
  require(manager.beginPreview(previewSettings(), Clock::time_point{}).status ==
              display::ApplyStatus::PreviewPending,
          "failed replacement regression starts a preview");
  backend.restoreStatus = display::RestoreStatus::Failed;

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
  require(frameCapRuntime.cap == 120,
          "permanent display failure keeps the candidate cap coherent");
}

void testFrameCapRuntimeAcrossEveryPreviewTerminalPath() {
  const auto now = Clock::time_point{};

  FakeBackend cancelledBackend;
  FakeFrameCapRuntime cancelledCap;
  display::DisplaySettingsManager cancelledManager(
      cancelledBackend, cancelledCap, initialSettings());
  cancelledManager.beginPreview(previewSettings(), now);
  require(cancelledCap.cap == 120,
          "mixed preview applies its candidate cap immediately");
  cancelledManager.cancelPreview(display::RollbackReason::Cancelled);
  require(cancelledCap.cap == 0,
          "explicit cancellation restores the captured cap");

  FakeBackend failedBackend;
  failedBackend.applySucceeds = false;
  FakeFrameCapRuntime failedCap;
  display::DisplaySettingsManager failedManager(failedBackend, failedCap,
                                                initialSettings());
  failedManager.beginPreview(previewSettings(), now);
  require(failedCap.cap == 0,
          "display apply failure leaves the old cap effective");

  FakeBackend capFailureBackend;
  FakeFrameCapRuntime capFailure;
  display::DisplaySettingsManager capFailureManager(
      capFailureBackend, capFailure, initialSettings());
  capFailure.failNextApply = true;
  const auto capFailureResult =
      capFailureManager.beginPreview(previewSettings(), now);
  require(capFailureResult.status == display::ApplyStatus::FailedRolledBack,
          "frame-cap failure rolls a mixed display candidate back");
  require(capFailureBackend.restoreCalls == 1 &&
              capFailureBackend.state.settings == initialSettings(),
          "frame-cap failure restores the display snapshot");

  FakeBackend replacementBackend;
  FakeFrameCapRuntime replacementCap;
  display::DisplaySettingsManager replacementManager(
      replacementBackend, replacementCap, initialSettings());
  replacementManager.beginPreview(previewSettings(), now);
  auto replacement = previewSettings();
  replacement.mode = DisplayMode::BorderlessFullscreen;
  replacement.frameCap = 240;
  replacementManager.beginPreview(replacement, now + std::chrono::seconds(1));
  require(replacementCap.appliedCaps == std::vector<std::uint32_t>{120, 0, 240},
          "second preview restores the first cap before applying its own");
  replacementManager.cancelPreview(display::RollbackReason::Cancelled);
  require(replacementCap.cap == 0,
          "second preview cancellation restores the original captured cap");
}

void testPersistedIntentRemainsSeparateFromCapturedRuntime() {
  FakeBackend backend;
  backend.state.settings = initialSettings();
  FakeFrameCapRuntime frameCapRuntime;
  frameCapRuntime.cap = 0;
  auto persisted = previewSettings();
  display::DisplaySettingsManager manager(backend, frameCapRuntime, persisted);

  require(manager.configuredIntent() == persisted,
          "manager preserves the persisted display intent");
  require(manager.lastWorkingSettings() == initialSettings(),
          "last-working state comes from captured runtime, not intent");
  const auto startup = manager.applySafeStartupIntent();
  require(startup.status == display::ApplyStatus::Applied &&
              frameCapRuntime.cap == persisted.frameCap,
          "startup applies only the safe app-owned frame cap");
  require(backend.applyCalls == 0,
          "startup does not risk an unconfirmed display mutation");

  const auto result =
      manager.beginPreview(manager.configuredIntent(), Clock::time_point{});
  require(result.status == display::ApplyStatus::PreviewPending &&
              backend.applyCalls == 1,
          "equal-to-persisted Apply still mutates differing runtime");
}

void testUnsupportedPersistedDisplayIntentStaysPendingSafely() {
  FakeBackend backend;
  FakeFrameCapRuntime frameCapRuntime;
  auto persisted = previewSettings();
  persisted.displayIndex = 99;
  display::DisplaySettingsManager manager(backend, frameCapRuntime, persisted);

  const auto startup = manager.applySafeStartupIntent();
  require(startup.status == display::ApplyStatus::Applied &&
              frameCapRuntime.cap == persisted.frameCap,
          "safe startup cap is independent of an unavailable display intent");
  require(manager.beginPreview(manager.configuredIntent(), Clock::time_point{})
                  .status == display::ApplyStatus::Unsupported,
          "unavailable persisted display intent remains pending and explicit");
  require(backend.applyCalls == 0,
          "unsupported persisted display intent never mutates runtime");

  FakeBackend failedCapBackend;
  FakeFrameCapRuntime failedCapRuntime;
  failedCapRuntime.failNextApply = true;
  display::DisplaySettingsManager failedCapManager(
      failedCapBackend, failedCapRuntime, previewSettings());
  require(failedCapManager.applySafeStartupIntent().status ==
              display::ApplyStatus::FailedRolledBack,
          "startup frame-cap failure is surfaced without changing display");
  require(failedCapRuntime.cap == 0 && failedCapBackend.applyCalls == 0,
          "failed safe startup retains captured effective runtime");
}

void testRealFramePacerIsTheManagedFrameCapRuntime() {
  FakeBackend backend;
  FramePacer pacer;
  const auto now = Clock::time_point{};
  pacer.setCap(50);
  pacer.reset(now);
  pacer.framePresented(now);
  require(pacer.remaining(now) == std::chrono::milliseconds(20),
          "test starts with a 50 FPS runtime deadline");

  auto captured = initialSettings();
  captured.frameCap = 50;
  backend.state.settings = captured;
  display::DisplaySettingsManager manager(backend, pacer, captured);
  auto candidate = captured;
  candidate.frameCap = 100;
  require(manager.beginPreview(candidate, now).status ==
              display::ApplyStatus::Applied,
          "cap-only manager apply succeeds against the real pacer");
  pacer.framePresented(now);
  require(pacer.remaining(now) == std::chrono::milliseconds(10),
          "managed cap changes the real pacer's next deadline");
}

void testExternalResizeIsRecapturedBeforeApply() {
  FakeBackend backend;
  FakeFrameCapRuntime frameCapRuntime;
  display::DisplaySettingsManager manager(backend, frameCapRuntime,
                                          initialSettings());
  auto externallyResized = initialSettings();
  externallyResized.width = 1600;
  externallyResized.height = 900;
  backend.state.settings = externallyResized;

  const auto result =
      manager.beginPreview(initialSettings(), Clock::time_point{});
  require(result.status == display::ApplyStatus::PreviewPending,
          "stale accepted size cannot make an external resize a false no-op");
  require(backend.applyCalls == 1,
          "Apply mutates runtime after an external resize");
  require(manager.configuredIntent() == initialSettings(),
          "runtime recapture does not overwrite persisted intent");
}

void testConfirmationRecapturesRuntimeAndRejectsDrift() {
  FakeBackend backend;
  FakeFrameCapRuntime frameCapRuntime;
  display::DisplaySettingsManager manager(backend, frameCapRuntime,
                                          initialSettings());
  const auto candidate = previewSettings();
  manager.beginPreview(candidate, Clock::time_point{});

  auto drifted = candidate;
  drifted.width = 1600;
  drifted.height = 900;
  backend.state.settings = drifted;
  const auto keep = manager.confirmPreview();
  require(keep.status == display::ApplyStatus::PreviewPending,
          "Keep cannot confirm a candidate that is no longer effective");
  require(keep.effective == drifted,
          "failed Keep reports authoritative drifted runtime");
  require(manager.hasPendingPreview(),
          "drifted preview remains pending for safe rollback");
  require(
      manager.lastWorkingSettings() == initialSettings(),
      "failed Keep does not promote drifted runtime to last-working intent");

  const auto cancelled =
      manager.cancelPreview(display::RollbackReason::Cancelled);
  require(cancelled.status == display::ApplyStatus::Applied &&
              cancelled.effective == initialSettings(),
          "drifted preview cancellation reports the restored snapshot");
}

void testTransientRollbackRemainsPendingAndRetries() {
  const auto now = Clock::time_point{};

  FakeBackend timeoutBackend;
  FakeFrameCapRuntime timeoutCap;
  display::DisplaySettingsManager timeoutManager(timeoutBackend, timeoutCap,
                                                 initialSettings());
  timeoutManager.beginPreview(previewSettings(), now);
  timeoutBackend.restoreStatus = display::RestoreStatus::RetryableFailure;
  const auto deferredTimeout = timeoutManager.tick(
      now + display::DisplaySettingsManager::kConfirmationTimeout);
  require(deferredTimeout.has_value() &&
              deferredTimeout->status == display::ApplyStatus::RollbackPending,
          "timeout reports a transient rollback without discarding it");
  require(timeoutManager.hasPendingPreview() && timeoutCap.cap == 120,
          "deferred timeout keeps candidate display and cap coherent");
  timeoutBackend.restoreStatus = display::RestoreStatus::Restored;
  const auto retriedTimeout = timeoutManager.tick(
      now + display::DisplaySettingsManager::kConfirmationTimeout +
      std::chrono::seconds(1));
  require(retriedTimeout.has_value() &&
              retriedTimeout->status == display::ApplyStatus::Applied &&
              !timeoutManager.hasPendingPreview() && timeoutCap.cap == 0,
          "timeout retries and completes after renderer access returns");

  FakeBackend focusBackend;
  FakeFrameCapRuntime focusCap;
  display::DisplaySettingsManager focusManager(focusBackend, focusCap,
                                               initialSettings());
  focusManager.beginPreview(previewSettings(), now);
  focusBackend.restoreStatus = display::RestoreStatus::RetryableFailure;
  const auto deferredFocus = focusManager.onFocusLost();
  require(deferredFocus.has_value() &&
              deferredFocus->status == display::ApplyStatus::RollbackPending &&
              focusManager.hasPendingPreview() && focusCap.cap == 120,
          "focus rollback remains pending while renderer access is transient");
  focusBackend.restoreStatus = display::RestoreStatus::Restored;
  require(
      focusManager.cancelPreview(display::RollbackReason::FocusLost).status ==
          display::ApplyStatus::Applied,
      "focus rollback can be retried explicitly");

  FakeBackend cancelBackend;
  FakeFrameCapRuntime cancelCap;
  display::DisplaySettingsManager cancelManager(cancelBackend, cancelCap,
                                                initialSettings());
  cancelManager.beginPreview(previewSettings(), now);
  cancelBackend.restoreStatus = display::RestoreStatus::RetryableFailure;
  require(
      cancelManager.cancelPreview(display::RollbackReason::Cancelled).status ==
              display::ApplyStatus::RollbackPending &&
          cancelManager.hasPendingPreview() && cancelCap.cap == 120,
      "explicit cancel remains pending after a transient failure");
  cancelBackend.restoreStatus = display::RestoreStatus::Restored;
  require(
      cancelManager.cancelPreview(display::RollbackReason::Cancelled).status ==
          display::ApplyStatus::Applied,
      "explicit cancel succeeds on retry");
}

void testRuntimeDriftBeforeRollbackReportsRestoredSnapshot() {
  FakeBackend backend;
  FakeFrameCapRuntime frameCapRuntime;
  display::DisplaySettingsManager manager(backend, frameCapRuntime,
                                          initialSettings());
  const auto now = Clock::time_point{};
  manager.beginPreview(previewSettings(), now);
  auto drifted = previewSettings();
  drifted.width = 1600;
  drifted.height = 900;
  backend.state.settings = drifted;

  const auto timeout =
      manager.tick(now + display::DisplaySettingsManager::kConfirmationTimeout);
  require(timeout.has_value() && timeout->effective == initialSettings(),
          "timeout after runtime drift reports authoritative restored state");
}
} // namespace

int main() {
  testPreviewConfirmation();
  testTimeoutAndFocusLossRestoreImmediately();
  testApplyFailureRollsBackAndReportsRestoreFailure();
  testUnsupportedFieldsRejectWithoutBackendCalls();
  testSecondPreviewRestoresFirstAndFrameCapIsSafe();
  testCleanupRestoresPendingPreview();
  testCleanupRetriesTransientRollback();
  testSecondPreviewStopsWhenFirstCannotRestore();
  testFrameCapRuntimeAcrossEveryPreviewTerminalPath();
  testPersistedIntentRemainsSeparateFromCapturedRuntime();
  testUnsupportedPersistedDisplayIntentStaysPendingSafely();
  testRealFramePacerIsTheManagedFrameCapRuntime();
  testExternalResizeIsRecapturedBeforeApply();
  testConfirmationRecapturesRuntimeAndRejectsDrift();
  testTransientRollbackRemainsPendingAndRetries();
  testRuntimeDriftBeforeRollbackReportsRestoredSnapshot();
  return 0;
}
