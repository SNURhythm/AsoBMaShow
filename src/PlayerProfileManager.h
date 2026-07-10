#pragma once

#include "PlayerProfile.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

enum class ProfileError {
  None,
  InvalidName,
  NotFound,
  ActiveProfileDeletion,
  LastProfileDeletion,
  FutureVersion,
  IoFailure,
  MigrationFailure,
  IntegrityFailure,
  SwitchBlocked
};

struct ProfileResult {
  ProfileError error = ProfileError::None;
  std::string message;
  std::optional<PlayerProfile> profile;

  [[nodiscard]] bool ok() const { return error == ProfileError::None; }
};

enum class ProfileMigrationPhase {
  PrepareStaging,
  WriteSettings,
  WriteInput,
  SnapshotScores,
  SnapshotReplays,
  EnsureScoreSchema,
  EnsureReplaySchema,
  ValidateIntegrity,
  CompareRows,
  WriteMetadata,
  FinalizeProfile,
  WriteBootstrap
};

struct PlayerProfileManagerDependencies {
  std::function<std::string()> generateUuid;
  std::function<std::string()> utcNow;
  std::function<bool(const std::filesystem::path &,
                     const std::filesystem::path &, std::string &)>
      snapshotDatabase;
  std::function<bool(ProfileMigrationPhase, std::string &)>
      beforeMigrationPhase;
};

class PlayerProfileManager {
public:
  explicit PlayerProfileManager(
      std::filesystem::path applicationDataRoot,
      PlayerProfileManagerDependencies dependencies = {});

  ProfileResult Initialize();
  [[nodiscard]] const PlayerProfile &activeProfile() const;
  [[nodiscard]] PlayerProfilePaths activePaths() const;
  [[nodiscard]] PlayerProfilePaths pathsFor(std::string_view id) const;
  [[nodiscard]] std::vector<PlayerProfile> listProfiles() const;
  ProfileResult createProfile(std::string displayName);
  ProfileResult duplicateProfile(std::string_view sourceId,
                                 std::string displayName);
  ProfileResult renameProfile(std::string_view id, std::string displayName);
  ProfileResult deleteProfile(std::string_view id);
  [[nodiscard]] ProfileResult validateProfile(std::string_view id) const;
  ProfileResult commitActiveProfile(std::string_view id);

private:
  std::filesystem::path applicationDataRoot_;
  PlayerProfileManagerDependencies dependencies_;
  std::optional<PlayerProfile> activeProfile_;
};
