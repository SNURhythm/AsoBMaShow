#include "SettingsLibraryTask.h"

#include <utility>

SettingsLibraryTask::~SettingsLibraryTask() { stopAndWait(); }

bool SettingsLibraryTask::start(Work work) {
  {
    std::lock_guard lock(mutex_);
    if (running_) {
      return false;
    }
    running_ = true;
  }
  if (worker_.joinable()) {
    worker_.join();
  }
  try {
    worker_ = std::jthread([this, work = std::move(work)](const std::stop_token &token) {
      const Publisher updates(*this, token);
      work(token, updates);
      std::lock_guard lock(mutex_);
      running_ = false;
    });
  } catch (...) {
    std::lock_guard lock(mutex_);
    running_ = false;
    throw;
  }
  return true;
}

bool SettingsLibraryTask::running() const {
  std::lock_guard lock(mutex_);
  return running_;
}

SettingsLibraryTask::Updates SettingsLibraryTask::takeUpdates() {
  std::lock_guard lock(mutex_);
  return std::exchange(pending_, {});
}

void SettingsLibraryTask::stopAndWait() {
  if (worker_.joinable()) {
    worker_.request_stop();
    worker_.join();
  }
  std::lock_guard lock(mutex_);
  pending_ = {};
}

void SettingsLibraryTask::Publisher::tableStatus(
    std::string text, bool succeeded, bool reload) const {
  std::lock_guard lock(owner_.mutex_);
  if (!token_.stop_requested()) {
    owner_.pending_.tableStatus = Status{std::move(text), succeeded};
    owner_.pending_.reload = owner_.pending_.reload || reload;
  }
}

void SettingsLibraryTask::Publisher::folderStatus(
    std::string text, bool succeeded, bool reload) const {
  std::lock_guard lock(owner_.mutex_);
  if (!token_.stop_requested()) {
    owner_.pending_.folderStatus = Status{std::move(text), succeeded};
    owner_.pending_.reload = owner_.pending_.reload || reload;
  }
}

void SettingsLibraryTask::Publisher::importProgress(ImportProgress progress) const {
  std::lock_guard lock(owner_.mutex_);
  if (!token_.stop_requested()) {
    progress.succeeded = progress.finished && progress.succeeded;
    if (!progress.finished) {
      progress.submittedUrl.clear();
    }
    owner_.pending_.importProgress = std::move(progress);
  }
}
