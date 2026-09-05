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
}

void SelectAudioLog(std::string_view message) {
  std::lock_guard<std::mutex> lock(gLogMutex);
  std::error_code error;
  const auto now = std::chrono::system_clock::now();
  const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                          now.time_since_epoch())
                          .count();
  std::ofstream log(
      Utils::GetDocumentsPath("select-audio.log"),
      std::ios::app);
  if (!log) {
    return;
  }
  log << millis << " " << message << "\n";
}

} // namespace audio::diag