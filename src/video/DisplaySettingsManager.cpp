#include "DisplaySettingsManager.h"

#include <algorithm>
#include <sstream>
#include <utility>

namespace display {
namespace {
const DisplayInfo *findDisplay(const Capabilities &capabilities, int index) {
  const auto found = std::find_if(
      capabilities.displays.begin(), capabilities.displays.end(),
      [index](const DisplayInfo &display) { return display.index == index; });
  return found == capabilities.displays.end() ? nullptr : &*found;
}

bool hasResolution(const DisplayInfo &display, int width, int height) {
  return std::ranges::any_of(
      display.resolutions, [width, height](const Resolution &resolution) {
        return resolution.width == width && resolution.height == height;
      });
}

std::string rollbackMessage(RollbackReason reason) {
  switch (reason) {
  case RollbackReason::Timeout:
    return "Display preview timed out and was restored.";
  case RollbackReason::FocusLost:
    return "Display preview was restored after focus loss.";
  case RollbackReason::Cancelled:
    return "Display preview was cancelled and restored.";
  case RollbackReason::ApplyFailed:
    return "Display apply failed and the previous state was restored.";
  }
  return "Display preview was restored.";
}
} // namespace

DisplaySettingsManager::DisplaySettingsManager(
    IDisplayBackend &backendValue,
    player_settings::VideoSettings initialSettings)
    : backend(backendValue), backendCapabilities(backend.capabilities()),
      confirmedSettings(std::move(initialSettings)) {}

DisplaySettingsManager::~DisplaySettingsManager() {
  if (pendingPreview.has_value()) {
    (void)cancelPreview(RollbackReason::Cancelled);
  }
}

Capabilities DisplaySettingsManager::capabilities() const {
  return backendCapabilities;
}

bool DisplaySettingsManager::displayFieldsEqual(
    const player_settings::VideoSettings &left,
    const player_settings::VideoSettings &right) {
  return left.mode == right.mode && left.displayIndex == right.displayIndex &&
         left.width == right.width && left.height == right.height &&
         left.vsync == right.vsync;
}

std::optional<std::string> DisplaySettingsManager::unsupportedReason(
    const player_settings::VideoSettings &candidate) const {
  const bool modeChanged = candidate.mode != confirmedSettings.mode;
  const bool displayChanged =
      candidate.displayIndex != confirmedSettings.displayIndex;
  const bool resolutionChanged = candidate.width != confirmedSettings.width ||
                                 candidate.height != confirmedSettings.height;
  const bool vsyncChanged = candidate.vsync != confirmedSettings.vsync;
  const bool frameCapChanged = candidate.frameCap != confirmedSettings.frameCap;

  if (modeChanged && !backendCapabilities.canChangeMode) {
    return "Display mode changes are not supported on this platform.";
  }
  if (displayChanged && !backendCapabilities.canSelectDisplay) {
    return "Display selection is not supported on this platform.";
  }
  if (resolutionChanged && !backendCapabilities.canSelectResolution) {
    return "Resolution selection is not supported on this platform.";
  }
  if (vsyncChanged && !backendCapabilities.canChangeVsync) {
    return "VSync changes are not supported on this platform.";
  }
  if (frameCapChanged && !backendCapabilities.canSetFrameCap) {
    return "Frame limiting is not supported on this platform.";
  }

  if (displayChanged || resolutionChanged) {
    const DisplayInfo *display =
        findDisplay(backendCapabilities, candidate.displayIndex);
    if (display == nullptr) {
      return "The requested display is unavailable.";
    }
    if (resolutionChanged &&
        !hasResolution(*display, candidate.width, candidate.height)) {
      return "The requested resolution is unavailable on that display.";
    }
  }
  return std::nullopt;
}

ApplyResult DisplaySettingsManager::beginPreview(
    const player_settings::VideoSettings &candidate,
    std::chrono::steady_clock::time_point now) {
  if (pendingPreview.has_value()) {
    ApplyResult rollback = cancelPreview(RollbackReason::Cancelled);
    if (rollback.status == ApplyStatus::FailedUnrecoverable) {
      return rollback;
    }
  }

  if (const auto unsupported = unsupportedReason(candidate)) {
    return {.status = ApplyStatus::Unsupported,
            .effective = confirmedSettings,
            .message = *unsupported};
  }

  if (displayFieldsEqual(candidate, confirmedSettings)) {
    confirmedSettings = candidate;
    return {.status = ApplyStatus::Applied,
            .effective = confirmedSettings,
            .message = {}};
  }

  RuntimeState previous = backend.capture();
  previous.settings.frameCap = confirmedSettings.frameCap;
  std::string applyError;
  if (!backend.apply(candidate, applyError)) {
    std::string restoreError;
    if (backend.restore(previous, restoreError)) {
      return {.status = ApplyStatus::FailedRolledBack,
              .effective = confirmedSettings,
              .message = applyError.empty()
                             ? rollbackMessage(RollbackReason::ApplyFailed)
                             : applyError};
    }

    std::ostringstream message;
    message << (applyError.empty() ? "Display apply failed." : applyError)
            << " Restoration also failed";
    if (!restoreError.empty()) {
      message << ": " << restoreError;
    }
    return {.status = ApplyStatus::FailedUnrecoverable,
            .effective = candidate,
            .message = message.str()};
  }

  pendingPreview = PendingPreview{.previous = std::move(previous),
                                  .candidate = candidate,
                                  .deadline = now + kConfirmationTimeout};
  return {.status = ApplyStatus::PreviewPending,
          .effective = candidate,
          .message = "Confirm the display settings within 15 seconds."};
}

bool DisplaySettingsManager::confirmPreview() {
  if (!pendingPreview.has_value()) {
    return false;
  }
  confirmedSettings = pendingPreview->candidate;
  pendingPreview.reset();
  return true;
}

ApplyResult DisplaySettingsManager::cancelPreview(RollbackReason reason) {
  if (!pendingPreview.has_value()) {
    return {.status = ApplyStatus::Applied,
            .effective = confirmedSettings,
            .message = {}};
  }

  PendingPreview pending = std::move(*pendingPreview);
  pendingPreview.reset();
  std::string errorMessage;
  if (backend.restore(pending.previous, errorMessage)) {
    return {.status = ApplyStatus::Applied,
            .effective = confirmedSettings,
            .message = rollbackMessage(reason)};
  }

  std::string message = "Could not restore the previous display state.";
  if (!errorMessage.empty()) {
    message += " " + errorMessage;
  }
  return {.status = ApplyStatus::FailedUnrecoverable,
          .effective = pending.candidate,
          .message = std::move(message)};
}

std::optional<ApplyResult>
DisplaySettingsManager::tick(std::chrono::steady_clock::time_point now) {
  if (!pendingPreview.has_value() || now < pendingPreview->deadline) {
    return std::nullopt;
  }
  return cancelPreview(RollbackReason::Timeout);
}

void DisplaySettingsManager::onFocusLost() {
  if (pendingPreview.has_value()) {
    (void)cancelPreview(RollbackReason::FocusLost);
  }
}

bool DisplaySettingsManager::hasPendingPreview() const {
  return pendingPreview.has_value();
}
} // namespace display
