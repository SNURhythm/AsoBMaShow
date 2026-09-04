#include "StartupTiming.h"

#include "Utils.h"
#include "targets.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <sstream>

namespace {
constexpr auto kSessionMaxMicros = std::chrono::microseconds(60'000'000);
}

StartupTiming &StartupTiming::instance() {
  static StartupTiming timing;
  return timing;
}

void StartupTiming::beginSession() {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto now = std::chrono::steady_clock::now();
  if (active_ && now - start_ > kSessionMaxMicros) {
    // A previous session never reached the first frame; flush what we have.
    flushLocked(now);
  }
  ++session_;
  start_ = now;
  active_ = true;
  std::ostringstream line;
  line << "=== session " << session_ << " start button press ===";
  auto path = Utils::GetDocumentsPath() / "startup-timings.log";
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (std::FILE *file = std::fopen(path.string().c_str(), "a")) {
    std::fprintf(file, "%s\n", line.str().c_str());
    std::fclose(file);
  }
}

void StartupTiming::mark(const char *stage) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!active_) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      now - start_);
  auto path = Utils::GetDocumentsPath() / "startup-timings.log";
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (std::FILE *file = std::fopen(path.string().c_str(), "a")) {
    std::fprintf(file, "  %8lld us  %s\n",
                 static_cast<long long>(elapsed.count()), stage);
    std::fclose(file);
  }
}

void StartupTiming::note(const std::string &line) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!active_) {
    return;
  }
  auto path = Utils::GetDocumentsPath() / "startup-timings.log";
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (std::FILE *file = std::fopen(path.string().c_str(), "a")) {
    std::fprintf(file, "  note: %s\n", line.c_str());
    std::fclose(file);
  }
}

void StartupTiming::finishFirstFrame() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!active_) {
    return;
  }
  flushLocked(std::chrono::steady_clock::now());
}

void StartupTiming::flushLocked(std::chrono::steady_clock::time_point end) {
  if (!active_) {
    return;
  }
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::microseconds>(end - start_);
  auto path = Utils::GetDocumentsPath() / "startup-timings.log";
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (std::FILE *file = std::fopen(path.string().c_str(), "a")) {
    std::fprintf(file, "  %8lld us  first GamePlayScene frame rendered\n",
                 static_cast<long long>(elapsed.count()));
    std::fclose(file);
  }
  active_ = false;
}