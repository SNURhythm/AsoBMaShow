#pragma once

#include "SkinPackageStore.h"

#include <cstddef>
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

struct RejectedPreparedDisposal {
  PreparedPackage prepared;
  SkinDeferredCleanup cleanup;
};

class SkinPackageOperationService;

// Pre-acquired, allocation-free admission for one rejected prepared package or
// one cleanup-only capability. Reserve before starting an import, while no
// staging capability exists. A live reservation guarantees that either
// transfer remains nonblocking and is drained by the service worker even when
// normal operation slots are full or shutdown starts concurrently. Shutdown
// waits for live reservations to transfer or be released; reservation owners
// must therefore be torn down before the service.
class SkinPreparedDisposalReservation {
public:
  SkinPreparedDisposalReservation(
      SkinPreparedDisposalReservation &&other) noexcept;
  SkinPreparedDisposalReservation &
  operator=(SkinPreparedDisposalReservation &&other) noexcept;
  SkinPreparedDisposalReservation(
      const SkinPreparedDisposalReservation &) = delete;
  SkinPreparedDisposalReservation &
  operator=(const SkinPreparedDisposalReservation &) = delete;
  ~SkinPreparedDisposalReservation();

  // A valid reservation transfers ownership and returns nullopt. A returned
  // value is an explicit fail-closed result for invalid/moved-from misuse; the
  // service never destroys it on the caller's behalf.
  [[nodiscard]] std::optional<RejectedPreparedDisposal>
  transfer(RejectedPreparedDisposal disposal) && noexcept;
  // Used when prepare admission rejects before PreparedPackage exists. A
  // valid reservation transfers the exact cleanup capability and returns
  // nullopt; invalid/moved-from misuse returns it intact without running it.
  [[nodiscard]] std::optional<SkinDeferredCleanup>
  transfer(SkinDeferredCleanup cleanup) && noexcept;
  [[nodiscard]] explicit operator bool() const noexcept;

private:
  SkinPreparedDisposalReservation(SkinPackageOperationService *,
                                  std::size_t) noexcept;
  void release() noexcept;

  SkinPackageOperationService *service_ = nullptr;
  std::size_t slot_ = 0;

  friend class SkinPackageOperationService;
};

struct SkinPackageOperationHandle {
  std::uint64_t ticket = 0;
  std::shared_ptr<const SkinPackageProgressMailbox> progress;
  // Exactly one rejection field can be present when ticket is zero. Ordinary
  // request rejection returns cleanup alone; publish rejection returns both
  // the exact prepared staging capability and its cleanup.
  std::optional<SkinDeferredCleanup> rejectedCleanup;
  std::optional<RejectedPreparedDisposal> rejectedPrepared;
};

#if defined(ASOBMASHOW_SKIN_OPERATION_SERVICE_TESTING)
class SkinPackageOperationTestObserver {
public:
  virtual ~SkinPackageOperationTestObserver() = default;
  virtual bool failAdmissionAllocation() const noexcept = 0;
  virtual bool failPublishPackageCopy() const noexcept { return false; }
  virtual bool failPublishTerminalAllocation() const noexcept { return false; }
  virtual bool cancelBeforeExecution(std::uint64_t) const noexcept {
    return false;
  }
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
  // its bounded retention limit and transfers every supplied capability back
  // in the matching rejection field.
  [[nodiscard]] SkinPackageOperationHandle
  submitPrepareArchive(std::filesystem::path zip, SkinPackageId package,
                       SkinDeferredCleanup cleanup,
                       SkinSafetyPolicy safetyPolicy = SkinSafetyPolicy{});
  [[nodiscard]] SkinPackageOperationHandle
  submitPrepareFolder(std::filesystem::path folder, SkinPackageId package,
                      SkinDeferredCleanup cleanup,
                      SkinSafetyPolicy safetyPolicy = SkinSafetyPolicy{});
  [[nodiscard]] SkinPackageOperationHandle submitPublish(
      PreparedPackage prepared, PackageCollisionPolicy collisionPolicy,
      ProfileInventorySnapshot inventory, SkinDeferredCleanup cleanup = {});
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
  // Reservation admission is bounded but occurs before a caller can own a
  // rejected PreparedPackage. Once admitted, transfer of that one capability
  // is guaranteed while the service object remains alive, including during a
  // concurrent shutdown.
  [[nodiscard]] std::optional<SkinPreparedDisposalReservation>
  reservePreparedDisposal() noexcept;
  [[nodiscard]] std::optional<SkinPackageOperationCompletion>
  poll(std::uint64_t ticket);
  void cancelAndDetach(std::uint64_t ticket) noexcept;
  // Success returns nullopt and transfers both capabilities to the worker.
  // Rejection returns both intact without running cleanup or destroying
  // prepared staging on the caller's behalf.
  [[nodiscard]] std::optional<RejectedPreparedDisposal>
  discardPrepared(PreparedPackage prepared, SkinDeferredCleanup cleanup = {});
  void shutdown() noexcept;

private:
  friend class SkinPreparedDisposalReservation;
  [[nodiscard]] std::optional<RejectedPreparedDisposal>
  transferReservedPreparedDisposal(std::size_t,
                                   RejectedPreparedDisposal) noexcept;
  [[nodiscard]] std::optional<SkinDeferredCleanup>
  transferReservedPreparedCleanup(std::size_t, SkinDeferredCleanup) noexcept;
  void releasePreparedDisposalReservation(std::size_t) noexcept;

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace skin
