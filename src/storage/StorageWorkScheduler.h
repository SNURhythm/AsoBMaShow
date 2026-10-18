#pragma once
#include "StoragePreflight.h"
#include <condition_variable>
#include <atomic>
#include <mutex>
#include <thread>

namespace storage_management {
class StorageWorkBudget {
public:
  using MediaKind = asobmshow::chart_assets::MediaKind;
  class Lease {
  public:
    Lease(Lease &&) noexcept;
    Lease &operator=(Lease &&) = delete;
    ~Lease();
  private:
    friend class StorageWorkBudget;
    Lease(StorageWorkBudget *owner, MediaKind kind, std::uint64_t bytes) : owner_(owner), kind_(kind), bytes_(bytes) {}
    StorageWorkBudget *owner_;
    MediaKind kind_;
    std::uint64_t bytes_;
  };
  StorageWorkBudget(StorageExecutionLimits, std::function<std::optional<std::uint64_t>()>);
  std::optional<Lease> Acquire(MediaKind, std::uint64_t temporaryBytes, Checkpoint);
  void StopForRecovery();
  bool RecoveryRequired() const { return recoveryRequired_; }
private:
  StorageExecutionLimits limits_;
  std::function<std::optional<std::uint64_t>()> available_;
  std::mutex mutex_;
  std::condition_variable changed_;
  std::size_t audio_ = 0, video_ = 0;
  std::uint64_t reserved_ = 0;
  std::atomic<bool> recoveryRequired_ = false;
};
// Each directory has one worker; ancestors settle before descendant admission.
// Work/checkpoints may run concurrently. Progress delivery is serialized.
using DirectoryWork = std::function<StorageReport(const StorageDirectorySummary &, StorageWorkBudget &, Checkpoint, ProgressCallback)>;
using StorageWorkerLauncher = std::function<std::thread(std::function<void()>)>;
StorageReport RunStorageDirectories(const StoragePreflight &, StorageExecutionLimits,
    std::function<std::optional<std::uint64_t>()>, DirectoryWork, Checkpoint = {}, ProgressCallback = {}, StorageWorkerLauncher = {});
}
