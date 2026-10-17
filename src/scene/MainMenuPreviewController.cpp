#include "MainMenuPreviewController.h"

MainMenuPreviewController::MainMenuPreviewController(
    ChartPreloadWorker::Processor processor, std::function<void()> release,
    std::chrono::milliseconds debounceDelay, std::function<void()> stopPlayback,
    std::function<bool()> admit)
    : release_(std::move(release)), stopPlayback_(std::move(stopPlayback)), admit_(std::move(admit)),
      worker_(debounceDelay) {
  worker_.configure(std::move(processor));
  worker_.setOnIdle([this] { releaseWhenIdle(); });
}

MainMenuPreviewController::~MainMenuPreviewController() { stop(); }

void MainMenuPreviewController::request(const ChartMetaRecord &record,
                                        bool bypassDebounce) {
  std::lock_guard stopLock(stopMutex_);
  if (admit_ && !admit_()) return;
  {
    std::lock_guard lock(releaseMutex_);
    releasePending_ = false;
  }
  worker_.request(record, bypassDebounce);
}

void MainMenuPreviewController::cancel() { worker_.cancel(); }

void MainMenuPreviewController::cancelAndReleaseWhenIdle() {
  {
    std::lock_guard lock(releaseMutex_);
    releasePending_ = true;
  }
  worker_.cancel();
}

void MainMenuPreviewController::stop() {
  std::lock_guard stopLock(stopMutex_);
  worker_.stop();
  if (stopPlayback_) stopPlayback_();
  std::lock_guard lock(releaseMutex_);
  releasePending_ = false;
}

bool MainMenuPreviewController::superseded(std::string_view path) const {
  return worker_.superseded(path);
}

void MainMenuPreviewController::releaseWhenIdle() {
  bool release = false;
  {
    std::lock_guard lock(releaseMutex_);
    release = std::exchange(releasePending_, false);
  }
  if (release && release_) release_();
}
