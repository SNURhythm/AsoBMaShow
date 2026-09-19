#include "IrUploadPreparationTask.h"

#include <utility>

namespace ir_uploads {

PreparationTask::~PreparationTask() { stopAndWait(); }

bool PreparationTask::start(std::vector<ir::IrUploadCandidate> candidates,
                            PreparationDependencies dependencies) {
  if (worker_.joinable()) {
    return false;
  }
  {
    std::lock_guard lock(mutex_);
    if (pending_.completion) {
      return false;
    }
    pending_ = {};
  }
  enqueueGate_ = std::make_shared<DurableEnqueueGate>();
  try {
    worker_ = std::jthread(
        [this, gate = enqueueGate_, candidates = std::move(candidates),
         dependencies = std::move(dependencies)](const std::stop_token &token) mutable {
          dependencies.progress = [this](std::size_t completed, std::size_t total) {
            std::lock_guard lock(mutex_);
            pending_.progress = {completed, total};
          };
          auto result = prepareSelectedCandidates(candidates, token, dependencies, gate);
          std::lock_guard lock(mutex_);
          pending_.completion = std::move(result);
        });
  } catch (...) {
    enqueueGate_.reset();
    throw;
  }
  return true;
}

bool PreparationTask::hasWorker() const noexcept { return worker_.joinable(); }

PreparationTask::Updates PreparationTask::takeUpdates() {
  Updates updates;
  {
    std::lock_guard lock(mutex_);
    updates = std::exchange(pending_, {});
  }
  if (updates.completion) {
    if (worker_.joinable()) {
      worker_.join();
    }
    enqueueGate_.reset();
  }
  return updates;
}

void PreparationTask::stopAndWait() {
  if (!worker_.joinable()) {
    return;
  }
  enqueueGate_->requestCancellation();
  worker_.request_stop();
  worker_.join();
  enqueueGate_.reset();
}

void PreparationTask::reset() {
  stopAndWait();
  std::lock_guard lock(mutex_);
  pending_ = {};
}

} // namespace ir_uploads
