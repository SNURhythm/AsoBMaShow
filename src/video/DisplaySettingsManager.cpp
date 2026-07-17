#include "DisplaySettingsManager.h"

#include <algorithm>
#include <cstdio>
#include <sstream>
#include <thread>
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
  lastWorkingIntent = captureEffectiveSettings();
}

DisplaySettingsManager::~DisplaySettingsManager() {
  if (pendingPreview.has_value()) {
    const ApplyResult result = shutdown();
    if (result.status == ApplyStatus::RollbackPending) {
      std::fputs("Display rollback remained pending during manager teardown.\n",
                 stderr);
    } else if (result.status == ApplyStatus::FailedUnrecoverable) {
      std::fprintf(stderr,
                   "Display rollback failed during manager teardown: %s\n",
                   result.message.c_str());
    }
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
  return lastWorkingIntent;
}

player_settings::VideoSettings
DisplaySettingsManager::captureEffectiveSettings() const {
  auto effective = backend.capture().settings;
  effective.frameCap = frameCapRuntime.currentFrameCap();
  return effective;
}

ApplyResult DisplaySettingsManager::applySafeStartupIntent() {
  const std::uint32_t candidateCap = persistedIntent.frameCap;
  const auto effectiveBefore = captureEffectiveSettings();
  if (candidateCap != 0 && (candidateCap < 15 || candidateCap > 1000)) {
    return {.status = ApplyStatus::Unsupported,
            .effective = effectiveBefore,
            .message =
                "The persisted frame cap is outside the supported range."};
  }
  if (candidateCap != effectiveBefore.frameCap &&
      !backendCapabilities.canSetFrameCap) {
    return {.status = ApplyStatus::Unsupported,
            .effective = effectiveBefore,
            .message = "Frame limiting is not supported on this platform."};
  }

  std::string errorMessage;
  if (!applyFrameCap(candidateCap, errorMessage)) {
    return {.status = ApplyStatus::FailedRolledBack,
            .effective = captureEffectiveSettings(),
            .message = errorMessage.empty()
                           ? "Could not apply the persisted frame cap."
                           : std::move(errorMessage)};
  }
  lastWorkingIntent.frameCap = candidateCap;
  return {.status = ApplyStatus::Applied,
          .effective = captureEffectiveSettings(),
          .message = {}};
}

bool DisplaySettingsManager::displayFieldsEqual(
    const player_settings::VideoSettings &left,
    const player_settings::VideoSettings &right) {
  if (left.mode != right.mode || left.displayIndex != right.displayIndex ||
      left.vsync != right.vsync) {
    return false;
  }
  if (left.mode == player_settings::DisplayMode::BorderlessFullscreen) {
    return true;
  }
  return left.width == right.width && left.height == right.height;
}

std::optional<std::string> DisplaySettingsManager::unsupportedReason(
    const player_settings::VideoSettings &candidate,
    const player_settings::VideoSettings &effective) const {
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
      (candidate.frameCap < 1 || candidate.frameCap > 1000)) {
    return "The requested frame cap is outside the supported range.";
  }

  const bool modeChanged = candidate.mode != effective.mode;
  const bool displayChanged = candidate.displayIndex != effective.displayIndex;
  const bool resolutionChanged = candidate.width != effective.width ||
                                 candidate.height != effective.height;
  const bool vsyncChanged = candidate.vsync != effective.vsync;
  const bool frameCapChanged = candidate.frameCap != effective.frameCap;

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
  const RestoreStatus displayStatus = backend.restore(previous, displayError);
  if (displayStatus == RestoreStatus::RetryableFailure) {
    std::string message = "Display rollback is waiting for renderer access.";
    if (!displayError.empty()) {
      message += " " + displayError;
    }
    return {.status = ApplyStatus::RollbackPending,
            .effective = captureEffectiveSettings(),
            .message = std::move(message)};
  }
  if (displayStatus == RestoreStatus::Failed) {
    std::ostringstream message;
    if (!applyError.empty()) {
      message << applyError << " ";
    }
    message << "Could not restore the previous display state.";
    if (!displayError.empty()) {
      message << " " << displayError;
    }
    return {.status = ApplyStatus::FailedUnrecoverable,
            .effective = captureEffectiveSettings(),
            .message = message.str()};
  }

  std::string frameCapError;
  const bool frameCapRestored =
      applyFrameCap(previous.settings.frameCap, frameCapError);
  if (frameCapRestored) {
    std::string message =
        reason == RollbackReason::ApplyFailed && !applyError.empty()
            ? std::move(applyError)
            : rollbackMessage(reason);
    return {.status = reason == RollbackReason::ApplyFailed
                          ? ApplyStatus::FailedRolledBack
                          : ApplyStatus::Applied,
            .effective = captureEffectiveSettings(),
            .message = std::move(message)};
  }

  std::ostringstream message;
  if (!applyError.empty()) {
    message << applyError << " ";
  }
  message << "Could not restore the previous runtime state.";
  if (!frameCapRestored && !frameCapError.empty()) {
    message << " Frame cap: " << frameCapError;
  }
  return {.status = ApplyStatus::FailedUnrecoverable,
          .effective = captureEffectiveSettings(),
          .message = message.str()};
}

ApplyResult DisplaySettingsManager::beginPreview(
    const player_settings::VideoSettings &candidate,
    std::chrono::steady_clock::time_point now) {
  if (pendingPreview.has_value()) {
    ApplyResult rollback = cancelPreview(RollbackReason::Cancelled);
    if (rollback.status == ApplyStatus::RollbackPending ||
        rollback.status == ApplyStatus::FailedUnrecoverable) {
      return rollback;
    }
  }

  RuntimeState previous = backend.capture();
  previous.settings.frameCap = frameCapRuntime.currentFrameCap();
  if (const auto unsupported =
          unsupportedReason(candidate, previous.settings)) {
    return {.status = ApplyStatus::Unsupported,
            .effective = previous.settings,
            .message = *unsupported};
  }

  if (displayFieldsEqual(candidate, previous.settings)) {
    std::string frameCapError;
    if (!applyFrameCap(candidate.frameCap, frameCapError)) {
      return {.status = ApplyStatus::FailedRolledBack,
              .effective = captureEffectiveSettings(),
              .message = frameCapError.empty()
                             ? "Could not apply the requested frame cap."
                             : std::move(frameCapError)};
    }
    lastWorkingIntent = candidate;
    return {.status = ApplyStatus::Applied,
            .effective = captureEffectiveSettings(),
            .message = {}};
  }

  std::string applyError;
  if (!backend.apply(candidate, applyError)) {
    ApplyResult result =
        rollback(previous, RollbackReason::ApplyFailed, std::move(applyError));
    if (result.status == ApplyStatus::RollbackPending) {
      pendingPreview =
          PendingPreview{.previous = std::move(previous),
                         .candidate = candidate,
                         .deadline = now,
                         .rollbackReason = RollbackReason::ApplyFailed};
    }
    return result;
  }

  std::string frameCapError;
  if (!applyFrameCap(candidate.frameCap, frameCapError)) {
    ApplyResult result = rollback(previous, RollbackReason::ApplyFailed,
                                  std::move(frameCapError));
    if (result.status == ApplyStatus::RollbackPending) {
      pendingPreview =
          PendingPreview{.previous = std::move(previous),
                         .candidate = candidate,
                         .deadline = now,
                         .rollbackReason = RollbackReason::ApplyFailed};
    }
    return result;
  }

  pendingPreview = PendingPreview{.previous = std::move(previous),
                                  .candidate = candidate,
                                  .deadline = now + kConfirmationTimeout,
                                  .rollbackReason = std::nullopt};
  return {.status = ApplyStatus::PreviewPending,
          .effective = captureEffectiveSettings(),
          .message = "Confirm the display settings within 15 seconds."};
}

ApplyResult DisplaySettingsManager::confirmPreview() {
  if (!pendingPreview.has_value()) {
    return {.status = ApplyStatus::Unsupported,
            .effective = captureEffectiveSettings(),
            .message = "There is no display preview to confirm."};
  }
  if (pendingPreview->rollbackReason.has_value()) {
    return {.status = ApplyStatus::RollbackPending,
            .effective = captureEffectiveSettings(),
            .message = "This display preview is waiting to roll back."};
  }

  const auto effective = captureEffectiveSettings();
  if (!displayFieldsEqual(effective, pendingPreview->candidate) ||
      effective.frameCap != pendingPreview->candidate.frameCap) {
    return {.status = ApplyStatus::PreviewPending,
            .effective = effective,
            .message =
                "The effective display changed; restore or apply again."};
  }
  lastWorkingIntent = pendingPreview->candidate;
  pendingPreview.reset();
  return {
      .status = ApplyStatus::Applied, .effective = effective, .message = {}};
}

ApplyResult DisplaySettingsManager::cancelPreview(RollbackReason reason) {
  if (!pendingPreview.has_value()) {
    return {.status = ApplyStatus::Applied,
            .effective = captureEffectiveSettings(),
            .message = {}};
  }

  PendingPreview &pending = *pendingPreview;
  if (!pending.rollbackReason.has_value()) {
    pending.rollbackReason = reason;
  }
  ApplyResult result = rollback(pending.previous, *pending.rollbackReason);
  if (result.status != ApplyStatus::RollbackPending) {
    pendingPreview.reset();
  }
  return result;
}

ApplyResult DisplaySettingsManager::shutdown() {
  constexpr int kMaxRollbackAttempts = 16;
  ApplyResult result = cancelPreview(RollbackReason::Cancelled);
  for (int attempt = 1; attempt < kMaxRollbackAttempts &&
                        result.status == ApplyStatus::RollbackPending;
       ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    result = cancelPreview(RollbackReason::Cancelled);
  }
  return result;
}

std::optional<ApplyResult>
DisplaySettingsManager::tick(std::chrono::steady_clock::time_point now) {
  backend.observeRuntimeState();
  if (!pendingPreview.has_value()) {
    return std::nullopt;
  }
  if (!pendingPreview->rollbackReason.has_value() &&
      now < pendingPreview->deadline) {
    return std::nullopt;
  }
  return cancelPreview(
      pendingPreview->rollbackReason.value_or(RollbackReason::Timeout));
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
