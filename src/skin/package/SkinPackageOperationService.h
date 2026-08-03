#pragma once

#include "SkinPackageStore.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

namespace skin {

class SkinDeferredCleanup {
public:
  SkinDeferredCleanup() noexcept = default;
  explicit SkinDeferredCleanup(std::function<void()> action);
  SkinDeferredCleanup(SkinDeferredCleanup &&) noexcept;
  SkinDeferredCleanup &operator=(SkinDeferredCleanup &&) noexcept;
  SkinDeferredCleanup(const SkinDeferredCleanup &) = delete;
  SkinDeferredCleanup &operator=(const SkinDeferredCleanup &) = delete;
  ~SkinDeferredCleanup();

  void run() noexcept;

private:
  std::function<void()> action_;
};

struct ReconcileProfileActivationsResult {
  bool completed = false;
  std::vector<SkinDiagnostic> diagnostics;
};

using SkinPackageOperationPayload =
    std::variant<PreparePackageResult, PublishPackageResult, ScanPackagesResult,
                 RemovePackageResult, PrepareActivationResult,
                 GarbageCollectionResult, ReconcileProfileActivationsResult>;

struct SkinPackageOperationCompletion {
  std::uint64_t ticket = 0;
  SkinPackageOperationPayload payload;
};

class SkinPackageProgressMailbox {
public:
  SkinProgress snapshot() const noexcept;

private:
  struct State;
  explicit SkinPackageProgressMailbox(std::shared_ptr<State> state);
  void publish(SkinProgress progress) noexcept;
  std::shared_ptr<State> state_;
  friend class SkinPackageOperationService;
};

struct SkinPackageOperationHandle {
  std::uint64_t ticket = 0;
  std::shared_ptr<const SkinPackageProgressMailbox> progress;
};

class SkinPackageOperationService {
public:
  SkinPackageOperationService(SkinPackageStore &, SkinEntryValidator &);
  ~SkinPackageOperationService();

  SkinPackageOperationService(const SkinPackageOperationService &) = delete;
  SkinPackageOperationService &
  operator=(const SkinPackageOperationService &) = delete;

  SkinPackageOperationHandle submitPrepareArchive(std::filesystem::path zip,
                                                  SkinPackageId package,
                                                  SkinDeferredCleanup cleanup);
  SkinPackageOperationHandle submitPrepareFolder(std::filesystem::path folder,
                                                 SkinPackageId package,
                                                 SkinDeferredCleanup cleanup);
  SkinPackageOperationHandle
  submitPublish(PreparedPackage prepared,
                PackageCollisionPolicy collisionPolicy,
                ProfileInventorySnapshot inventory);
  SkinPackageOperationHandle submitRescan(ProfileInventorySnapshot inventory);
  SkinPackageOperationHandle submitRemove(SkinPackageId package);
  SkinPackageOperationHandle
  submitPrepareActivation(VersionedSkinProfileSettings base, SkinEntryId entry,
                          SkinProfileSettings candidate);
  SkinPackageOperationHandle submitGarbageCollection();
  SkinPackageOperationHandle
  submitReconcileProfileActivations(std::vector<SkinProfileId> profiles);
  AcquireActivationResult
  acquireValidatedActivation(const SkinProfileId &profile,
                             const SkinEntryId &entry,
                             std::string_view configurationDigest);
  std::shared_ptr<const SkinPackageCatalogSnapshot>
  catalogSnapshot() const noexcept;
  std::optional<SkinPackageOperationCompletion> poll(std::uint64_t ticket);
  void cancelAndDetach(std::uint64_t ticket) noexcept;
  void discardPrepared(PreparedPackage prepared,
                       SkinDeferredCleanup cleanup = {});
  void shutdown() noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace skin
