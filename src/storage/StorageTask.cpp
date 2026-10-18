#include "StorageTask.h"

namespace storage_management {
StorageTask::StorageTask(StorageTaskDependencies dependencies) : dependencies_(std::move(dependencies)) {}
StorageTask::~StorageTask() { Shutdown(); }
bool StorageTask::Start(OptimizationRequest &&request) {
  std::lock_guard lifecycle(lifecycleMutex_);
  if (stopped_ || guard_ || worker_.joinable()) return false;
  std::string error;
  auto admission = dependencies_.admit(request, error);
  if (!admission) {
    std::lock_guard state(stateMutex_);
    snapshot_.admissionError = std::move(error);
    return false;
  }
  guard_.emplace(std::move(*admission));
  cancelled_ = false;
  {
    std::lock_guard state(stateMutex_);
    snapshot_ = {.running = true};
    snapshot_.progress.phase = StoragePhase::Prepare;
    if (request.preflight) snapshot_.progress.filesTotal = request.preflight->eligibleCount;
    finished_ = false;
  }
  try {
    worker_ = std::thread([this, request = std::move(request)]() mutable {
      StorageReport report;
      try {
        report = dependencies_.run(std::move(request), [this] { return !cancelled_.load(); },
            [this](const StorageProgress &progress) {
              std::lock_guard state(stateMutex_);
              snapshot_.progress = progress;
            });
      } catch (...) {
        // An exception could occur after a durable journal was written. Keep
        // admission closed until startup recovery establishes a safe state.
        report.failed = 1;
        report.recoveryRequired = true;
      }
      std::lock_guard state(stateMutex_);
      completion_ = std::move(report);
      finished_ = true;
    });
  } catch (...) {
    guard_.reset();
    std::lock_guard state(stateMutex_);
    snapshot_.running = false;
    throw;
  }
  return true;
}
void StorageTask::RequestCancel() {
  cancelled_ = true;
  {
    std::lock_guard state(stateMutex_);
    if (snapshot_.running) snapshot_.cancelling = true;
  }
  if (dependencies_.wake) dependencies_.wake();
}
StorageTaskSnapshot StorageTask::Snapshot() const {
  std::lock_guard state(stateMutex_);
  return snapshot_;
}
std::optional<StorageReport> StorageTask::takeCompletionLocked() {
  {
    std::lock_guard state(stateMutex_);
    if (!finished_ || !completion_) return {};
  }
  if (worker_.joinable()) worker_.join();
  std::optional<StorageReport> report;
  {
    std::lock_guard state(stateMutex_);
    report = std::move(completion_);
    completion_.reset();
  }
  try {
    guard_->Complete(*report);
  } catch (...) {
    report->recoveryRequired = true;
    ++report->failed;
  }
  if (!report->recoveryRequired) guard_.reset();
  {
    std::lock_guard state(stateMutex_);
    snapshot_.running = false;
    snapshot_.cancelling = false;
    snapshot_.recoveryRequired = report->recoveryRequired;
    snapshot_.lastReport = std::make_shared<const StorageReport>(*report);
  }
  return report;
}
std::optional<StorageReport> StorageTask::TakeCompletion() {
  std::lock_guard lifecycle(lifecycleMutex_);
  return takeCompletionLocked();
}
std::optional<StorageReport> StorageTask::CancelAndWait() {
  std::lock_guard lifecycle(lifecycleMutex_);
  RequestCancel();
  if (worker_.joinable()) worker_.join();
  return takeCompletionLocked();
}
void StorageTask::Shutdown() {
  std::lock_guard lifecycle(lifecycleMutex_);
  if (stopped_) return;
  stopped_ = true;
  RequestCancel();
  if (worker_.joinable()) worker_.join();
  takeCompletionLocked();
  guard_.reset();
}
}
