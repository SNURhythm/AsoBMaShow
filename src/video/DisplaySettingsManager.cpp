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
    IDisplayBackend &backendValue, IFrameCapRuntime &frameCapRuntimeValue,
    player_settings::VideoSettings configuredIntentValue)
    : backend(backendValue), frameCapRuntime(frameCapRuntimeValue),
      backendCapabilities(backend.capabilities()),
      persistedIntent(std::move(configuredIntentValue)) {
  confirmedSettings = backend.capture().settings;
  confirmedSettings.frameCap = frameCapRuntime.currentFrameCap();
}

DisplaySettingsManager::~DisplaySettingsManager() {
  if (pendingPreview.has_value()) {
    (void)cancelPreview(RollbackReason::Cancelled);
  }
}

Capabilities DisplaySettingsManager::capabilities() const {
  return backendCapabilities;
}

const player_settings::VideoSettings &
DisplaySettingsManager::configuredIntent() const {
  return persistedIntent;
}

const player_settings::VideoSettings &
DisplaySettingsManager::lastWorkingSettings() const {
  return confirmedSettings;
}

ApplyResult DisplaySettingsManager::applySafeStartupIntent() {
  const std::uint32_t candidateCap = persistedIntent.frameCap;
  if (candidateCap != 0 && (candidateCap < 15 || candidateCap > 1000)) {
    return {.status = ApplyStatus::Unsupported,
            .effective = confirmedSettings,
            .message =
                "The persisted frame cap is outside the supported range."};
  }
  if (candidateCap != confirmedSettings.frameCap &&
      !backendCapabilities.canSetFrameCap) {
    return {.status = ApplyStatus::Unsupported,
            .effective = confirmedSettings,
            .message = "Frame limiting is not supported on this platform."};
  }

  std::string errorMessage;
  if (!applyFrameCap(candidateCap, errorMessage)) {
    return {.status = ApplyStatus::FailedRolledBack,
            .effective = confirmedSettings,
            .message = errorMessage.empty()
                           ? "Could not apply the persisted frame cap."
                           : std::move(errorMessage)};
  }
  confirmedSettings.frameCap = candidateCap;
  return {.status = ApplyStatus::Applied,
          .effective = confirmedSettings,
          .message = {}};
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
  switch (candidate.mode) {
  case player_settings::DisplayMode::Windowed:
  case player_settings::DisplayMode::BorderlessFullscreen:
  case player_settings::DisplayMode::ExclusiveFullscreen:
    break;
  default:
    return "The requested display mode is invalid.";
  }
  if (candidate.displayIndex < 0) {
    return "The requested display index is invalid.";
  }
  if (candidate.width <= 0 || candidate.height <= 0) {
    return "The requested display size is invalid.";
  }
  if (candidate.frameCap != 0 &&
      (candidate.frameCap < 15 || candidate.frameCap > 1000)) {
    return "The requested frame cap is outside the supported range.";
  }

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

  if (modeChanged || displayChanged || resolutionChanged) {
    const DisplayInfo *display =
        findDisplay(backendCapabilities, candidate.displayIndex);
    if (display == nullptr) {
      return "The requested display is unavailable.";
    }
    const bool exclusiveModeNeedsMatch =
        candidate.mode == player_settings::DisplayMode::ExclusiveFullscreen &&
        (modeChanged || displayChanged);
    if ((resolutionChanged || exclusiveModeNeedsMatch) &&
        !hasResolution(*display, candidate.width, candidate.height)) {
      return "The requested resolution is unavailable on that display.";
    }
  }
  return std::nullopt;
}

bool DisplaySettingsManager::applyFrameCap(std::uint32_t candidate,
                                           std::string &errorMessage) {
  if (frameCapRuntime.currentFrameCap() == candidate) {
    return true;
  }
  return frameCapRuntime.applyFrameCap(candidate, errorMessage);
}

ApplyResult DisplaySettingsManager::rollback(const RuntimeState &previous,
                                             RollbackReason reason,
                                             std::string applyError) {
  std::string displayError;
  const bool displayRestored = backend.restore(previous, displayError);
  std::string frameCapError;
  const bool frameCapRestored =
      applyFrameCap(previous.settings.frameCap, frameCapError);
  if (displayRestored && frameCapRestored) {
    std::string message =
        reason == RollbackReason::ApplyFailed && !applyError.empty()
            ? std::move(applyError)
            : rollbackMessage(reason);
    return {.status = reason == RollbackReason::ApplyFailed
                          ? ApplyStatus::FailedRolledBack
                          : ApplyStatus::Applied,
            .effective = confirmedSettings,
            .message = std::move(message)};
  }

  std::ostringstream message;
  if (!applyError.empty()) {
    message << applyError << " ";
  }
  message << "Could not restore the previous runtime state.";
  if (!displayRestored && !displayError.empty()) {
    message << " Display: " << displayError;
  }
  if (!frameCapRestored && !frameCapError.empty()) {
    message << " Frame cap: " << frameCapError;
  }
  auto effective = backend.capture().settings;
  effective.frameCap = frameCapRuntime.currentFrameCap();
  return {.status = ApplyStatus::FailedUnrecoverable,
          .effective = std::move(effective),
          .message = message.str()};
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
    std::string frameCapError;
    if (!applyFrameCap(candidate.frameCap, frameCapError)) {
      return {.status = ApplyStatus::FailedRolledBack,
              .effective = confirmedSettings,
              .message = frameCapError.empty()
                             ? "Could not apply the requested frame cap."
                             : std::move(frameCapError)};
    }
    confirmedSettings = candidate;
    return {.status = ApplyStatus::Applied,
            .effective = confirmedSettings,
            .message = {}};
  }

  RuntimeState previous = backend.capture();
  previous.settings.frameCap = frameCapRuntime.currentFrameCap();
  std::string applyError;
  if (!backend.apply(candidate, applyError)) {
    return rollback(previous, RollbackReason::ApplyFailed,
                    std::move(applyError));
  }

  std::string frameCapError;
  if (!applyFrameCap(candidate.frameCap, frameCapError)) {
    return rollback(previous, RollbackReason::ApplyFailed,
                    std::move(frameCapError));
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
  return rollback(pending.previous, reason);
}

std::optional<ApplyResult>
DisplaySettingsManager::tick(std::chrono::steady_clock::time_point now) {
  if (!pendingPreview.has_value() || now < pendingPreview->deadline) {
    return std::nullopt;
  }
  return cancelPreview(RollbackReason::Timeout);
}

std::optional<ApplyResult> DisplaySettingsManager::onFocusLost() {
  if (!pendingPreview.has_value()) {
    return std::nullopt;
  }
  return cancelPreview(RollbackReason::FocusLost);
}

bool DisplaySettingsManager::hasPendingPreview() const {
  return pendingPreview.has_value();
}
} // namespace display
