#include "ReplayExportJob.h"

#include <exception>
#include <utility>

namespace replay {

bool ReplayExportJob::tryBegin() { return !active_.exchange(true); }

void ReplayExportJob::start(ReplayVideoExportOptions options, Work work) {
  if (worker_.joinable()) {
    worker_.join();
  }
  {
    std::lock_guard lock(progressMutex_);
    progress_.reset();
  }
  cancelled_ = false;
  worker_ = std::jthread(
      [this, options = std::move(options), work = std::move(work)](
          const std::stop_token &stop) mutable {
        ReplayVideoExportResult result;
        try {
          options.stop = stop;
          options.progressCallback = [this](const ReplayVideoExportProgress &progress) {
            publishProgress(progress);
          };
          result = work(options, cancelled_);
        } catch (const std::exception &error) {
          result = {.success = false, .message = error.what()};
        } catch (...) {
          result = {.success = false,
                    .message = "Unexpected replay export failure"};
        }
        std::lock_guard lock(resultMutex_);
        result_ = std::move(result);
      });
}

void ReplayExportJob::cancelAndWait() {
  cancelled_ = true;
  if (worker_.joinable()) {
    worker_.request_stop();
    worker_.join();
  }
}

void ReplayExportJob::reset() {
  cancelAndWait();
  {
    std::lock_guard lock(progressMutex_);
    progress_.reset();
  }
  {
    std::lock_guard lock(resultMutex_);
    result_.reset();
  }
  active_ = false;
}

void ReplayExportJob::publishProgress(
    const ReplayVideoExportProgress &progress) {
  std::lock_guard lock(progressMutex_);
  progress_ = progress;
}

std::optional<ReplayVideoExportProgress> ReplayExportJob::takeProgress() {
  std::lock_guard lock(progressMutex_);
  return std::exchange(progress_, std::nullopt);
}

std::optional<ReplayVideoExportResult> ReplayExportJob::takeResult() {
  std::optional<ReplayVideoExportResult> result;
  {
    std::lock_guard lock(resultMutex_);
    result = std::exchange(result_, std::nullopt);
  }
  if (result) {
    if (worker_.joinable()) {
      worker_.join();
    }
    active_ = false;
  }
  return result;
}

} // namespace replay
