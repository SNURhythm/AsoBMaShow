#include "MusicSelectFolderStatusLoader.h"

#include <exception>
#include <utility>

MusicSelectFolderStatusLoader::~MusicSelectFolderStatusLoader() {
  cancel();
  worker_.request_stop();
  condition_.notify_all();
}

bool MusicSelectFolderStatusLoader::request(std::vector<MusicSelectBar> bars,
                                           std::string modeFilter,
                                           int longNoteMode, Processor process) {
  std::vector<MusicSelectBarId> rows;
  rows.reserve(bars.size());
  for (const auto &bar : bars) rows.push_back(bar.id);
  std::unique_lock lock(mutex_);
  const bool sameRequest = rows == rows_ && modeFilter == modeFilter_ &&
                           longNoteMode == longNoteMode_;
  if (sameRequest) {
    if (!retryAt_ || std::chrono::steady_clock::now() < *retryAt_) return false;
    bars = std::move(failedBars_);
  } else {
    results_.clear();
  }
  retryAt_.reset();
  failedBars_.clear();
  auto previousStop = activeStop_;
  activeStop_ = std::stop_source{};
  rows_ = std::move(rows);
  modeFilter_ = std::move(modeFilter);
  longNoteMode_ = longNoteMode;
  pending_ = Request{std::move(bars), std::move(process), ++generation_,
                     activeStop_.get_token()};
  if (!worker_.joinable()) {
    worker_ = std::jthread([this](std::stop_token stop) { run(stop); });
  }
  lock.unlock();
  previousStop.request_stop();
  condition_.notify_all();
  return true;
}

void MusicSelectFolderStatusLoader::cancel() {
  std::unique_lock lock(mutex_);
  auto previousStop = activeStop_;
  ++generation_;
  pending_.reset();
  results_.clear();
  rows_.clear();
  modeFilter_.clear();
  longNoteMode_ = -1;
  failedBars_.clear();
  retryAt_.reset();
  lock.unlock();
  previousStop.request_stop();
  condition_.notify_all();
}

std::vector<MusicSelectFolderStatusLoader::Result>
MusicSelectFolderStatusLoader::takeResults() {
  std::lock_guard lock(mutex_);
  return std::exchange(results_, {});
}

bool MusicSelectFolderStatusLoader::retryReady() {
  std::lock_guard lock(mutex_);
  return retryAt_ && std::chrono::steady_clock::now() >= *retryAt_;
}

void MusicSelectFolderStatusLoader::run(std::stop_token stop) {
  while (!stop.stop_requested()) {
    Request request;
    {
      std::unique_lock lock(mutex_);
      if (!condition_.wait(lock, stop, [this] { return pending_.has_value(); })) {
        return;
      }
      request = std::move(*pending_);
      pending_.reset();
    }
    for (const auto &bar : request.bars) {
      {
        std::lock_guard lock(mutex_);
        if (stop.stop_requested() || request.generation != generation_) break;
      }
      Result result{.id = bar.id};
      for (int attempt = 0; attempt < 2; ++attempt) {
        if (request.stop.stop_requested()) break;
        try {
          result.frame = request.process(bar, request.stop);
          result.error.clear();
          break;
        } catch (const std::exception &error) {
          result.error = error.what();
        }
        if (attempt == 0) {
          std::unique_lock lock(mutex_);
          condition_.wait_for(lock, request.stop, std::chrono::milliseconds(100),
                              [&] { return request.generation != generation_; });
        }
      }
      std::lock_guard lock(mutex_);
      if (stop.stop_requested() || request.generation != generation_) break;
      if (!result.error.empty()) failedBars_.push_back(bar);
      results_.push_back(std::move(result));
    }
    std::lock_guard lock(mutex_);
    if (request.generation == generation_ && !failedBars_.empty()) {
      retryAt_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    }
  }
}
