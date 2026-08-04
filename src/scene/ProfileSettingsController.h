#pragma once

#include "../PlayerProfileManager.h"
#include "../ProfileArchive.h"
#include "../ProfileSessionCoordinator.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class ApplicationContext;

enum class ProfileSettingsPhase {
  Idle,
  ConfirmDelete,
  ConfirmOverwrite,
  PickingImport,
  Importing,
  PreparingExport,
  PickingExport,
};

enum class ProfileSettingsStatusKind { None, Info, Success, Warning, Error };

struct ProfileSettingsStatus {
  ProfileSettingsStatusKind kind = ProfileSettingsStatusKind::None;
  std::string message;

  bool operator==(const ProfileSettingsStatus &) const = default;
};

struct ProfileActionEligibility {
  bool enabled = false;
  std::string reason;

  bool operator==(const ProfileActionEligibility &) const = default;
};

struct ProfileListResult {
  ProfileError error = ProfileError::None;
  std::string message;
  std::vector<PlayerProfile> profiles;
  std::string activeProfileId;

  [[nodiscard]] bool ok() const { return error == ProfileError::None; }
};

struct ProfileSettingsControllerDependencies {
  std::function<ProfileListResult()> listProfiles;
  std::function<ProfileResult(std::string)> create;
  std::function<ProfileResult(std::string_view, std::string)> rename;
  std::function<ProfileResult(std::string_view, std::string)> duplicate;
  std::function<ProfileResult(std::string_view)> remove;
  std::function<ProfileSwitchResult(std::string_view)> activate;
  std::function<ProfileArchiveResult(std::string_view,
                                     const std::filesystem::path &)>
      exportProfile;
  std::function<ProfileArchiveResult(const std::filesystem::path &,
                                     const ProfileImportOptions &)>
      importProfile;
  std::function<bool(std::string &)> flushSettings;
  std::function<bool(std::string &)> flushInput;
  std::function<bool(std::string &)> beginArchivePipeline;
  std::function<void()> endArchivePipeline;
  // Dependency-free Task 24 boundary. Enabled builds retain the returned
  // opaque token across the exact profile-membership mutation; disabled builds
  // use the controller's no-op default without depending on skin types.
  std::function<std::optional<std::uint64_t>(
      std::optional<std::string_view>, std::string &)>
      beginSkinProfileCatalogMutation;
  std::function<void(std::uint64_t, bool, bool)>
      finishSkinProfileCatalogMutation;
};

enum class ProfileArchiveTaskKind { Export, Import };

class ProfileArchiveTask {
public:
  ProfileArchiveTask(const ProfileArchiveTask &) = delete;
  ProfileArchiveTask &operator=(const ProfileArchiveTask &) = delete;
  ProfileArchiveTask(ProfileArchiveTask &&) noexcept = default;
  ProfileArchiveTask &operator=(ProfileArchiveTask &&) noexcept = default;

  [[nodiscard]] ProfileArchiveTaskKind kind() const { return kind_; }
  [[nodiscard]] std::uint64_t generation() const { return generation_; }
  ProfileArchiveResult execute();

private:
  friend class ProfileSettingsController;
  ProfileArchiveTask(ProfileArchiveTaskKind kind, std::uint64_t generation,
                     std::function<ProfileArchiveResult()> operation)
      : kind_(kind), generation_(generation), operation_(std::move(operation)) {
  }

  ProfileArchiveTaskKind kind_;
  std::uint64_t generation_;
  std::function<ProfileArchiveResult()> operation_;
};

class ProfileSettingsController {
public:
  explicit ProfileSettingsController(
      ProfileSettingsControllerDependencies dependencies);
  explicit ProfileSettingsController(ApplicationContext &context);
  ~ProfileSettingsController();

  [[nodiscard]] const std::vector<PlayerProfile> &profiles() const;
  [[nodiscard]] const std::string &activeProfileId() const;
  [[nodiscard]] const std::string &selectedProfileId() const;
  [[nodiscard]] const std::string &confirmationProfileId() const;
  [[nodiscard]] ProfileSettingsPhase phase() const;
  [[nodiscard]] const ProfileSettingsStatus &status() const;
  [[nodiscard]] bool actionsEnabled() const;

  bool refresh();
  bool select(std::string_view profileId);
  [[nodiscard]] ProfileActionEligibility
  deleteEligibility(std::string_view profileId) const;
  [[nodiscard]] ProfileActionEligibility
  overwriteEligibility(std::string_view profileId) const;

  ProfileResult create(std::string name);
  ProfileResult rename(std::string_view profileId, std::string name);
  ProfileResult duplicate(std::string_view profileId, std::string name);
  ProfileResult remove(std::string_view profileId);
  ProfileSwitchResult activate(std::string_view profileId);

  ProfileResult requestDelete(std::string_view profileId);
  ProfileResult confirmDelete();
  ProfileResult requestOverwrite(std::string_view profileId);
  void cancelConfirmation();

  bool beginImportPicker();
  bool beginConfirmedOverwritePicker();
  bool beginPreparedExportPicker(std::uint64_t generation);
  void cancelPicker();
  bool failPicker(std::string message);

  std::optional<ProfileArchiveTask>
  beginExport(std::string_view profileId,
              const std::filesystem::path &destination);
  std::optional<ProfileArchiveTask>
  beginImport(const std::filesystem::path &archive,
              const ProfileImportOptions &options = {});
  bool completeArchive(ProfileArchiveTaskKind kind, std::uint64_t generation,
                       const ProfileArchiveResult &result);
  void abandonArchive(std::uint64_t generation);

  ProfileArchiveResult exportProfile(std::string_view profileId,
                                     const std::filesystem::path &destination);
  ProfileArchiveResult importProfile(const std::filesystem::path &archive,
                                     const ProfileImportOptions &options = {});

  void recordError(std::string message);
  void recordWarning(std::string message);

private:
  [[nodiscard]] bool contains(std::string_view profileId) const;
  [[nodiscard]] ProfileActionEligibility
  destructiveEligibility(std::string_view profileId) const;
  [[nodiscard]] ProfileResult unavailableResult(std::string message) const;
  [[nodiscard]] ProfileArchiveResult
  unavailableArchiveResult(std::string message) const;
  bool flushActiveState(std::string &errorMessage);
  bool acquireArchivePipeline();
  void releaseArchivePipeline();
  std::optional<std::uint64_t> beginSkinProfileCatalogMutation(
      std::optional<std::string_view> existingTarget,
      std::string &errorMessage);
  void finishSkinProfileCatalogMutation(std::uint64_t token, bool succeeded,
                                        bool profileStillExists) noexcept;
  void abandonArchiveSkinMutation() noexcept;
  void setFailure(ProfileError error, std::string message,
                  std::string fallback);
  void setSuccess(std::string message, std::string fallback);
  bool refreshAfterMutation(std::optional<std::string> preferredProfileId,
                            const std::string &operationError = {});
  ProfileResult finishMutation(ProfileResult result, std::string successText,
                               std::optional<std::string> preferredProfileId);
  void clearTransientPhase();

  ProfileSettingsControllerDependencies dependencies_;
  std::vector<PlayerProfile> profiles_;
  std::string activeProfileId_;
  std::string selectedProfileId_;
  std::string confirmationProfileId_;
  std::optional<ProfileSettingsStatus> confirmationPriorStatus_;
  ProfileSettingsPhase phase_ = ProfileSettingsPhase::Idle;
  ProfileSettingsStatus status_;
  std::uint64_t nextArchiveGeneration_ = 1;
  std::uint64_t activeArchiveGeneration_ = 0;
  bool archivePipelineHeld_ = false;
  std::optional<ProfileSettingsStatus> archivePipelinePriorStatus_;
  std::uint64_t nextFallbackSkinMutationToken_ = 0;
  std::uint64_t activeArchiveSkinMutationToken_ = 0;
  std::optional<std::string> activeArchiveSkinMutationTarget_;
};
