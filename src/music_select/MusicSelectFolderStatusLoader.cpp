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
  if (rows == rows_ && modeFilter == modeFilter_ &&
      longNoteMode == longNoteMode_) return false;
  auto previousStop = activeStop_;
  activeStop_ = std::stop_source{};
  rows_ = std::move(rows);
  modeFilter_ = std::move(modeFilter);
  longNoteMode_ = longNoteMode;
  pending_ = Request{std::move(bars), std::move(process), ++generation_,
                     activeStop_.get_token()};
  results_.clear();
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
  lock.unlock();
  previousStop.request_stop();
  condition_.notify_all();
}

std::vector<MusicSelectFolderStatusLoader::Result>
MusicSelectFolderStatusLoader::takeResults() {
  std::lock_guard lock(mutex_);
  return std::exchange(results_, {});
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
      try {
        result.frame = request.process(bar, request.stop);
      } catch (const std::exception &error) {
        result.error = error.what();
      }
      std::lock_guard lock(mutex_);
      if (stop.stop_requested() || request.generation != generation_) break;
      results_.push_back(std::move(result));
    }
  }
}
