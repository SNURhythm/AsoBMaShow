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
    : directory_(applicationDataRoot / ".pending-ir-credential-cleanup") {}

std::filesystem::path PendingIrCredentialCleanup::markerPath(
    std::string_view profileId) const {
  return directory_ /
         (std::string(profileId) + std::string(kMarkerSuffix));
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

} // namespace ir
