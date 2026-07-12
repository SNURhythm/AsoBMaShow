#pragma once

#include "PlayerProfileManager.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

struct ProfileArchiveManifest {
  static constexpr int kFormatVersion = 2;

  int formatVersion = kFormatVersion;
  std::string sourceApplicationVersion;
  std::string profileUuid;
  std::string profileDisplayName;
  std::string createdAt;
  int profileSchemaVersion = 1;
  int settingsSchemaVersion = 1;
  int inputSchemaVersion = 1;
  int practiceSchemaVersion = 1;
  int scoreSchemaVersion = 5;
  int replaySchemaVersion = 3;
};

enum class ProfileImportMode { CreateWithNewId, Overwrite };

struct ProfileImportOptions {
  ProfileImportMode mode = ProfileImportMode::CreateWithNewId;
  std::optional<std::string> overwriteProfileId;
};

struct ProfileArchiveResult {
  ProfileError error = ProfileError::None;
  std::string message;
  std::optional<PlayerProfile> profile;

  [[nodiscard]] bool ok() const { return error == ProfileError::None; }
};

struct ProfileArchiveSizePolicy {
  static constexpr std::uint64_t kMaximumMetadataBytes = 1ULL * 1024 * 1024;
  static constexpr std::uint64_t kMaximumDatabaseBytes =
      2ULL * 1024 * 1024 * 1024;
  static constexpr std::uint64_t kMaximumTotalBytes = 4ULL * 1024 * 1024 * 1024;
  static constexpr std::uint64_t kMaximumExistingArchiveBytes =
      kMaximumTotalBytes + kMaximumMetadataBytes;

  [[nodiscard]] static bool memberSizeAllowed(std::string_view memberName,
                                              std::uint64_t bytes);
  [[nodiscard]] static bool totalSizeAllowed(std::uint64_t bytes);
  [[nodiscard]] static bool additionAllowed(std::string_view memberName,
                                            std::uint64_t currentMemberBytes,
                                            std::uint64_t currentTotalBytes,
                                            std::uint64_t additionalBytes);
};

enum class ProfileArchiveExportPhase { TemporaryArchiveWritten };

struct ProfileArchiveFilesystemOperations {
  std::function<bool(const std::filesystem::path &, std::string &)> syncFile;
  std::function<bool(const std::filesystem::path &, std::string &)>
      syncDirectory;
  std::function<bool(const std::filesystem::path &,
                     const std::filesystem::path &, std::string &)>
      durableRename;
  std::function<bool(const std::filesystem::path &, std::string &)> removePath;
};

using ProfileArchiveSizeCheck = std::function<bool(
    std::string_view memberName, std::uint64_t currentMemberBytes,
    std::uint64_t currentTotalBytes, std::uint64_t additionalBytes)>;

struct ProfileArchiveValidationOperations {
  // Optional dependency seams can only impose stricter limits; the built-in
  // production caps are always enforced first.
  ProfileArchiveSizeCheck declaredSizeAllowed;
  ProfileArchiveSizeCheck streamedSizeAllowed;
};

struct ProfileArchiveDependencies {
  std::function<bool(ProfileArchiveExportPhase, std::string &errorMessage)>
      beforeExportPhase;
  std::function<void(const std::filesystem::path &)> importWorkspaceCreated;
  ProfileArchiveFilesystemOperations filesystem;
  ProfileArchiveValidationOperations validation;
};

class ProfileArchiveService {
public:
  static constexpr std::uint64_t kMaximumMetadataBytes =
      ProfileArchiveSizePolicy::kMaximumMetadataBytes;
  static constexpr std::uint64_t kMaximumDatabaseBytes =
      ProfileArchiveSizePolicy::kMaximumDatabaseBytes;
  static constexpr std::uint64_t kMaximumTotalBytes =
      ProfileArchiveSizePolicy::kMaximumTotalBytes;

  explicit ProfileArchiveService(PlayerProfileManager &manager,
                                 ProfileArchiveDependencies dependencies = {});

  ProfileArchiveResult Export(std::string_view profileId,
                              const std::filesystem::path &destination);
  ProfileArchiveResult Import(const std::filesystem::path &archive,
                              const ProfileImportOptions &options = {});

private:
  PlayerProfileManager &manager_;
  ProfileArchiveDependencies dependencies_;
  std::string startupCleanupError_;
};
