#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace ir {

class PendingIrCredentialCleanup final {
public:
  explicit PendingIrCredentialCleanup(
      const std::filesystem::path &applicationDataRoot);

  [[nodiscard]] bool schedule(std::string_view profileId,
                              std::string &diagnostic) noexcept;
  [[nodiscard]] bool complete(std::string_view profileId,
                              std::string &diagnostic) noexcept;
  [[nodiscard]] std::vector<std::string>
  pending(std::string &diagnostic) const noexcept;

private:
  [[nodiscard]] std::filesystem::path
  markerPath(std::string_view profileId) const;

  std::filesystem::path directory_;
  mutable std::mutex mutex_;
};

enum class ProfileCredentialDeletionStatus {
  Removed,
  QueueFailed,
  ProfileDeletionFailed,
  CredentialCleanupPending,
};

struct ProfileCredentialDeletionResult {
  ProfileCredentialDeletionStatus status =
      ProfileCredentialDeletionStatus::QueueFailed;
  std::string diagnostic;
};

using ProfileDeletionOperation = std::function<bool(std::string &diagnostic)>;
using ProfileCredentialRemovalOperation =
    std::function<bool(std::string &diagnostic)>;

[[nodiscard]] ProfileCredentialDeletionResult coordinateProfileCredentialDeletion(
    PendingIrCredentialCleanup &pending, std::string_view profileId,
    ProfileDeletionOperation deleteProfile,
    ProfileCredentialRemovalOperation removeCredentials) noexcept;

struct PendingIrCredentialCleanupRetryResult {
  std::size_t completed = 0;
  std::size_t cancelled = 0;
  std::size_t retained = 0;
  std::string diagnostic;
};

using ProfileExistenceLookup = std::function<bool(std::string_view profileId)>;
using PendingProfileCredentialRemoval =
    std::function<bool(std::string_view profileId, std::string &diagnostic)>;

[[nodiscard]] PendingIrCredentialCleanupRetryResult
retryPendingProfileCredentialCleanup(
    PendingIrCredentialCleanup &pending, ProfileExistenceLookup profileExists,
    PendingProfileCredentialRemoval removeCredentials) noexcept;

} // namespace ir
