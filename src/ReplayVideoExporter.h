#pragma once

#include "ReplayData.h"
#include "context.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>

namespace replay {
struct CourseReplayConsumerOutcome;
}

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

// Keeps the normal export's selected-skin preflight ahead of every output
// action. It is deliberately a tiny seam: the caller still owns all real
// audio, rendering, file, and platform work in `continueExport`.
namespace replay_video_export {
template <typename Preflight, typename Continue>
ReplayVideoExportResult
runPreflightGatedNormalExport(Preflight &&preflight, Continue &&continueExport) {
  if (const auto failure = std::forward<Preflight>(preflight)()) {
    return *failure;
  }
  return std::forward<Continue>(continueExport)();
}
} // namespace replay_video_export

class ReplayVideoExporter {
public:
  static ReplayVideoExportResult
  Export(ApplicationContext &context, bms_parser::Chart *chart,
         const ReplayData &replay,
         const ReplayVideoExportOptions &options = {});
  static ReplayVideoExportResult
  ExportCourseReplay(ApplicationContext &context, const CourseReplayData &replay,
                     const ReplayVideoExportOptions &options = {});
  static ReplayVideoExportResult ExportCourseReplay(
      ApplicationContext &context,
      replay::CourseReplayConsumerOutcome &&verified,
      const ReplayVideoExportOptions &options = {});
};
