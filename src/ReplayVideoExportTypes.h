#pragma once

#include "ThreadCompat.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>

struct ReplayVideoExportProgress {
  double fraction = 0.0;
  std::string message;
  std::size_t frameIndex = 0;
  std::size_t frameCount = 0;
};

using ReplayVideoExportProgressCallback =
    std::function<void(const ReplayVideoExportProgress &)>;

struct ReplayVideoExportOptions {
  int width = 0;
  int height = 0;
  int fps = 0;
  bool includeResultScreen = false;
  bool renderTouchPoints = true;
  bool renderReplayGhosts = true;
  std::string pacemakerTarget;
  ReplayVideoExportProgressCallback progressCallback;
  std::stop_token stop;
};

struct ReplayVideoExportResult {
  bool success = false;
  std::filesystem::path outputPath;
  std::string message;
};
