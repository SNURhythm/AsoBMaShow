#include "FindBmsTask.h"

#include <utility>

FindBmsTask::~FindBmsTask() { stopAndWait(); }

bool FindBmsTask::start(Work work) {
  {
    std::lock_guard lock(mutex_);
    if (running_ || pending_.result.has_value()) {
      return false;
    }
    running_ = true;
    pending_ = {};
  }
  if (worker_.joinable()) {
    worker_.join();
  }
  cancelled_ = false;
  try {
    worker_ = std::jthread([this, work = std::move(work)](const std::stop_token &token) {
      auto progress = [this](const BmsSearchDownloadProgress &event) {
        std::lock_guard lock(mutex_);
        pending_.progress.push_back(event);
        while (pending_.progress.size() > kMaxPendingProgressEvents) {
          pending_.progress.pop_front();
        }
      };
      if (token.stop_requested()) {
        cancelled_ = true;
      }
      auto result = work(cancelled_, std::move(progress));
      std::lock_guard lock(mutex_);
      pending_.result = std::move(result);
      running_ = false;
    });
  } catch (...) {
    std::lock_guard lock(mutex_);
    running_ = false;
    throw;
  }
  return true;
}

bool FindBmsTask::running() const {
  std::lock_guard lock(mutex_);
  return running_ || pending_.result.has_value();
}

FindBmsTask::Updates FindBmsTask::takeUpdates() {
  std::lock_guard lock(mutex_);
  return std::exchange(pending_, {});
}

void FindBmsTask::requestCancel() {
  cancelled_ = true;
  if (worker_.joinable()) {
    worker_.request_stop();
  }
}

void FindBmsTask::stopAndWait() {
  requestCancel();
  if (worker_.joinable()) {
    worker_.join();
  }
  std::lock_guard lock(mutex_);
  pending_ = {};
}
