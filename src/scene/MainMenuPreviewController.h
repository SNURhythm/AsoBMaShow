#pragma once

#include "ChartPreloadWorker.h"

// Owns Main Menu's preview scheduling and deferred media-release protocol.
// The selected chart stays in the scene because Start can reuse it after stop().
// Configure once at construction. Call request/cancel from the selection thread;
// a handoff worker may call stop() after the scene has excluded new requests.
class MainMenuPreviewController final {
public:
  MainMenuPreviewController(
      ChartPreloadWorker::Processor processor, std::function<void()> release,
      std::chrono::milliseconds debounceDelay = std::chrono::milliseconds(100));
  ~MainMenuPreviewController();
  MainMenuPreviewController(const MainMenuPreviewController &) = delete;
  MainMenuPreviewController &operator=(const MainMenuPreviewController &) = delete;

  // A new preview withdraws any deferred release of the previous selection.
  void request(const ChartMetaRecord &record);
  void cancel();

  // Nonblocking. Release runs on the worker after any load finishes. As with
  // ChartPreloadWorker::cancel(), this does not start an unstarted/stopped worker.
  void cancelAndReleaseWhenIdle();

  // Join loading and any deferred release, then discard the release request.
  // Does not unconditionally release media: Start may reuse the loaded chart.
  void stop();
  [[nodiscard]] bool superseded(std::string_view path) const;

private:
  void releaseWhenIdle();

  std::function<void()> release_;
  std::mutex releaseMutex_;
  bool releasePending_ = false;
  // Joined before the callback and its synchronization state are destroyed.
  ChartPreloadWorker worker_;
};
