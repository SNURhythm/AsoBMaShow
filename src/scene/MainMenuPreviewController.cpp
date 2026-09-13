#include "MainMenuPreviewController.h"

MainMenuPreviewController::MainMenuPreviewController(
    ChartPreloadWorker::Processor processor, std::function<void()> release,
    std::chrono::milliseconds debounceDelay)
    : release_(std::move(release)), worker_(debounceDelay) {
  worker_.configure(std::move(processor));
  worker_.setOnIdle([this] { releaseWhenIdle(); });
}

MainMenuPreviewController::~MainMenuPreviewController() { stop(); }

void MainMenuPreviewController::request(const ChartMetaRecord &record) {
  {
    std::lock_guard lock(releaseMutex_);
    releasePending_ = false;
  }
  worker_.request(record);
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
  worker_.stop();
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
