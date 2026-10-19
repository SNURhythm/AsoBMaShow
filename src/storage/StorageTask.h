#pragma once
#include "StorageMaintenanceGuard.h"
#include "StorageOptimizer.h"
#include <atomic>
#include <thread>

namespace storage_management {
struct StorageTaskSnapshot {
  bool running = false;
  bool cancelling = false;
  bool recoveryRequired = false;
  StorageProgress progress;
  std::string admissionError;
  std::shared_ptr<const StorageReport> lastReport;
};
struct StorageTaskDependencies {
  std::function<std::optional<StorageMaintenanceGuard>(const OptimizationRequest &, std::string &)> admit;
  std::function<StorageReport(OptimizationRequest, Checkpoint, ProgressCallback)> run;
  std::function<void()> wake;
};
class StorageTask {
public:
  explicit StorageTask(StorageTaskDependencies);
  ~StorageTask();
  // Rejected admission leaves the reviewed request owned by the caller.
  bool Start(OptimizationRequest &&);
  void RequestCancel();
  StorageTaskSnapshot Snapshot() const;
  // Application-thread handoff; reservation remains active until this call.
  std::optional<StorageReport> TakeCompletion();
  std::optional<StorageReport> CancelAndWait();
  void Shutdown();
private:
  std::optional<StorageReport> takeCompletionLocked();
  StorageTaskDependencies dependencies_;
  mutable std::mutex lifecycleMutex_, stateMutex_;
  std::atomic<bool> cancelled_{false};
  bool stopped_ = false;
  bool finished_ = false;
  StorageTaskSnapshot snapshot_;
  std::optional<StorageReport> completion_;
  std::optional<StorageMaintenanceGuard> guard_;
  std::thread worker_;
};
}
