#include "ReplayRecordTask.h"

#include <utility>

ReplayRecordTask::~ReplayRecordTask() { cancelAndWait(); }

void ReplayRecordTask::start(Work work) {
  if (worker_.joinable()) {
    worker_.join();
  }
  auto cancelled = std::make_shared<std::atomic_bool>(false);
  {
    std::lock_guard<std::mutex> lock(completionMutex_);
    pendingCompletion_ = {};
    completionPublished_ = false;
    cancelled_ = cancelled;
  }
  active_.store(true, std::memory_order_release);
  try {
    worker_ = std::jthread(
        [this, work = std::move(work), cancelled](std::stop_token stop) mutable {
          if (!stop.stop_requested() && !cancelled->load()) {
            work(cancelled);
          }
          // Even a cooperative cancellation needs UI delivery to release the
          // scene's busy state. cancelAndWait discards this notification.
          std::lock_guard lock(completionMutex_);
          if (!completionPublished_) {
            pendingCompletion_ = [] {};
            completionPublished_ = true;
          }
        });
  } catch (...) {
    cancelAndWait();
    throw;
  }
}

void ReplayRecordTask::publish(Completion completion) {
  std::lock_guard<std::mutex> lock(completionMutex_);
  if (cancelled_ == nullptr || cancelled_->load()) {
    return;
  }
  pendingCompletion_ = std::move(completion);
  completionPublished_ = true;
}

ReplayRecordTask::Completion ReplayRecordTask::takeCompletion() {
  Completion completion;
  {
    std::lock_guard<std::mutex> lock(completionMutex_);
    completion = std::move(pendingCompletion_);
    pendingCompletion_ = {};
  }
  if (!completion) {
    return {};
  }
  if (worker_.joinable()) {
    worker_.join();
  }
  active_.store(false, std::memory_order_release);
  return completion;
}

void ReplayRecordTask::cancelAndWait() {
  {
    std::lock_guard<std::mutex> lock(completionMutex_);
    if (cancelled_ != nullptr) {
      cancelled_->store(true, std::memory_order_release);
    }
  }
  if (worker_.joinable()) {
    worker_.request_stop();
    worker_.join();
  }
  active_.store(false, std::memory_order_release);
  std::lock_guard<std::mutex> lock(completionMutex_);
  pendingCompletion_ = {};
}

bool ReplayRecordTask::active() const noexcept {
  return active_.load(std::memory_order_acquire);
}
