#include "ChartPreloadWorker.h"

ChartPreloadWorker::~ChartPreloadWorker() { stop(); }

void ChartPreloadWorker::configure(Processor processor) {
  processor_ = std::move(processor);
}

void ChartPreloadWorker::request(const ChartMetaRecord &record) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_ &&
        fspath_to_path_t(pending_->meta.BmsPath) ==
            fspath_to_path_t(record.meta.BmsPath)) {
      return;  // this exact chart is already queued
    }
    if (inFlightPath_ && inFlightCancellation_ &&
        !inFlightCancellation_->load(std::memory_order_acquire) &&
        *inFlightPath_ == fspath_to_path_t(record.meta.BmsPath)) {
      return;  // this exact chart is already being processed
    }
    // A request that supersedes a different in-flight chart must abort that
    // load promptly (it may be a long jukebox chart load that only checks its
    // cancellation flag between phases), so the new selection is served
    // without blocking on the old one.
    if (inFlightCancellation_) {
      inFlightCancellation_->store(true, std::memory_order_release);
    }
    // A request may arrive after cancel() left stop_ set. Re-enable processing
    // on the idle worker; ensureWorker() starts a thread after stop() joined it.
    stop_.store(false, std::memory_order_release);
    pending_ = record;
    pendingSince_ = std::chrono::steady_clock::now();
  }
  ensureWorker();
  cv_.notify_one();
}

void ChartPreloadWorker::cancel() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_.store(true, std::memory_order_release);
    if (inFlightCancellation_) {
      inFlightCancellation_->store(true, std::memory_order_release);
    }
    pending_.reset();
    inFlightPath_.reset();
    idleNotificationPending_ = true;
  }
  cv_.notify_all();
}

void ChartPreloadWorker::stop() {
  cancel();
  joinIfRunning();
}

bool ChartPreloadWorker::superseded(std::string_view path) const {
  std::unique_lock<std::mutex> lock(mutex_);
  if (stop_.load(std::memory_order_acquire)) {
    return true;
  }
  // A newer request for a different chart supersedes the current one; a
  // re-request for the same chart is not a supersession (request() dedups it).
  return pending_.has_value() &&
         fspath_to_path_t(pending_->meta.BmsPath) != fspath_to_path_t(path);
}

bool ChartPreloadWorker::isRequesting(std::string_view path) const {
  std::unique_lock<std::mutex> lock(mutex_);
  if (pending_ &&
      fspath_to_path_t(pending_->meta.BmsPath) == fspath_to_path_t(path)) {
    return true;
  }
  return inFlightPath_.has_value() && *inFlightPath_ == fspath_to_path_t(path);
}

void ChartPreloadWorker::setOnIdle(std::function<void()> onIdle) {
  onIdle_ = std::move(onIdle);
}

void ChartPreloadWorker::ensureWorker() {
  if (thread_.joinable()) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_.store(false, std::memory_order_release);
  }
  thread_ = std::jthread(
      [this](std::stop_token stop) { workerLoop(std::move(stop)); });
}

void ChartPreloadWorker::workerLoop(std::stop_token stop) {
  while (true) {
    ChartMetaRecord request;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      while (true) {
        if (idleNotificationPending_) {
          idleNotificationPending_ = false;
          lock.unlock();
          if (onIdle_) onIdle_();
          lock.lock();
          continue;
        }
        if (stop.stop_requested()) {
          return;
        }
        if (stop_.load(std::memory_order_acquire) || !pending_.has_value()) {
          cv_.wait(lock, [&] {
            return stop.stop_requested() ||
                   idleNotificationPending_ ||
                   (!stop_.load(std::memory_order_acquire) && pending_.has_value());
          });
          continue;
        }
        const auto elapsed = std::chrono::steady_clock::now() - pendingSince_;
        if (elapsed >= debounceDelay_) {
          break;
        }
        cv_.wait_for(lock, debounceDelay_ - elapsed, [&] {
          return stop.stop_requested() ||
                 idleNotificationPending_ ||
                 stop_.load(std::memory_order_acquire);
        });
      }
      request = std::move(*pending_);
      pending_.reset();
      inFlightPath_ = fspath_to_path_t(request.meta.BmsPath);
      inFlightCancellation_ = std::make_shared<std::atomic_bool>(false);
    }
    std::shared_ptr<std::atomic_bool> cancellation = inFlightCancellation_;
    if (processor_) {
      processor_(request, *cancellation);
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      inFlightPath_.reset();
      if (inFlightCancellation_ == cancellation) {
        inFlightCancellation_.reset();
      }
    }
    if (onIdle_) {
      onIdle_();
    }
  }
}

void ChartPreloadWorker::joinIfRunning() {
  if (thread_.joinable()) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      thread_.request_stop();
    }
    cv_.notify_all();
    thread_.join();
  }
}
