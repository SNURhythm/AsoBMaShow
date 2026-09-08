#pragma once

#include "SkinActivationCommitStore.h"
#include "SkinArchiveImporter.h"
#include "SkinPackageCatalog.h"
#include "../SkinSafetyPolicy.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace skin {

class SkinPackageOperationService;

enum class SkinPackageStoreIoOperation : std::uint8_t {
  RecoveryPhysicalVerification,
  RemovalVisibleRetained,
};

// Narrow fault-observation seam for deterministic transaction tests.
class SkinPackageStoreIoObserver {
public:
  virtual ~SkinPackageStoreIoObserver() = default;
  virtual void reached(SkinPackageStoreIoOperation,
                       const std::filesystem::path &) const noexcept = 0;
};

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

enum class SkinRecoveryDisposition : std::uint8_t {
  Recovered,
  AlreadyRecovered,
  ConcurrentCallRejected,
  Failed,
};

struct SkinRecoveryResult {
  SkinRecoveryDisposition disposition = SkinRecoveryDisposition::Failed;
  std::vector<SkinDiagnostic> diagnostics;
};

struct RemovePackageResult {
  bool removed = false;
  bool cancelled = false;
  SkinPackageId package;
  std::vector<SkinDiagnostic> diagnostics;
};

struct PrepareActivationResult {
  std::optional<PreparedSkinActivation> prepared;
  std::optional<ValidatedSkinActivation> previousActivation;
  bool cancelled = false;
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
  virtual SkinValidationResult
  validate(SkinRevisionReadView revision, const SkinEntryId &entry,
           const EntryProfileSettings *desiredSettings, std::stop_token stop,
           const SkinSafetyPolicy &) {
    return validate(std::move(revision), entry, desiredSettings, stop);
  }
};

class SkinPackageStore : public SkinActivationCommitStore {
public:
  SkinPackageStore(SkinStorageRoots, SkinPackageCatalog &, SkinAliasDetector &,
                   ISkinProfileSnapshotProvider &,
                   std::shared_ptr<const SkinPackageStoreIoObserver> = {});

  // Synchronous, exclusive bootstrap. The first call must finish before the
  // operation service is constructed. A later call returns AlreadyRecovered;
  // an overlapping call returns ConcurrentCallRejected. Neither replays files.
  SkinRecoveryResult recoverBeforeServiceStart();
  PreparePackageResult prepareArchive(const std::filesystem::path &zip,
                                      const SkinPackageId &package,
                                      std::stop_token stop,
                                      SkinProgressCallback progress,
                                      SkinSafetyPolicy safetyPolicy =
                                          SkinSafetyPolicy{});
  PreparePackageResult prepareFolder(const std::filesystem::path &folder,
                                     const SkinPackageId &package,
                                     std::stop_token stop,
                                     SkinProgressCallback progress,
                                     SkinSafetyPolicy safetyPolicy =
                                         SkinSafetyPolicy{});
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
  // Strong admission boundary: before returning PendingProfileSave, any
  // exception or typed rejection must retain no owner ticket or prepared
  // activation. After PendingProfileSave is returned, Store owns both and
  // pollPreparedActivationCommit must eventually return a terminal result,
  // acknowledging its distinct owner ticket exactly once before doing so.
  // Callers treat the Store ticket as opaque and never pass it to the owner.
  CommitActivationResult
  beginPreparedActivationCommit(PreparedSkinActivation &&prepared,
                                ISkinProfileSettingsOwner &owner) override;
  CommitActivationResult
  pollPreparedActivationCommit(std::uint64_t ticket,
                               ISkinProfileSettingsOwner &owner) override;
  AcquireActivationResult
  acquireValidatedActivation(const SkinProfileId &profile,
                             const SkinEntryId &entry,
                             std::string_view configurationDigest);
  std::shared_ptr<const SkinPackageCatalogSnapshot>
  catalogSnapshot() const noexcept;
  RemovePackageResult removePackage(const SkinPackageId &package,
                                    std::stop_token stop);
  void removeProfileActivations(const SkinProfileId &profile) override;
  void
  reconcileProfileActivations(std::span<const SkinProfileId> existingProfiles);
  GarbageCollectionResult collectGarbage();

private:
  friend class SkinPackageOperationService;
  bool operationServiceReady() const noexcept;
  bool materializeStableRevision(const std::filesystem::path &revisionRoot,
                                 const std::filesystem::path &destination,
                                 const SkinPackageId &package,
                                 std::string_view expectedDigest,
                                 std::vector<SkinDiagnostic> &diagnostics);

  SkinStorageRoots roots_;
  SkinPackageCatalog &catalog_;
  SkinAliasDetector &aliases_;
  ISkinProfileSnapshotProvider &profileSnapshots_;
  std::shared_ptr<const SkinPackageStoreIoObserver> ioObserver_;
  // 0 = not started, 1 = running, 2 = attempted/completed.
  std::atomic_uint8_t recoveryState_{0};
  std::atomic_bool recoverySucceeded_{false};
  std::atomic_bool poisoned_{false};
  std::map<std::string, SkinRevisionLease, std::less<>> revisionLeases_;
  std::map<std::string, SkinRevisionWeakPin, std::less<>> revisionPins_;
  using ActivationMap =
      std::map<std::string, ValidatedSkinActivation, std::less<>>;
  ActivationMap activations_;
  struct PendingActivationCommit {
    std::uint64_t ownerTicket = 0;
    std::uint64_t sourceGeneration = 0;
    std::uint64_t catalogGeneration = 0;
    SkinProfileId profileId;
    ActivationMap::node_type activationNode;
    ValidatedSkinActivation terminalActivation;
    SkinPackageCatalogSnapshot catalogUpdate;
    bool catalogChanged = false;
  };
  std::map<std::uint64_t, PendingActivationCommit> pendingActivationCommits_;
  std::uint64_t nextActivationCommitTicket_ = 0;
  std::uint64_t stateCatalogGeneration_ = 0;
  std::uint64_t stateSourceGeneration_ = 0;
  bool catalogMutationInFlight_ = false;
  std::mutex stateMutex_;
};

} // namespace skin
