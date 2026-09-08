#include "MusicSelectDirectoryLoader.h"

#include <exception>
#include <utility>

MusicSelectDirectoryLoader::~MusicSelectDirectoryLoader() {
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
  }
  cancel();
  worker_.request_stop();
  condition_.notify_all();
  if (worker_.joinable()) worker_.join();
}

std::uint64_t MusicSelectDirectoryLoader::request(MusicSelectBarId id,
                                                 Processor process) {
  std::optional<Request> discarded;
  std::vector<Result> discardedResults;
  std::unique_lock lock(mutex_);
  if (stopping_) return 0;
  auto previousStop = activeStop_;
  activeStop_ = std::stop_source{};
  const auto generation = ++generation_;
  pending_.swap(discarded);
  results_.swap(discardedResults);
  pending_ = Request{std::move(id), std::move(process), generation,
                     activeStop_.get_token()};
  if (!worker_.joinable()) {
    worker_ = std::jthread([this](std::stop_token stop) { run(stop); });
  }
  lock.unlock();
  previousStop.request_stop();
  condition_.notify_all();
  return generation;
}

void MusicSelectDirectoryLoader::cancel() {
  std::optional<Request> discarded;
  std::vector<Result> discardedResults;
  std::unique_lock lock(mutex_);
  auto previousStop = activeStop_;
  ++generation_;
  pending_.swap(discarded);
  results_.swap(discardedResults);
  lock.unlock();
  previousStop.request_stop();
  condition_.notify_all();
}

std::vector<MusicSelectDirectoryLoader::Result>
MusicSelectDirectoryLoader::takeResults() {
  std::lock_guard lock(mutex_);
  return std::exchange(results_, {});
}

void MusicSelectDirectoryLoader::run(std::stop_token stop) {
  while (!stop.stop_requested()) {
    std::optional<Request> request;
    {
      std::unique_lock lock(mutex_);
      if (!condition_.wait(lock, stop, [this] { return pending_.has_value(); })) {
        return;
      }
      if (stopping_ || stop.stop_requested()) return;
      request.swap(pending_);
      if (request->generation != generation_ || request->stop.stop_requested()) {
        continue;
      }
    }
    Result result{.id = request->id, .generation = request->generation};
    try {
      result.content = request->process(request->stop);
    } catch (const std::exception &error) {
      result.error = error.what();
      if (result.error.empty()) result.error = "Directory loading failed";
    } catch (...) {
      result.error = "Unknown directory loading error";
    }
    std::lock_guard lock(mutex_);
    if (!stopping_ && !stop.stop_requested() &&
        !request->stop.stop_requested() && request->generation == generation_) {
      results_.push_back(std::move(result));
    }
  }
}
