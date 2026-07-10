#include "ProfileRuntimeReapply.h"

#include <exception>
#include <string_view>
#include <utility>

namespace {
void appendException(std::vector<std::string> &warnings,
                     std::string_view operation,
                     const std::exception *error = nullptr) {
  std::string warning(operation);
  warning += " failed";
  if (error != nullptr && error->what()[0] != '\0') {
    warning += ": ";
    warning += error->what();
  } else {
    warning += ".";
  }
  warnings.push_back(std::move(warning));
}

template <typename Callback>
void invokeVoid(const Callback &callback, std::string_view operation,
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
void invokeWarning(const Callback &callback, std::string_view operation,
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

  invokeVoid(callbacks.sanitize, "Settings sanitization", result.warnings);
  invokeVoid(callbacks.applyTheme, "Theme reapplication", result.warnings);
  invokeVoid(callbacks.applyJukebox, "Jukebox reapplication", result.warnings);
  invokeWarning(callbacks.applyMetadata, "System metadata reapplication",
                result.warnings);
  invokeWarning(callbacks.applyAudio, "Audio reapplication", result.warnings);
  invokeVoid(callbacks.refreshDrafts, "Settings draft refresh",
             result.warnings);

  if (!callbacks.applyDisplay) {
    appendException(result.warnings, "Display reapplication");
    return result;
  }
  try {
    const ProfileDisplayRuntimeResult display = callbacks.applyDisplay();
    if (display.outcome == ProfileDisplayRuntimeOutcome::Failed) {
      result.warnings.push_back(display.message.empty()
                                    ? "Display reapplication failed."
                                    : display.message);
    }
  } catch (const std::exception &error) {
    appendException(result.warnings, "Display reapplication", &error);
  } catch (...) {
    appendException(result.warnings, "Display reapplication");
  }
  return result;
}
