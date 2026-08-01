#include "PendingIrCredentialCleanup.h"

#include "../AtomicFile.h"
#include "IrCredentialBackend.h"

#include <algorithm>
#include <array>
#include <exception>
#include <span>
#include <system_error>
#include <utility>

namespace ir {
namespace {
constexpr std::string_view kMarkerSuffix = ".pending";
constexpr std::string_view kOverwriteResetMarker =
    ".pending-ir-credential-overwrite-reset";
constexpr std::size_t kMaximumPendingProfiles = 1024;
constexpr std::array<std::byte, 8> kMarkerContents = {
    std::byte{'p'}, std::byte{'e'}, std::byte{'n'}, std::byte{'d'},
    std::byte{'i'}, std::byte{'n'}, std::byte{'g'}, std::byte{'\n'}};

void appendDiagnostic(std::string &destination, std::string_view detail) {
  if (detail.empty()) {
    return;
  }
  if (!destination.empty()) {
    destination += "; ";
  }
  destination += detail;
}

bool removeMarkerFile(const std::filesystem::path &path,
                      std::string &diagnostic) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error == std::errc::no_such_file_or_directory ||
      status.type() == std::filesystem::file_type::not_found) {
    diagnostic.clear();
    return true;
  }
  if (error) {
    diagnostic = "pending credential cleanup marker could not be inspected: " +
                 error.message();
    return false;
  }
  if (!std::filesystem::is_regular_file(status)) {
    diagnostic = "pending credential cleanup marker is not a regular file";
    return false;
  }
  if (!std::filesystem::remove(path, error) || error) {
    diagnostic = "pending credential cleanup marker could not be removed";
    if (error) {
      diagnostic += ": " + error.message();
    }
    return false;
  }
  return atomic_file::syncDirectory(path.parent_path(), diagnostic);
}
} // namespace

PendingIrCredentialCleanup::PendingIrCredentialCleanup(
    const std::filesystem::path &applicationDataRoot)
    : applicationDataRoot_(applicationDataRoot),
      directory_(applicationDataRoot / ".pending-ir-credential-cleanup") {}

std::filesystem::path PendingIrCredentialCleanup::markerPath(
    std::string_view profileId) const {
  return directory_ /
         (std::string(profileId) + std::string(kMarkerSuffix));
}

std::filesystem::path PendingIrCredentialCleanup::overwriteResetMarkerPath(
    std::string_view profileId, bool staging) const {
  const std::string directoryName =
      staging ? ".staging-" + std::string(profileId) : std::string(profileId);
  return applicationDataRoot_ / "profiles" / directoryName /
         std::string(kOverwriteResetMarker);
}

bool PendingIrCredentialCleanup::schedule(std::string_view profileId,
                                          std::string &diagnostic) noexcept {
  try {
    std::lock_guard lock(mutex_);
    diagnostic.clear();
    if (!isValidCredentialProfileId(profileId)) {
      diagnostic = "pending credential cleanup profile identity is invalid";
      return false;
    }
    const auto operations = atomic_file::privateFileOperations();
    return atomic_file::writeWithoutBackup(markerPath(profileId),
                                           kMarkerContents, diagnostic,
                                           &operations);
  } catch (const std::exception &error) {
    diagnostic = std::string("pending credential cleanup could not be saved: ") +
                 error.what();
    return false;
  } catch (...) {
    diagnostic = "pending credential cleanup could not be saved";
    return false;
  }
}

bool PendingIrCredentialCleanup::complete(std::string_view profileId,
                                          std::string &diagnostic) noexcept {
  try {
    std::lock_guard lock(mutex_);
    diagnostic.clear();
    if (!isValidCredentialProfileId(profileId)) {
      diagnostic = "pending credential cleanup profile identity is invalid";
      return false;
    }
    return removeMarkerFile(markerPath(profileId), diagnostic);
  } catch (const std::exception &error) {
    diagnostic =
        std::string("pending credential cleanup could not be completed: ") +
        error.what();
    return false;
  } catch (...) {
    diagnostic = "pending credential cleanup could not be completed";
    return false;
  }
}

std::vector<std::string>
PendingIrCredentialCleanup::pending(std::string &diagnostic) const noexcept {
  try {
    std::lock_guard lock(mutex_);
    diagnostic.clear();
    std::error_code error;
    const auto directoryStatus =
        std::filesystem::symlink_status(directory_, error);
    if (error == std::errc::no_such_file_or_directory ||
        directoryStatus.type() == std::filesystem::file_type::not_found) {
      return {};
    }
    if (error || !std::filesystem::is_directory(directoryStatus)) {
      diagnostic = error
                       ? "pending credential cleanup directory could not be "
                         "inspected: " +
                             error.message()
                       : "pending credential cleanup path is not a directory";
      return {};
    }

    std::vector<std::string> result;
    std::filesystem::directory_iterator entries(directory_, error);
    const std::filesystem::directory_iterator end;
    while (!error && entries != end) {
      const auto status = entries->symlink_status(error);
      if (error) {
        break;
      }
      const auto filename = entries->path().filename().string();
      if (std::filesystem::is_regular_file(status) &&
          filename.ends_with(kMarkerSuffix)) {
        const std::string profileId =
            filename.substr(0, filename.size() - kMarkerSuffix.size());
        if (!isValidCredentialProfileId(profileId)) {
          diagnostic = "pending credential cleanup marker identity is invalid";
          return {};
        }
        if (result.size() >= kMaximumPendingProfiles) {
          diagnostic = "pending credential cleanup marker limit exceeded";
          return {};
        }
        result.push_back(profileId);
      }
      entries.increment(error);
    }
    if (error) {
      diagnostic = "pending credential cleanup directory could not be read: " +
                   error.message();
      return {};
    }
    std::ranges::sort(result);
    return result;
  } catch (const std::exception &error) {
    diagnostic =
        std::string("pending credential cleanup could not be loaded: ") +
        error.what();
    return {};
  } catch (...) {
    diagnostic = "pending credential cleanup could not be loaded";
    return {};
  }
}

bool PendingIrCredentialCleanup::scheduleOverwriteReset(
    std::string_view profileId, std::string &diagnostic) noexcept {
  try {
    std::lock_guard lock(mutex_);
    diagnostic.clear();
    if (!isValidCredentialProfileId(profileId)) {
      diagnostic = "overwrite credential reset profile identity is invalid";
      return false;
    }
    const auto marker = overwriteResetMarkerPath(profileId, true);
    std::error_code error;
    const auto parentStatus =
        std::filesystem::symlink_status(marker.parent_path(), error);
    if (error || !std::filesystem::is_directory(parentStatus) ||
        std::filesystem::is_symlink(parentStatus)) {
      diagnostic = "overwrite credential reset staging directory is invalid";
      return false;
    }
    const auto operations = atomic_file::privateFileOperations();
    return atomic_file::writeWithoutBackup(marker, kMarkerContents, diagnostic,
                                           &operations);
  } catch (const std::exception &error) {
    diagnostic = std::string("overwrite credential reset could not be saved: ") +
                 error.what();
    return false;
  } catch (...) {
    diagnostic = "overwrite credential reset could not be saved";
    return false;
  }
}

bool PendingIrCredentialCleanup::completeOverwriteReset(
    std::string_view profileId, std::string &diagnostic) noexcept {
  try {
    std::lock_guard lock(mutex_);
    diagnostic.clear();
    if (!isValidCredentialProfileId(profileId)) {
      diagnostic = "overwrite credential reset profile identity is invalid";
      return false;
    }
    return removeMarkerFile(overwriteResetMarkerPath(profileId, false),
                            diagnostic);
  } catch (const std::exception &error) {
    diagnostic =
        std::string("overwrite credential reset could not be completed: ") +
        error.what();
    return false;
  } catch (...) {
    diagnostic = "overwrite credential reset could not be completed";
    return false;
  }
}

std::vector<std::string> PendingIrCredentialCleanup::pendingOverwriteResets(
    std::string &diagnostic) const noexcept {
  try {
    std::lock_guard lock(mutex_);
    diagnostic.clear();
    const auto profilesRoot = applicationDataRoot_ / "profiles";
    std::error_code error;
    const auto rootStatus = std::filesystem::symlink_status(profilesRoot, error);
    if (error == std::errc::no_such_file_or_directory ||
        rootStatus.type() == std::filesystem::file_type::not_found) {
      return {};
    }
    if (error || !std::filesystem::is_directory(rootStatus) ||
        std::filesystem::is_symlink(rootStatus)) {
      diagnostic = "overwrite credential reset profile directory is invalid";
      return {};
    }

    std::vector<std::string> result;
    std::filesystem::directory_iterator entries(profilesRoot, error);
    const std::filesystem::directory_iterator end;
    while (!error && entries != end) {
      const auto profileStatus = entries->symlink_status(error);
      if (error) {
        break;
      }
      const std::string profileId = entries->path().filename().string();
      if (std::filesystem::is_directory(profileStatus) &&
          !std::filesystem::is_symlink(profileStatus) &&
          isValidCredentialProfileId(profileId)) {
        const auto markerStatus = std::filesystem::symlink_status(
            entries->path() / std::string(kOverwriteResetMarker), error);
        if (error == std::errc::no_such_file_or_directory ||
            markerStatus.type() == std::filesystem::file_type::not_found) {
          error.clear();
        } else if (error) {
          break;
        } else if (!std::filesystem::is_regular_file(markerStatus) ||
                   std::filesystem::is_symlink(markerStatus)) {
          diagnostic = "overwrite credential reset marker is invalid";
          return {};
        } else {
          if (result.size() >= kMaximumPendingProfiles) {
            diagnostic = "overwrite credential reset marker limit exceeded";
            return {};
          }
          result.push_back(profileId);
        }
      }
      entries.increment(error);
    }
    if (error) {
      diagnostic = "overwrite credential reset directory could not be read: " +
                   error.message();
      return {};
    }
    std::ranges::sort(result);
    return result;
  } catch (const std::exception &error) {
    diagnostic =
        std::string("overwrite credential resets could not be loaded: ") +
        error.what();
    return {};
  } catch (...) {
    diagnostic = "overwrite credential resets could not be loaded";
    return {};
  }
}

ProfileCredentialDeletionResult coordinateProfileCredentialDeletion(
    PendingIrCredentialCleanup &pending, std::string_view profileId,
    ProfileDeletionOperation deleteProfile,
    ProfileCredentialRemovalOperation removeCredentials) noexcept {
  ProfileCredentialDeletionResult result;
  try {
    if (!deleteProfile || !removeCredentials) {
      result.diagnostic = "profile credential deletion operations are missing";
      return result;
    }
    if (!pending.schedule(profileId, result.diagnostic)) {
      result.status = ProfileCredentialDeletionStatus::QueueFailed;
      return result;
    }

    std::string deletionDiagnostic;
    if (!deleteProfile(deletionDiagnostic)) {
      result.status = ProfileCredentialDeletionStatus::ProfileDeletionFailed;
      result.diagnostic = std::move(deletionDiagnostic);
      std::string markerDiagnostic;
      if (!pending.complete(profileId, markerDiagnostic)) {
        appendDiagnostic(result.diagnostic, markerDiagnostic);
      }
      return result;
    }

    std::string credentialDiagnostic;
    if (!removeCredentials(credentialDiagnostic)) {
      result.status =
          ProfileCredentialDeletionStatus::CredentialCleanupPending;
      result.diagnostic = credentialDiagnostic.empty()
                              ? "Secure IR credential cleanup will retry."
                              : std::move(credentialDiagnostic);
      return result;
    }

    std::string markerDiagnostic;
    if (!pending.complete(profileId, markerDiagnostic)) {
      result.status =
          ProfileCredentialDeletionStatus::CredentialCleanupPending;
      result.diagnostic = std::move(markerDiagnostic);
      return result;
    }
    result.status = ProfileCredentialDeletionStatus::Removed;
    result.diagnostic = std::move(deletionDiagnostic);
    return result;
  } catch (const std::exception &error) {
    result.diagnostic =
        std::string("profile credential deletion failed unexpectedly: ") +
        error.what();
    return result;
  } catch (...) {
    result.diagnostic = "profile credential deletion failed unexpectedly";
    return result;
  }
}

ProfileCredentialOverwriteCleanupResult
finishProfileCredentialOverwriteCleanup(
    PendingIrCredentialCleanup &pending, std::string_view profileId,
    ProfileCredentialRemovalOperation removeCredentials) noexcept {
  ProfileCredentialOverwriteCleanupResult result;
  try {
    if (!removeCredentials) {
      result.diagnostic = "overwrite credential cleanup operation is missing";
      return result;
    }
    if (!removeCredentials(result.diagnostic)) {
      if (result.diagnostic.empty()) {
        result.diagnostic = "Secure IR credential cleanup will retry.";
      }
      return result;
    }
    if (!pending.completeOverwriteReset(profileId, result.diagnostic)) {
      return result;
    }
    result.status = ProfileCredentialOverwriteCleanupStatus::Removed;
    return result;
  } catch (const std::exception &error) {
    result.diagnostic =
        std::string("overwrite credential cleanup failed unexpectedly: ") +
        error.what();
    return result;
  } catch (...) {
    result.diagnostic = "overwrite credential cleanup failed unexpectedly";
    return result;
  }
}

PendingIrCredentialCleanupRetryResult retryPendingProfileCredentialCleanup(
    PendingIrCredentialCleanup &pending, ProfileExistenceLookup profileExists,
    PendingProfileCredentialRemoval removeCredentials) noexcept {
  PendingIrCredentialCleanupRetryResult result;
  try {
    if (!profileExists || !removeCredentials) {
      result.diagnostic = "pending credential cleanup operations are missing";
      return result;
    }
    std::string loadDiagnostic;
    const auto profileIds = pending.pending(loadDiagnostic);
    if (!loadDiagnostic.empty()) {
      result.diagnostic = std::move(loadDiagnostic);
      return result;
    }

    for (const auto &profileId : profileIds) {
      if (profileExists(profileId)) {
        std::string markerDiagnostic;
        if (pending.complete(profileId, markerDiagnostic)) {
          ++result.cancelled;
        } else {
          ++result.retained;
          appendDiagnostic(result.diagnostic, markerDiagnostic);
        }
        continue;
      }

      std::string credentialDiagnostic;
      if (!removeCredentials(profileId, credentialDiagnostic)) {
        ++result.retained;
        appendDiagnostic(result.diagnostic, credentialDiagnostic);
        continue;
      }
      std::string markerDiagnostic;
      if (pending.complete(profileId, markerDiagnostic)) {
        ++result.completed;
      } else {
        ++result.retained;
        appendDiagnostic(result.diagnostic, markerDiagnostic);
      }
    }
    return result;
  } catch (const std::exception &error) {
    result.diagnostic =
        std::string("pending credential cleanup retry failed: ") +
        error.what();
    return result;
  } catch (...) {
    result.diagnostic = "pending credential cleanup retry failed";
    return result;
  }
}

PendingIrCredentialCleanupRetryResult
retryPendingProfileCredentialOverwriteCleanup(
    PendingIrCredentialCleanup &pending,
    PendingProfileCredentialRemoval removeCredentials) noexcept {
  PendingIrCredentialCleanupRetryResult result;
  try {
    if (!removeCredentials) {
      result.diagnostic = "overwrite credential cleanup operation is missing";
      return result;
    }
    std::string loadDiagnostic;
    const auto profileIds = pending.pendingOverwriteResets(loadDiagnostic);
    if (!loadDiagnostic.empty()) {
      result.diagnostic = std::move(loadDiagnostic);
      return result;
    }
    for (const auto &profileId : profileIds) {
      std::string credentialDiagnostic;
      if (!removeCredentials(profileId, credentialDiagnostic)) {
        ++result.retained;
        appendDiagnostic(result.diagnostic, credentialDiagnostic);
        continue;
      }
      std::string markerDiagnostic;
      if (pending.completeOverwriteReset(profileId, markerDiagnostic)) {
        ++result.completed;
      } else {
        ++result.retained;
        appendDiagnostic(result.diagnostic, markerDiagnostic);
      }
    }
    return result;
  } catch (const std::exception &error) {
    result.diagnostic =
        std::string("overwrite credential cleanup retry failed: ") +
        error.what();
    return result;
  } catch (...) {
    result.diagnostic =
        "overwrite credential cleanup retry failed unexpectedly";
    return result;
  }
}

} // namespace ir
