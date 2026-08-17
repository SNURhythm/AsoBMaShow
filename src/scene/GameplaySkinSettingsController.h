#pragma once

#include "../PlatformDocumentHandoff.h"
#include "../skin/SkinCommitCoordinator.h"
#include "../skin/beatoraja/SkinDiagnosticHistory.h"
#include "../skin/package/SkinPackageOperationService.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace skin {

struct SkinPackageNameSuggestion {
  std::string originalSourceName;
  std::string suggestedPackageName;
  std::string validationError;

  [[nodiscard]] bool ok() const noexcept { return validationError.empty(); }
};

[[nodiscard]] SkinPackageNameSuggestion
suggestSkinPackageName(std::string originalSourceName,
                       PlatformTemporaryPathKind pathKind);

enum class GameplaySkinSettingsState : std::uint8_t {
  Empty,
  Ready,
  Busy,
  Error,
};

struct GameplaySkinEntryRow {
  SkinEntryId entry;
  SkinEntryMetadataSnapshot metadata;
  std::string revisionDigest;
  std::string configurationDigest;
  SkinValidationDisposition validation = SkinValidationDisposition::Invalid;
  EntryProfileSettings settings;
  std::vector<SkinDiagnostic> diagnostics;
};

struct GameplaySkinSettingsSnapshot {
  GameplaySkinSettingsState state = GameplaySkinSettingsState::Empty;
  bool featureAvailable = true;
  bool compatibilityEnabled = false;
  SkinSafetyLevel safetyLevel = SkinSafetyLevel::Standard;
  std::optional<SkinSafetyLevel> pendingSafetyLevel;
  std::map<int, SkinEntryId> selectedGameplayEntries;
  // Transitional projection for old callers. The trait map above is the UI's
  // source of truth.
  std::optional<SkinEntryId> selected7KeyEntry;
  std::vector<GameplaySkinEntryRow> entries;
  std::optional<SkinPackageNameSuggestion> preparedName;
  std::optional<SkinPackageId> collisionPackage;
  bool hasPackageProgress = false;
  SkinProgress progress;
  SkinRescanProgress rescanProgress;
  std::vector<SkinDiagnosticHistoryRecord> history;
  std::string statusMessage;
  bool canCancel = false;
  // Set only by the controller. It describes catalog/profile generations and
  // compact live UI state, so Settings does not re-encode every static skin
  // title and configuration declaration on each frame.
  std::string cachedPresentationKey;
};

struct ControllerActionResult {
  bool accepted = false;
  bool asynchronous = false;
  std::string message;
  std::vector<SkinDiagnostic> diagnostics;
};

struct GameplaySkinSettingsControllerDependencies {
  SkinPackageOperationService &operations;
  SkinDiagnosticHistory &history;
  SkinProfileId profileId;
  ISkinProfileSettingsOwner &profileOwner;
  ISkinProfileSnapshotProvider &profileSnapshots;
  SkinCommitCoordinator &commits;
  SkinActivationClientId clientId;
  std::function<void()> requestRescan;
  std::function<void()> cancelRescan;
  std::function<SkinRescanProgress()> rescanProgress;
  std::function<void(const SkinEntryId &)> requestRevalidation;
  std::function<std::shared_ptr<const SkinPackageCatalogSnapshot>()>
      catalogSnapshot;
  std::function<platform_document_handoff::PlatformDocumentHandoffOperation()>
      beginArchiveHandoff;
  std::function<platform_document_handoff::PlatformDocumentHandoffOperation(
      PlatformDirectoryImportRequest)>
      beginFolderHandoff;
};

class GameplaySkinSettingsController {
public:
  explicit GameplaySkinSettingsController(
      GameplaySkinSettingsControllerDependencies dependencies);
  ~GameplaySkinSettingsController();

  GameplaySkinSettingsController(const GameplaySkinSettingsController &) =
      delete;
  GameplaySkinSettingsController &
  operator=(const GameplaySkinSettingsController &) = delete;

  [[nodiscard]] const GameplaySkinSettingsSnapshot &snapshot() const noexcept;
  void poll();
  void profileChanged(SkinProfileId profileId, SkinActivationClientId clientId);

  [[nodiscard]] ControllerActionResult beginArchiveImport();
  [[nodiscard]] ControllerActionResult beginFolderImport();
  [[nodiscard]] ControllerActionResult
  setSuggestedPackageName(std::string packageName);
  [[nodiscard]] ControllerActionResult
  confirmPreparedImport(PackageCollisionPolicy collisionPolicy);
  [[nodiscard]] ControllerActionResult requestRescan();
  [[nodiscard]] ControllerActionResult
  requestRevalidation(const SkinEntryId &entry);
  [[nodiscard]] ControllerActionResult
  selectGameplayTrait(int skinType, const SkinEntryId &entry);
  [[nodiscard]] ControllerActionResult clearGameplayTrait(int skinType);
  // Transitional convenience API: selects the trait declared by the entry.
  [[nodiscard]] ControllerActionResult select(const SkinEntryId &entry);
  [[nodiscard]] ControllerActionResult setCompatibilityEnabled(bool enabled);
  [[nodiscard]] ControllerActionResult setSafetyLevel(SkinSafetyLevel level);
  [[nodiscard]] ControllerActionResult confirmSafetyLevelChange();
  void cancelSafetyLevelChange() noexcept;
  [[nodiscard]] ControllerActionResult setOption(const SkinEntryId &entry,
                                                 std::string name, int value);
  [[nodiscard]] ControllerActionResult
  setFileChoice(const SkinEntryId &entry, std::string name, std::string value);
  [[nodiscard]] ControllerActionResult
  setOffset(const SkinEntryId &entry, std::string name, ConfigOffset value);
  [[nodiscard]] ControllerActionResult setViewport(const SkinEntryId &entry,
                                                   ViewportSettings viewport);
  [[nodiscard]] ControllerActionResult
  requestRemoval(const SkinPackageId &package);
  [[nodiscard]] ControllerActionResult resetLayout(const SkinEntryId &entry);

  void cancelRescan() noexcept;
  void close() noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace skin
