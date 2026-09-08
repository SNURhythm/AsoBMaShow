#include "MusicSelectFolderStatusLoader.h"

#include <algorithm>
#include <exception>
#include <utility>

MusicSelectFolderStatusLoader::~MusicSelectFolderStatusLoader() {
  cancel();
  worker_.request_stop();
  condition_.notify_all();
}

bool MusicSelectFolderStatusLoader::request(std::vector<MusicSelectBar> bars,
                                           std::string modeFilter,
                                           int longNoteMode, Processor process,
                                           MusicSelectBarId priority) {
  std::vector<MusicSelectBarId> rows;
  rows.reserve(bars.size());
  for (const auto &bar : bars) rows.push_back(bar.id);
  std::unique_lock lock(mutex_);
  const bool sameConfiguration = modeFilter == modeFilter_ &&
                                 longNoteMode == longNoteMode_;
  const bool sameRequest = rows == rows_ && sameConfiguration;
  if (sameRequest) {
    if (!retryAt_ || std::chrono::steady_clock::now() < *retryAt_) {
      auto priorityStop = prioritizeLocked(priority);
      lock.unlock();
      if (priorityStop) priorityStop->request_stop();
      condition_.notify_all();
      return false;
    }
    bars = std::move(failedBars_);
  }
  const auto included = [&](const MusicSelectBarId &id) {
    return std::ranges::find(rows, id) != rows.end();
  };
  const bool preserveActive = sameConfiguration && activeId_ &&
      activeGeneration_ == generation_ && included(*activeId_) &&
      !activeStop_.stop_requested();
  std::erase_if(completed_, [&](const auto &id) {
    return !sameConfiguration || !included({id});
  });
  std::erase_if(results_, [&](const auto &result) {
    return !sameConfiguration || !included(result.id);
  });
  std::erase_if(bars, [&](const auto &bar) {
    return completed_.contains(bar.id.value) ||
           (preserveActive && bar.id == *activeId_);
  });
  priority_ = priority;
  const auto first = std::ranges::find(bars, priority_, &MusicSelectBar::id);
  if (first != bars.end()) std::rotate(bars.begin(), first, std::next(first));
  retryAt_.reset();
  failedBars_.clear();
  auto previousStop = activeStop_;
  if (!preserveActive) ++generation_;
  rows_ = std::move(rows);
  modeFilter_ = std::move(modeFilter);
  longNoteMode_ = longNoteMode;
  std::optional<Request> discarded;
  pending_.swap(discarded);
  pending_ = Request{std::move(bars),
                     std::make_shared<Processor>(std::move(process)), generation_};
  if (!worker_.joinable()) {
    worker_ = std::jthread([this](std::stop_token stop) { run(stop); });
  }
  auto priorityStop = prioritizeLocked(priority);
  lock.unlock();
  if (!preserveActive) previousStop.request_stop();
  if (priorityStop) priorityStop->request_stop();
  condition_.notify_all();
  return true;
}

void MusicSelectFolderStatusLoader::prioritize(const MusicSelectBarId &id) {
  std::unique_lock lock(mutex_);
  auto previousStop = prioritizeLocked(id);
  lock.unlock();
  if (previousStop) previousStop->request_stop();
  condition_.notify_all();
}

std::optional<std::stop_source> MusicSelectFolderStatusLoader::prioritizeLocked(
    const MusicSelectBarId &id) {
  priority_ = id;
  if (!pending_) return std::nullopt;
  const auto found = std::ranges::find(pending_->bars, id, &MusicSelectBar::id);
  if (found == pending_->bars.end()) return std::nullopt;
  std::rotate(pending_->bars.begin(), found, std::next(found));
  if (activeId_ && *activeId_ != id) return activeStop_;
  return std::nullopt;
}

void MusicSelectFolderStatusLoader::cancel() {
  std::optional<Request> discarded;
  std::unique_lock lock(mutex_);
  auto previousStop = activeStop_;
  ++generation_;
  pending_.swap(discarded);
  results_.clear();
  rows_.clear();
  completed_.clear();
  priority_ = {};
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
    MusicSelectBar bar;
    std::shared_ptr<Processor> process;
    std::uint64_t generation;
    std::stop_token operationStop;
    {
      std::unique_lock lock(mutex_);
      if (!condition_.wait(lock, stop, [this] {
            return pending_ && !pending_->bars.empty();
          })) {
        return;
      }
      bar = std::move(pending_->bars.front());
      pending_->bars.erase(pending_->bars.begin());
      process = pending_->process;
      generation = pending_->generation;
      activeId_ = bar.id;
      activeGeneration_ = generation;
      activeStop_ = std::stop_source{};
      operationStop = activeStop_.get_token();
    }
    Result result{.id = bar.id};
    for (int attempt = 0; attempt < 2; ++attempt) {
      if (operationStop.stop_requested()) break;
      try {
        result.frame = (*process)(bar, operationStop);
        result.error.clear();
        break;
      } catch (const std::exception &error) {
        result.error = error.what();
        if (result.error.empty()) result.error = "Folder statistics failed";
      } catch (...) {
        result.error = "Unknown folder statistics failure";
      }
      if (attempt == 0) {
        std::unique_lock lock(mutex_);
        condition_.wait_for(lock, operationStop, std::chrono::milliseconds(100),
                            [&] { return generation != generation_; });
      }
    }
    std::lock_guard lock(mutex_);
    activeId_.reset();
    if (stop.stop_requested() || generation != generation_) continue;
    if (operationStop.stop_requested()) {
      if (bar.id == priority_) pending_->bars.insert(pending_->bars.begin(), std::move(bar));
      else pending_->bars.push_back(std::move(bar));
      continue;
    }
    if (!result.error.empty()) failedBars_.push_back(bar);
    else completed_.insert(bar.id.value);
    results_.push_back(std::move(result));
    if (pending_->bars.empty() && !failedBars_.empty()) {
      retryAt_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    }
  }
}
