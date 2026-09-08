#pragma once

#include <chrono>
#include <mutex>
#include <set>
#include <string>

// Records stage timestamps for the chart-start → first-frame path so a hang
// can be attributed to a specific phase. Writes a line per session to
// <platform documents>/startup-timings.log. Thread-safe; safe to call from
// the selector/main-menu UI thread and the background launch thread.
class StartupTiming {
public:
  static StartupTiming &instance();

  // Begins a new start-to-first-frame session and returns its session id.
  // Each call after BeginSession appends to the same session until
  // FinishFirstFrame. A stale session older than kSessionMaxMicros is
  // superseded by the next BeginSession.
  void beginSession();

  // Appends a stage marker with the micros elapsed since beginSession.
  void mark(const char *stage);

  // Appends a free-form debug line to the current session log (no elapsed
  // prefix). Used for diagnostic evidence during startup-path debugging.
  void note(const std::string &line);

  // Appends line once per session for a given key. Used to report renderer
  // conditions that fire every frame without flooding the log.
  void noteOnce(const char *key, const std::string &line);

  void finishFirstFrame();

private:
  StartupTiming() = default;
  void flushLocked(std::chrono::steady_clock::time_point end);

  std::mutex mutex_;
  std::chrono::steady_clock::time_point start_{};
  bool active_ = false;
  std::size_t session_ = 0;
  std::set<std::string> notedKeys_;
};