#include "ProfileRuntimeReapply.h"
#include "../i18n/Localization.h"

#include <exception>
#include <string_view>
#include <utility>

namespace {
struct FailureMessage {
  const char *message;
  const char *detail;
};
void appendException(std::vector<std::string> &warnings,
                     FailureMessage operation,
                     const std::exception *error = nullptr) {
  if (error != nullptr && error->what()[0] != '\0') {
    warnings.push_back(i18n::format(operation.detail, {{"detail", error->what()}}));
  } else {
    warnings.push_back(i18n::tr(operation.message));
  }
}

template <typename Callback>
void invokeVoid(const Callback &callback, FailureMessage operation,
                std::vector<std::string> &warnings) {
  if (!callback) {
    appendException(warnings, operation);
    return;
  }
  try {
    callback();
  } catch (const std::exception &error) {
    appendException(warnings, operation, &error);
  } catch (...) {
    appendException(warnings, operation);
  }
}

template <typename Callback>
void invokeWarning(const Callback &callback, FailureMessage operation,
                   std::vector<std::string> &warnings) {
  if (!callback) {
    appendException(warnings, operation);
    return;
  }
  try {
    std::string warning = callback();
    if (!warning.empty()) {
      warnings.push_back(std::move(warning));
    }
  } catch (const std::exception &error) {
    appendException(warnings, operation, &error);
  } catch (...) {
    appendException(warnings, operation);
  }
}
} // namespace

ProfileRuntimeReapplyResult ReapplyProfileRuntimeAfterSwitch(
    const ProfileSwitchResult &switchResult,
    const ProfileRuntimeReapplyCallbacks &callbacks) {
  ProfileRuntimeReapplyResult result;
  if (!switchResult.ok()) {
    return result;
  }
  result.profileCommitted = true;

  invokeVoid(callbacks.sanitize, {"settings.profiles.runtime.sanitize_failed", "settings.profiles.runtime.sanitize_failed_detail"}, result.warnings);
  invokeVoid(callbacks.applyTheme, {"settings.profiles.runtime.theme_failed", "settings.profiles.runtime.theme_failed_detail"}, result.warnings);
  invokeVoid(callbacks.applyJukebox, {"settings.profiles.runtime.jukebox_failed", "settings.profiles.runtime.jukebox_failed_detail"}, result.warnings);
  invokeWarning(callbacks.applyMetadata, {"settings.profiles.runtime.metadata_failed", "settings.profiles.runtime.metadata_failed_detail"},
                result.warnings);
  invokeWarning(callbacks.applyAudio, {"settings.profiles.runtime.audio_failed", "settings.profiles.runtime.audio_failed_detail"}, result.warnings);
  invokeVoid(callbacks.refreshDrafts, {"settings.profiles.runtime.drafts_failed", "settings.profiles.runtime.drafts_failed_detail"},
             result.warnings);

  if (!callbacks.applyDisplay) {
    appendException(result.warnings, {"settings.profiles.runtime.display_failed", "settings.profiles.runtime.display_failed_detail"});
    return result;
  }
  try {
    const ProfileDisplayRuntimeResult display = callbacks.applyDisplay();
    if (display.outcome == ProfileDisplayRuntimeOutcome::Failed) {
      result.warnings.push_back(display.message.empty()
                                    ? i18n::tr("settings.profiles.runtime.display_failed")
                                    : display.message);
    }
  } catch (const std::exception &error) {
    appendException(result.warnings, {"settings.profiles.runtime.display_failed", "settings.profiles.runtime.display_failed_detail"}, &error);
  } catch (...) {
    appendException(result.warnings, {"settings.profiles.runtime.display_failed", "settings.profiles.runtime.display_failed_detail"});
  }
  return result;
}
