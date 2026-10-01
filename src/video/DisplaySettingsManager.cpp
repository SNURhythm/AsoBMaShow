#include "DisplaySettingsManager.h"

#include <algorithm>
#include <cstdio>
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

i18n::Text rollbackMessage(RollbackReason reason) {
  switch (reason) {
  case RollbackReason::Timeout:
    return i18n::message("settings.audio_video.display.preview_timeout");
  case RollbackReason::FocusLost:
    return i18n::message("settings.audio_video.display.preview_focus_lost");
  case RollbackReason::Cancelled:
    return i18n::message("settings.audio_video.display.preview_cancelled");
  case RollbackReason::ApplyFailed:
    return i18n::message("settings.audio_video.display.apply_failed_restored");
  }
  return i18n::message("settings.audio_video.display.preview_restored");
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
                   result.message.resolve().c_str());
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
  if (candidateCap != 0 && (candidateCap < 1 || candidateCap > 1000)) {
    return {.status = ApplyStatus::Unsupported,
            .effective = effectiveBefore,
            .message =
                i18n::message("settings.audio_video.display.persisted_cap_invalid")};
  }
  if (candidateCap != effectiveBefore.frameCap &&
      !backendCapabilities.canSetFrameCap) {
    return {.status = ApplyStatus::Unsupported,
            .effective = effectiveBefore,
            .message = i18n::message("settings.audio_video.display.cap_unsupported")};
  }

  std::string errorMessage;
  if (!applyFrameCap(candidateCap, errorMessage)) {
    return {.status = ApplyStatus::FailedRolledBack,
            .effective = captureEffectiveSettings(),
            .message = errorMessage.empty()
                           ? i18n::message("settings.audio_video.display.persisted_cap_failed")
                           : i18n::Text(std::move(errorMessage))};
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

std::optional<i18n::Text> DisplaySettingsManager::unsupportedReason(
    const player_settings::VideoSettings &candidate,
    const player_settings::VideoSettings &effective) const {
  switch (candidate.mode) {
  case player_settings::DisplayMode::Windowed:
  case player_settings::DisplayMode::BorderlessFullscreen:
  case player_settings::DisplayMode::ExclusiveFullscreen:
    break;
  default:
    return i18n::message("settings.audio_video.display.mode_invalid");
  }
  if (candidate.displayIndex < 0) {
    return i18n::message("settings.audio_video.display.index_invalid");
  }
  if (candidate.width <= 0 || candidate.height <= 0) {
    return i18n::message("settings.audio_video.display.size_invalid");
  }
  if (candidate.frameCap != 0 &&
      (candidate.frameCap < 1 || candidate.frameCap > 1000)) {
    return i18n::message("settings.audio_video.display.cap_invalid");
  }

  const bool modeChanged = candidate.mode != effective.mode;
  const bool displayChanged = candidate.displayIndex != effective.displayIndex;
  const bool resolutionChanged = candidate.width != effective.width ||
                                 candidate.height != effective.height;
  const bool vsyncChanged = candidate.vsync != effective.vsync;
  const bool frameCapChanged = candidate.frameCap != effective.frameCap;

  if (modeChanged && !backendCapabilities.canChangeMode) {
    return i18n::message("settings.audio_video.display.mode_unsupported");
  }
  if (displayChanged && !backendCapabilities.canSelectDisplay) {
    return i18n::message("settings.audio_video.display.selection_unsupported");
  }
  if (resolutionChanged && !backendCapabilities.canSelectResolution) {
    return i18n::message("settings.audio_video.display.resolution_unsupported");
  }
  if (vsyncChanged && !backendCapabilities.canChangeVsync) {
    return i18n::message("settings.audio_video.display.vsync_unsupported");
  }
  if (frameCapChanged && !backendCapabilities.canSetFrameCap) {
    return i18n::message("settings.audio_video.display.cap_unsupported");
  }

  if (modeChanged || displayChanged || resolutionChanged) {
    const DisplayInfo *display =
        findDisplay(backendCapabilities, candidate.displayIndex);
    if (display == nullptr) {
      return i18n::message("settings.audio_video.display.unavailable");
    }
    const bool exclusiveModeNeedsMatch =
        candidate.mode == player_settings::DisplayMode::ExclusiveFullscreen &&
        (modeChanged || displayChanged);
    if ((resolutionChanged || exclusiveModeNeedsMatch) &&
        !hasResolution(*display, candidate.width, candidate.height)) {
      return i18n::message("settings.audio_video.display.resolution_unavailable");
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
    i18n::Text message = i18n::message("settings.audio_video.display.rollback_waiting");
    if (!displayError.empty()) {
      message = i18n::message("settings.audio_video.status_detail",
                              {{"status", message}, {"detail", displayError}});
    }
    return {.status = ApplyStatus::RollbackPending,
            .effective = captureEffectiveSettings(),
            .message = std::move(message)};
  }
  if (displayStatus == RestoreStatus::Failed) {
    i18n::Text message = i18n::message("settings.audio_video.display.restore_failed");
    if (!applyError.empty()) {
      message = i18n::message("settings.audio_video.status_detail",
                              {{"status", applyError}, {"detail", message}});
    }
    if (!displayError.empty()) {
      message = i18n::message("settings.audio_video.status_detail",
                              {{"status", message}, {"detail", displayError}});
    }
    return {.status = ApplyStatus::FailedUnrecoverable,
            .effective = captureEffectiveSettings(),
            .message = std::move(message)};
  }

  std::string frameCapError;
  const bool frameCapRestored =
      applyFrameCap(previous.settings.frameCap, frameCapError);
  if (frameCapRestored) {
    i18n::Text message =
        reason == RollbackReason::ApplyFailed && !applyError.empty()
            ? i18n::Text(std::move(applyError))
            : rollbackMessage(reason);
    return {.status = reason == RollbackReason::ApplyFailed
                          ? ApplyStatus::FailedRolledBack
                          : ApplyStatus::Applied,
            .effective = captureEffectiveSettings(),
            .message = std::move(message)};
  }

  i18n::Text message = i18n::message("settings.audio_video.display.runtime_restore_failed");
  if (!applyError.empty()) {
    message = i18n::message("settings.audio_video.status_detail",
                            {{"status", applyError}, {"detail", message}});
  }
  if (!frameCapError.empty()) {
    message = i18n::message("settings.audio_video.status_detail",
        {{"status", message},
         {"detail", i18n::message("settings.audio_video.display.frame_cap_detail",
                                  {{"detail", frameCapError}})}});
  }
  return {.status = ApplyStatus::FailedUnrecoverable,
          .effective = captureEffectiveSettings(),
          .message = std::move(message)};
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
                             ? i18n::message("settings.audio_video.display.cap_failed")
                             : i18n::Text(std::move(frameCapError))};
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
          .message = i18n::message("settings.audio_video.confirm_within_15_seconds.message")};
}

ApplyResult DisplaySettingsManager::confirmPreview() {
  if (!pendingPreview.has_value()) {
    return {.status = ApplyStatus::Unsupported,
            .effective = captureEffectiveSettings(),
            .message = i18n::message("settings.audio_video.display.no_preview")};
  }
  if (pendingPreview->rollbackReason.has_value()) {
    return {.status = ApplyStatus::RollbackPending,
            .effective = captureEffectiveSettings(),
            .message = i18n::message("settings.audio_video.display.preview_rollback_waiting")};
  }

  const auto effective = captureEffectiveSettings();
  if (!displayFieldsEqual(effective, pendingPreview->candidate) ||
      effective.frameCap != pendingPreview->candidate.frameCap) {
    return {.status = ApplyStatus::PreviewPending,
            .effective = effective,
            .message =
                i18n::message("settings.audio_video.display.runtime_changed")};
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
