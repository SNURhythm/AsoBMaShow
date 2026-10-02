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
    retryRequest_.reset();
    retryRequested_ = false;
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

BmsSearchDownloadRetryCallback FindBmsTask::retryCallback() {
  return [this](const std::string &message, bool canResume) {
    std::unique_lock lock(mutex_);
    if (cancelled_.load() || !running_) return false;
    retryRequest_ = RetryRequest{message, canResume};
    retryRequested_ = false;
    pending_.retryChanged = true;
    retryCondition_.wait(lock, [this] {
      return cancelled_.load() || retryRequested_;
    });
    const bool retry = retryRequested_ && !cancelled_.load();
    retryRequest_.reset();
    retryRequested_ = false;
    pending_.retryChanged = true;
    return retry;
  };
}

std::optional<FindBmsTask::RetryRequest> FindBmsTask::retryRequest() const {
  std::lock_guard lock(mutex_);
  if (cancelled_.load() || retryRequested_) return std::nullopt;
  return retryRequest_;
}

bool FindBmsTask::retryDownload() {
  {
    std::lock_guard lock(mutex_);
    if (!retryRequest_ || retryRequested_ || cancelled_.load()) return false;
    retryRequested_ = true;
    pending_.retryChanged = true;
  }
  retryCondition_.notify_all();
  return true;
}

void FindBmsTask::requestCancel() {
  {
    std::lock_guard lock(mutex_);
    cancelled_ = true;
    pending_.retryChanged = true;
  }
  retryCondition_.notify_all();
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
  retryRequest_.reset();
  retryRequested_ = false;
}
