#pragma once

#include "../../ProfileSettingsPersistenceCoordinator.h"
#include "SkinArchiveImporter.h"
#include "SkinPackageCatalog.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace skin {

struct SkinValidationResult {
  SkinValidationDisposition disposition = SkinValidationDisposition::Invalid;
  bool cancelled = false;
  std::optional<EntryProfileSettings> reconciledSettings;
  std::optional<SkinEntryMetadataSnapshot> metadata;
  std::string configurationDigest;
  std::vector<SkinDiagnostic> diagnostics;
};

struct PublishPackageResult {
  bool published = false;
  bool retryableInventoryRace = false;
  std::optional<PreparedPackage> retryPrepared;
  SkinPackageId package;
  std::vector<SkinEntryId> entries;
  std::vector<SkinDiagnostic> diagnostics;
};

struct ScanPackagesResult {
  bool cancelled = false;
  bool retryableInventoryRace = false;
  std::uint64_t sourceGeneration = 0;
  std::vector<SkinEntryId> discoveredEntries;
  std::vector<SkinDiagnostic> diagnostics;
};

struct GarbageCollectionResult {
  std::uint64_t revisionsRemoved = 0;
  std::uint64_t bytesRemoved = 0;
  std::vector<SkinDiagnostic> diagnostics;
};

struct RemovePackageResult {
  bool removed = false;
  bool cancelled = false;
  SkinPackageId package;
  std::vector<SkinDiagnostic> diagnostics;
};

struct ValidatedSkinActivation {
  SkinRevisionLease revision;
  SkinEntryId entry;
  EntryProfileSettings reconciledSettings;
  std::string configurationDigest;
};

struct PreparedSkinActivation {
  std::uint64_t sourceGeneration = 0;
  std::uint64_t catalogGeneration = 0;
  std::uint64_t expectedProfileGeneration = 0;
  SkinProfileId profileId;
  ValidatedSkinActivation activation;
  SkinProfileSettings candidateProfileSettings;
};

struct PrepareActivationResult {
  std::optional<PreparedSkinActivation> prepared;
  std::optional<ValidatedSkinActivation> previousActivation;
  bool cancelled = false;
  std::vector<SkinDiagnostic> diagnostics;
};

enum class ActivationCommitDisposition : std::uint8_t {
  PendingProfileSave,
  ActivatedRequested,
  RetainedPrevious,
  ProfileGenerationChanged,
  SourceGenerationChanged,
  ProfileCommittedNeedsRevalidation,
};

struct CommitActivationResult {
  ActivationCommitDisposition disposition =
      ActivationCommitDisposition::RetainedPrevious;
  std::uint64_t ticket = 0;
  std::optional<ValidatedSkinActivation> activation;
  std::optional<VersionedSkinProfileSettings> profileSnapshot;
  std::vector<SkinDiagnostic> diagnostics;
};

struct AcquireActivationResult {
  std::optional<ValidatedSkinActivation> activation;
  std::vector<SkinDiagnostic> diagnostics;
};

class SkinEntryValidator {
public:
  virtual ~SkinEntryValidator() = default;
  virtual SkinValidationResult
  validate(SkinRevisionReadView revision, const SkinEntryId &entry,
           const EntryProfileSettings *desiredSettings,
           std::stop_token stop) = 0;
};

class SkinPackageStore {
public:
  SkinPackageStore(SkinStorageRoots, SkinPackageCatalog &, SkinAliasDetector &,
                   ISkinProfileSnapshotProvider &);

  void recover();
  PreparePackageResult prepareArchive(const std::filesystem::path &zip,
                                      const SkinPackageId &package,
                                      std::stop_token stop,
                                      SkinProgressCallback progress);
  PreparePackageResult prepareFolder(const std::filesystem::path &folder,
                                     const SkinPackageId &package,
                                     std::stop_token stop,
                                     SkinProgressCallback progress);
  PublishPackageResult
  publish(PreparedPackage &&prepared, PackageCollisionPolicy collisionPolicy,
          ProfileInventorySnapshot inventory, SkinEntryValidator &validator,
          std::stop_token stop, SkinProgressCallback progress);
  ScanPackagesResult rescanVisibleSources(std::stop_token stop,
                                          SkinProgressCallback progress,
                                          ProfileInventorySnapshot inventory,
                                          SkinEntryValidator &validator);
  PrepareActivationResult
  prepareActivation(const VersionedSkinProfileSettings &base,
                    const SkinEntryId &entry,
                    SkinProfileSettings candidateProfileSettings,
                    SkinEntryValidator &validator, std::stop_token stop);
  CommitActivationResult
  beginPreparedActivationCommit(PreparedSkinActivation &&prepared,
                                ISkinProfileSettingsOwner &owner);
  CommitActivationResult
  pollPreparedActivationCommit(std::uint64_t ticket,
                               ISkinProfileSettingsOwner &owner);
  AcquireActivationResult
  acquireValidatedActivation(const SkinProfileId &profile,
                             const SkinEntryId &entry,
                             std::string_view configurationDigest);
  std::shared_ptr<const SkinPackageCatalogSnapshot>
  catalogSnapshot() const noexcept;
  RemovePackageResult removePackage(const SkinPackageId &package,
                                    std::stop_token stop);
  void removeProfileActivations(const SkinProfileId &profile);
  void
  reconcileProfileActivations(std::span<const SkinProfileId> existingProfiles);
  GarbageCollectionResult collectGarbage();

private:
  SkinStorageRoots roots_;
  SkinPackageCatalog &catalog_;
  SkinAliasDetector &aliases_;
  ISkinProfileSnapshotProvider &profileSnapshots_;
  std::mutex stateMutex_;
};

} // namespace skin
