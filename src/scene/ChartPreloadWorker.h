#pragma once

#include "../repositories/ChartRepository.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string_view>
#include <thread>
#include <utility>

// Single-worker, latest-wins background loader for a chart's parse + jukebox
// work. MusicSelect uses it to preload the selected chart so Start is
// near-instant; MainMenu uses it for the debounced preview load. A new
// request supersedes any in-flight or queued work without blocking the caller
// (the worker abandons superseded work at its next checkpoint), so scrolling
// or changing selection never stalls on a heavy load.
//
// Requests are debounced: the worker waits for the selection to settle before
// starting, so rapid scrolling coalesces into a single load instead of
// starting-and-abandoning work for every intermediate bar. The scene supplies
// a Processor that performs parse + jukebox + publish and calls superseded()
// between phases. request() is latest-wins and dedups a re-request for the
// same path. cancel() stops cooperatively without joining; stop() joins (for
// teardown and before launching gameplay). onIdle fires on the worker thread
// after each processed request finishes, whether published or abandoned.
class ChartPreloadWorker {
public:
  using Processor = std::function<void(const ChartMetaRecord &,
                                       std::atomic_bool &cancelled)>;

  explicit ChartPreloadWorker(
      std::chrono::milliseconds debounceDelay = std::chrono::milliseconds(100))
      : debounceDelay_(debounceDelay) {}
  ChartPreloadWorker(const ChartPreloadWorker &) = delete;
  ChartPreloadWorker &operator=(const ChartPreloadWorker &) = delete;
  ~ChartPreloadWorker();

  void configure(Processor processor);

  // Requests a load of the given chart (latest-wins, debounced, non-blocking).
  // A request for a path already queued or in flight is a no-op.
  void request(const ChartMetaRecord &record);

  // Cancels any debounced pending request and cooperatively stops the worker
  // without joining. The worker abandons the current request at its next
  // checkpoint and returns to idle.
  void cancel();

  // Cancels and joins the worker thread (blocking). Safe to call multiple
  // times; a later request() respawns the worker.
  void stop();

  // Returns true when the worker should abandon work for the given path:
  // a stop was requested or a different request has been queued since.
  [[nodiscard]] bool superseded(std::string_view path) const;

  void setOnIdle(std::function<void()> onIdle);

private:
  void ensureWorker();
  void workerLoop(std::stop_token stop);
  void joinIfRunning();

  Processor processor_;
  std::function<void()> onIdle_;
  std::chrono::milliseconds debounceDelay_;
  std::jthread thread_;
  std::atomic_bool stop_{false};
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::optional<ChartMetaRecord> pending_;
  std::optional<path_t> inFlightPath_;
  std::chrono::steady_clock::time_point pendingSince_{};
};