#pragma once

#include "../ArchiveFile.h"
#include <SDL3/SDL.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string_view>

namespace perf {
// Coarse control-thread timings, also available in the in-app Performance Log.
// Never use this on an audio callback or another realtime path.
class StartupTrace {
public:
  explicit StartupTrace(std::string_view scope) : scope_(scope) { mark("begin"); }
  ~StartupTrace() { mark("end"); }
  StartupTrace(const StartupTrace &) = delete;
  StartupTrace &operator=(const StartupTrace &) = delete;

  void mark(std::string_view stage) noexcept {
    const auto now = Clock::now();
    const double elapsed = std::chrono::duration<double, std::milli>(now - last_).count();
    const double total = std::chrono::duration<double, std::milli>(now - start_).count();
    last_ = now;
    try {
      char message[256];
      std::snprintf(message, sizeof(message),
                    "[play-start] %.*s#%llu %.*s: %.2f ms (total %.2f ms)",
                    static_cast<int>(scope_.size()), scope_.data(),
                    static_cast<unsigned long long>(id_),
                    static_cast<int>(stage.size()), stage.data(), elapsed, total);
      SDL_Log("%s", message);
      archive_file::appendDebugLogLine(message);
    } catch (...) {
      // Diagnostic allocation failure must not affect the launch itself.
    }
  }

private:
  using Clock = std::chrono::steady_clock;
  inline static std::atomic<std::uint64_t> nextId_{0};
  std::string_view scope_;
  std::uint64_t id_ = ++nextId_;
  Clock::time_point start_ = Clock::now();
  Clock::time_point last_ = start_;
};
} // namespace perf
