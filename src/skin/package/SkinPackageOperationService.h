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
  // Present only when ticket is zero. Rejection returns the exact cleanup
  // ownership without invoking it or destroying its captures in the service.
  std::optional<SkinDeferredCleanup> rejectedCleanup;
};

struct RejectedPreparedDisposal {
  PreparedPackage prepared;
  SkinDeferredCleanup cleanup;
};

#if defined(ASOBMASHOW_SKIN_OPERATION_SERVICE_TESTING)
class SkinPackageOperationTestObserver {
public:
  virtual ~SkinPackageOperationTestObserver() = default;
  virtual void beforeCompletion(std::uint64_t ticket) const noexcept = 0;
  virtual void completed(std::uint64_t ticket) const noexcept = 0;
  virtual void disposing(std::uint64_t ticket) const noexcept = 0;
};
#endif

class SkinPackageOperationService {
public:
  // The store's exclusive recoverBeforeServiceStart() bootstrap must have
  // completed before this serialized filesystem service is constructed.
  SkinPackageOperationService(
      SkinPackageStore &, SkinEntryValidator &
#if defined(ASOBMASHOW_SKIN_OPERATION_SERVICE_TESTING)
      ,
      std::shared_ptr<const SkinPackageOperationTestObserver> observer = {}
#endif
  );
  ~SkinPackageOperationService();

  SkinPackageOperationService(const SkinPackageOperationService &) = delete;
  SkinPackageOperationService &
  operator=(const SkinPackageOperationService &) = delete;

  // Tickets are nonzero, process-monotonic, and never reused. Success consumes
  // cleanup ownership. A zero-ticket handle means the service is closed or at
  // its bounded retention limit and transfers cleanup back in rejectedCleanup.
  [[nodiscard]] SkinPackageOperationHandle
  submitPrepareArchive(std::filesystem::path zip, SkinPackageId package,
                       SkinDeferredCleanup cleanup);
  [[nodiscard]] SkinPackageOperationHandle
  submitPrepareFolder(std::filesystem::path folder, SkinPackageId package,
                      SkinDeferredCleanup cleanup);
  [[nodiscard]] SkinPackageOperationHandle
  submitPublish(PreparedPackage prepared,
                PackageCollisionPolicy collisionPolicy,
                ProfileInventorySnapshot inventory);
  [[nodiscard]] SkinPackageOperationHandle
  submitRescan(ProfileInventorySnapshot inventory);
  [[nodiscard]] SkinPackageOperationHandle submitRemove(SkinPackageId package);
  [[nodiscard]] SkinPackageOperationHandle
  submitPrepareActivation(VersionedSkinProfileSettings base, SkinEntryId entry,
                          SkinProfileSettings candidate);
  [[nodiscard]] SkinPackageOperationHandle submitGarbageCollection();
  [[nodiscard]] SkinPackageOperationHandle
  submitReconcileProfileActivations(std::vector<SkinProfileId> profiles);
  AcquireActivationResult
  acquireValidatedActivation(const SkinProfileId &profile,
                             const SkinEntryId &entry,
                             std::string_view configurationDigest);
  std::shared_ptr<const SkinPackageCatalogSnapshot>
  catalogSnapshot() const noexcept;
  std::optional<SkinPackageOperationCompletion> poll(std::uint64_t ticket);
  void cancelAndDetach(std::uint64_t ticket) noexcept;
  // Success returns nullopt and transfers both capabilities to the worker.
  // Rejection returns both intact without running cleanup or destroying
  // prepared staging on the caller's behalf.
  [[nodiscard]] std::optional<RejectedPreparedDisposal>
  discardPrepared(PreparedPackage prepared, SkinDeferredCleanup cleanup = {});
  void shutdown() noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace skin
