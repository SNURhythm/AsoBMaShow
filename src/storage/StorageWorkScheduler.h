#pragma once
#include "StoragePreflight.h"
#include <condition_variable>
#include <atomic>
#include <mutex>
#include <thread>
#include <future>
#include <span>

namespace storage_management {
class StorageWorkBudget {
public:
  using MediaKind = asobmshow::chart_assets::MediaKind;
  class Lease {
  public:
    Lease(Lease &&) noexcept;
    Lease &operator=(Lease &&) = delete;
    ~Lease();
    // Encoded output still occupies scratch until the lease is destroyed.
    void ReleaseEncoder();
    bool GrowTo(std::uint64_t temporaryBytes);
  private:
    friend class StorageWorkBudget;
    Lease(StorageWorkBudget *owner, MediaKind kind, std::uint64_t bytes) : owner_(owner), kind_(kind), bytes_(bytes) {}
    StorageWorkBudget *owner_;
    MediaKind kind_;
    std::uint64_t bytes_;
    bool encoding_ = true;
  };
  StorageWorkBudget(StorageExecutionLimits, std::function<std::optional<std::uint64_t>()>);
  std::optional<Lease> Acquire(MediaKind, std::uint64_t temporaryBytes, Checkpoint);
  // A busy caller must drain its own completed work before retrying.
  std::optional<Lease> TryAcquire(MediaKind, std::uint64_t temporaryBytes, bool &waiting);
  void StopForRecovery();
  bool RecoveryRequired() const { return recoveryRequired_; }
private:
  StorageExecutionLimits limits_;
  std::function<std::optional<std::uint64_t>()> available_;
  std::mutex mutex_;
  std::condition_variable changed_;
  std::size_t audio_ = 0, video_ = 0, leases_ = 0;
  std::uint64_t reserved_ = 0;
  std::atomic<bool> recoveryRequired_ = false;
};
// The future joins before its reservation is destroyed, including unwinding
// after a later launch fails. Only a bounded admitted job creates a thread.
class StorageEncodingJob {
public:
  StorageEncodingJob(StorageWorkBudget::Lease lease, std::function<EncodeResult()> work)
      : StorageEncodingJob(std::make_shared<StorageWorkBudget::Lease>(std::move(lease)), std::move(work)) {}
  StorageEncodingJob(std::shared_ptr<StorageWorkBudget::Lease> reservation, std::function<EncodeResult()> work)
      : reservation_(std::move(reservation)),
        future_(std::async(std::launch::async, [reservation = reservation_, work = std::move(work)] {
          struct Release { StorageWorkBudget::Lease &lease; ~Release() { lease.ReleaseEncoder(); } } release{*reservation};
          return work();
        })) {}
  bool Ready() const { return future_.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }
  void Wait() const { future_.wait_for(std::chrono::milliseconds(10)); }
  EncodeResult Take() { return future_.get(); }
private:
  std::shared_ptr<StorageWorkBudget::Lease> reservation_;
  std::future<EncodeResult> future_;
};
// Each directory or archive has one owner; ancestors settle before descendants.
// Work/checkpoints may run concurrently. Progress delivery is serialized.
using DirectoryWork = std::function<StorageReport(const StorageDirectorySummary &, StorageWorkBudget &, Checkpoint, ProgressCallback)>;
using StorageWorkerLauncher = std::function<std::thread(std::function<void()>)>;
StorageReport RunStorageDirectories(const StoragePreflight &, StorageExecutionLimits,
    std::function<std::optional<std::uint64_t>()>, DirectoryWork, Checkpoint = {}, ProgressCallback = {}, StorageWorkerLauncher = {},
    std::span<const StorageDirectorySummary> additionalOwners = {});
}
