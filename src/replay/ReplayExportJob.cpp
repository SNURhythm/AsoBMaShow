#include "ReplayExportJob.h"

#include <exception>
#include "../targets.h"
#if TARGET_OS_IPHONE
#include "../platform/IOSApplicationRuntime.h"
#include "../RAII.h"
#endif
#include <utility>

namespace replay {

ReplayExportJob::ReplayExportJob() {
#if TARGET_OS_IPHONE
  ownerExecutor_ = [](auto work, auto) { PostIOSApplicationWork(std::move(work)); };
#endif
}

void ReplayExportJob::execute(ReplayVideoExportOptions options, Work work,
                              std::stop_token stop) {
  ReplayVideoExportResult result;
  std::stop_callback cancel(stop, [this] { cancelled_ = true; });
  try {
#if TARGET_OS_IPHONE
    auto overlay = makeScopeExit([] { EndIOSReplayExport(); });
    BeginIOSReplayExport(ownerStop_);
#endif
    options.stop = stop;
    options.progressCallback = [this](const ReplayVideoExportProgress &progress) {
      publishProgress(progress);
#if TARGET_OS_IPHONE
      UpdateIOSReplayExport(progress.fraction, progress.message);
#endif
    };
    result = work(options, cancelled_);
  } catch (const std::exception &error) {
    result = {.success = false, .message = error.what()};
  } catch (...) {
    result = {.success = false, .message = "Unexpected replay export failure"};
  }
  publishResult(std::move(result));
}

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
  try {
    if (ownerExecutor_) {
      ownerStop_ = std::stop_source{};
      pending_ = std::make_shared<int>(0);
      const std::weak_ptr<int> lifetime = pending_;
      ownerExecutor_([this, lifetime, options = std::move(options), work = std::move(work)]() mutable {
        if (lifetime.expired()) return;
        execute(std::move(options), std::move(work), ownerStop_.get_token());
        pending_.reset();
      }, ownerStop_);
    } else {
      worker_ = std::jthread(
          [this, options = std::move(options), work = std::move(work)](
              const std::stop_token &stop) mutable {
            execute(std::move(options), std::move(work), stop);
          });
    }
  } catch (const std::exception &error) {
    pending_.reset();
    publishResult({.success = false, .message = error.what()});
  } catch (...) {
    pending_.reset();
    publishResult({.success = false,
                   .message = "Unexpected replay export failure"});
  }
}

void ReplayExportJob::cancelAndWait() {
  cancelled_ = true;
  ownerStop_.request_stop();
  if (pending_) {
    pending_.reset();
    publishResult({.success = false, .message = "Replay export cancelled"});
  }
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

void ReplayExportJob::publishResult(ReplayVideoExportResult result) {
  std::lock_guard lock(resultMutex_);
  result_ = std::move(result);
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
