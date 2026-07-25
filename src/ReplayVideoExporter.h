#pragma once

#include "analysis/JudgedPlaybackData.h"
#include "ResultPersistenceModel.h"
#include "replay/ReplayPlaybackData.h"
#include "context.h"

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
};

struct ReplayVideoExportResult {
  bool success = false;
  std::filesystem::path outputPath;
  std::string message;
};

class ReplayVideoExporter {
public:
  static ReplayVideoExportResult
  Export(ApplicationContext &context, bms_parser::Chart *chart,
         const JudgedPlaybackData &replay,
         const ReplayVideoExportOptions &options = {});
  static ReplayVideoExportResult Export(
      ApplicationContext &context, bms_parser::Chart *chart,
      const replay::ReplayPlaybackData &playback,
      const result_persistence::PersistedChartResult &result,
      const ReplayVideoExportOptions &options = {});
  static ReplayVideoExportResult
  ExportCourseReplay(ApplicationContext &context, const JudgedCoursePlaybackData &replay,
                     const ReplayVideoExportOptions &options = {});
  static ReplayVideoExportResult ExportCourseReplay(
      ApplicationContext &context,
      const replay::CourseReplayPlaybackData &playback,
      const result_persistence::PersistedCourseResult &result,
      const ReplayVideoExportOptions &options = {});
};
