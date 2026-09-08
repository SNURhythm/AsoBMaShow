#include "IrExternalUrlService.h"

namespace ir {

IrExternalUrlService::IrExternalUrlService(Resolver resolver)
    : resolver_(std::move(resolver)),
      worker_([this](std::stop_token stopToken) { workerMain(stopToken); }) {}

IrExternalUrlService::~IrExternalUrlService() { stop(); }

std::uint64_t IrExternalUrlService::open(IrExternalUrlRequest request) {
  std::lock_guard lock(mutex_);
  if (stopping_) {
    return 0;
  }
  activeStop_.request_stop();
  activeStop_ = std::stop_source{};
  const auto generation = ++generation_;
  pending_ = Work{.request = std::move(request),
                  .generation = generation,
                  .stopSource = activeStop_};
  snapshot_ = {.generation = generation};
  workAvailable_.notify_one();
  return generation;
}

void IrExternalUrlService::close(std::uint64_t generation) {
  std::lock_guard lock(mutex_);
  if (snapshot_.generation != generation) {
    return;
  }
  activeStop_.request_stop();
  if (pending_ && pending_->generation == generation) {
    pending_.reset();
  }
  snapshot_ = {.generation = generation};
}

IrExternalUrlSnapshot IrExternalUrlService::snapshot() const {
  std::lock_guard lock(mutex_);
  return snapshot_;
}

void IrExternalUrlService::stop() noexcept {
  {
    std::lock_guard lock(mutex_);
    if (stopping_) {
      return;
    }
    stopping_ = true;
    activeStop_.request_stop();
    pending_.reset();
  }
  workAvailable_.notify_all();
  worker_.request_stop();
  if (worker_.joinable()) {
    worker_.join();
  }
}

void IrExternalUrlService::workerMain(
    std::stop_token workerStopToken) noexcept {
  while (!workerStopToken.stop_requested()) {
    Work work;
    {
      std::unique_lock lock(mutex_);
      workAvailable_.wait(
          lock, [this] { return stopping_ || pending_.has_value(); });
      if (stopping_) {
        return;
      }
      work = std::move(*pending_);
      pending_.reset();
    }

    std::optional<std::string> url;
    try {
      if (!work.stopSource.stop_requested()) {
        url = resolver_(work.request, work.stopSource.get_token());
      }
    } catch (...) {
      url.reset();
    }
    if (workerStopToken.stop_requested() ||
        work.stopSource.stop_requested()) {
      continue;
    }

    std::lock_guard lock(mutex_);
    if (!stopping_ && work.generation == generation_) {
      snapshot_ = {.generation = work.generation,
                   .finished = true,
                   .url = std::move(url)};
    }
  }
}

} // namespace ir
