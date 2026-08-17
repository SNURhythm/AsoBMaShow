#pragma once

#include "GameplaySkinActivationRequest.h"
#include "SkinCommitCoordinator.h"
#include "SkinConfigurationWriteQueue.h"
#include "SkinStoragePaths.h"
#include "beatoraja/SkinDiagnosticHistory.h"
#include "package/SkinPackageOperationService.h"
#include "../scene/play/PlayfieldPresentationCoordinator.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

namespace skin {

enum class SkinRescanReason : std::uint8_t { Explicit };

struct GameplaySkinLifecycleOperationSubmission {
  std::uint64_t ticket = 0;
  // The lifecycle is also linked into focused tests without the operation
  // service implementation. Expose a value getter at this boundary rather
  // than making that test-only linkage depend on the worker mailbox type.
  std::function<SkinProgress()> progress;
  std::vector<SkinDiagnostic> diagnostics;
};

using GameplaySkinLifecycleOperationPayload =
    std::variant<std::monostate, PrepareActivationResult,
                 ReconcileProfileActivationsResult, ScanPackagesResult>;

struct GameplaySkinLifecycleOperationCompletion {
  std::uint64_t ticket = 0;
  GameplaySkinLifecycleOperationPayload payload;
};

// Injectable boundary used by the lifecycle state machine. Production's
// concrete constructor below binds this surface to the one app-owned service,
// profile owner/provider, commit coordinator, and writer queue.
struct GameplaySkinLifecycleDependencies {
  SkinStorageRoots roots;
  std::function<bool()> ensureVisibleRoot;
  std::function<VersionedSkinProfileSettings(const SkinProfileId &)>
      snapshotProfile;
  std::function<AcquireActivationResult(const SkinProfileId &,
                                        const SkinEntryId &, std::string_view)>
      acquireActivation;
  std::function<GameplaySkinLifecycleOperationSubmission(
      VersionedSkinProfileSettings, SkinEntryId, SkinProfileSettings)>
      submitPrepareActivation;
  std::function<GameplaySkinLifecycleOperationSubmission(
      std::vector<SkinProfileId>)>
      submitReconcileProfileActivations;
  std::function<GameplaySkinLifecycleOperationSubmission(
      ProfileInventorySnapshot)>
      submitRescan;
  std::function<std::optional<GameplaySkinLifecycleOperationCompletion>(
      std::uint64_t)>
      pollOperation;
  std::function<void(std::uint64_t)> cancelOperation;
  std::function<std::uint64_t()> beginProfileInventory;
  std::function<std::optional<AllSkinProfileSnapshotsResult>(std::uint64_t)>
      pollProfileInventory;
  std::function<void(std::uint64_t)> cancelProfileInventory;
  std::function<SkinActivationSubmissionResult(PreparedSkinActivation)>
      submitActivation;
  std::function<std::vector<SkinActivationCompletion>()>
      takeActivationCompletions;
  std::function<void()> pollCommitCoordinator;
  std::function<std::vector<VersionedSkinProfileSettings>()>
      takeRevalidationRequests;
  std::function<SkinProfileCommitSubmissionResult(
      const VersionedSkinProfileSettings &, SkinProfileSettings)>
      submitProfileSettings;
  std::function<std::vector<SkinProfileCommitCompletion>()>
      takeProfileCompletions;
  std::function<std::vector<SkinConfigurationWriteRequest>()>
      drainConfigurationWrites;
  std::function<void()> closeConfigurationWrites;
  std::function<std::shared_ptr<const SkinPackageCatalogSnapshot>()>
      catalogSnapshot;
  std::function<void()> detachCommitClient;
  std::function<void(SkinDiagnosticHistoryRecord)> appendHistory;
};

class GameplaySkinLifecycle {
public:
  static constexpr std::size_t maxPendingSessionWrites = 256;

  GameplaySkinLifecycle(SkinStorageRoots, SkinPackageOperationService &,
                        SkinDiagnosticHistory &, SkinConfigurationWriteQueue &,
                        ISkinProfileSettingsOwner &,
                        ISkinProfileSnapshotProvider &, SkinCommitCoordinator &,
                        SkinActivationClientId lifecycleClientId);
  explicit GameplaySkinLifecycle(GameplaySkinLifecycleDependencies);
  ~GameplaySkinLifecycle();

  GameplaySkinLifecycle(const GameplaySkinLifecycle &) = delete;
  GameplaySkinLifecycle &operator=(const GameplaySkinLifecycle &) = delete;

  void startAfterProfileInitialization(SkinProfileId);
  void profileChanged(SkinProfileId);
  void requestRescan(SkinRescanReason);
  void cancelRescan() noexcept;
  [[nodiscard]] SkinRescanProgress rescanProgress() const noexcept;
  void requestRevalidation(const SkinEntryId &);
  GameplayViewportPersistenceResult
  requestViewportReset(const PlaySkinSessionIdentity &, ViewportSettings);
  void poll();
  [[nodiscard]] std::shared_ptr<const SkinPackageCatalogSnapshot>
  catalogSnapshot() const noexcept;
  [[nodiscard]] GameplaySkinAcquisition
  acquireForNextChart(int keyMode = 7);
  void recordPresentationFailure(const PresentationFailure &);
  void shutdown() noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace skin
