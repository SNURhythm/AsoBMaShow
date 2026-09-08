#include "SelectAudioDiagnostics.h"

#include "../Utils.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

namespace audio::diag {

namespace {
std::mutex gLogMutex;

// Cached on the first call so the Documents path is resolved once (likely from
// the UI/main thread, where iOS UIKit file-path APIs are valid). Worker threads
// must not call GetDocumentsPath (it can fail or require the main thread on
// iOS); they reuse the cached path so their diagnostic lines are written too.
std::filesystem::path gLogPath;

std::filesystem::path resolveLogPath() {
  if (!gLogPath.empty()) {
    return gLogPath;
  }
  std::filesystem::path path = Utils::GetDocumentsPath("select-audio.log");
  if (path.empty()) {
    path = std::filesystem::path("select-audio.log");
  }
  gLogPath = path;
  return gLogPath;
}
}

void SelectAudioLog(std::string_view message) {
  std::lock_guard<std::mutex> lock(gLogMutex);
  const auto now = std::chrono::system_clock::now();
  const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                          now.time_since_epoch())
                          .count();
  std::ofstream log(resolveLogPath(), std::ios::app);
  if (!log) {
    return;
  }
  log << millis << " " << message << "\n";
}

} // namespace audio::diag