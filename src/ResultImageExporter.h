#pragma once

#include "analysis/JudgedPlaybackData.h"
#include "bms_parser.hpp"
#include "context.h"
#include "practice/PracticeResultModel.h"
#include "scene/ResultPresentationModel.h"
#include "scene/play/ReplayResultContext.h"
#include "scene/play/RhythmState.h"
#include "skin/SkinTypes.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

struct ResultImageExportResult {
  bool success = false;
  std::filesystem::path outputPath;
  std::string message;
};

namespace result_image_export {

struct PresentationPlan {
  ResultSkinData skinData{};
  std::optional<result_gauge_history::ResultGaugeGraph> gauge;
  std::string filename;
};

struct PresentationExportDestination {
  std::filesystem::path outputDirectory;
  std::string timestamp;
};

using PresentationRenderBackend = std::function<ResultImageExportResult(
    ResultSkinData, std::optional<result_gauge_history::ResultGaugeGraph>,
    const std::filesystem::path &)>;

namespace detail {

[[nodiscard]] inline bool isReservedFilenameStem(std::string_view value) {
  std::string upper;
  upper.reserve(value.size());
  std::ranges::transform(value, std::back_inserter(upper),
                         [](unsigned char character) {
                           return static_cast<char>(std::toupper(character));
                         });
  constexpr std::array<std::string_view, 4> exact{"CON", "PRN", "AUX", "NUL"};
  if (std::ranges::find(exact, upper) != exact.end()) {
    return true;
  }
  return upper.size() == 4 &&
         (upper.starts_with("COM") || upper.starts_with("LPT")) &&
         upper[3] >= '1' && upper[3] <= '9';
}

[[nodiscard]] inline std::string
sanitizedPresentationTitle(std::string_view title) {
  constexpr std::size_t kMaximumTitleBytes = 80;
  std::string result;
  result.reserve(std::min(title.size(), kMaximumTitleBytes));
  bool pendingSeparator = false;
  for (const unsigned char character : title) {
    if ((character >= 'a' && character <= 'z') ||
        (character >= 'A' && character <= 'Z') ||
        (character >= '0' && character <= '9') || character == '-' ||
        character == '_') {
      if (pendingSeparator && !result.empty() && result.back() != '_') {
        result.push_back('_');
      }
      if (result.size() >= kMaximumTitleBytes) {
        break;
      }
      result.push_back(static_cast<char>(character));
      pendingSeparator = false;
    } else {
      pendingSeparator = true;
    }
  }
  if (result.size() > kMaximumTitleBytes) {
    result.resize(kMaximumTitleBytes);
  }
  while (!result.empty() && result.back() == '_') {
    result.pop_back();
  }
  if (result.empty()) {
    return "result";
  }
  if (isReservedFilenameStem(result)) {
    result.insert(0, "result_");
  }
  return result;
}

} // namespace detail

[[nodiscard]] inline PresentationPlan
presentationPlanFor(const ResultPresentationModel &presentation,
                    std::string_view timestamp) {
  PresentationPlan plan;
  plan.skinData.presentation = &presentation;
  plan.skinData.showControls = false;
  plan.skinData.showTimingAnalytics = false;
  plan.gauge = result_gauge_history::graphFor(presentation.gaugeSeries, 0);
  plan.skinData.showResultGraph = !presentation.gaugeSeries.empty();
  plan.filename = detail::sanitizedPresentationTitle(presentation.title) + "_" +
                  std::string(timestamp) + ".png";
  return plan;
}

} // namespace result_image_export

class ResultImageExporter {
public:
  // Shared presentation export orchestration. Production supplies the bgfx
  // renderer; controlled/headless callers can supply another artifact writer
  // while exercising the same destination, filename, skin, and gauge plan.
  static ResultImageExportResult
  Export(const ResultPresentationModel &presentation,
         const result_image_export::PresentationExportDestination &destination,
         const result_image_export::PresentationRenderBackend &renderBackend);
  static ResultImageExportResult
  Export(ApplicationContext &context,
         const ResultPresentationModel &presentation);
  static ResultImageExportResult Export(
      ApplicationContext &context, const bms_parser::ChartMeta &meta,
      const RhythmState &state, const std::string &playModeLabel = {},
      const std::string &laneOrderLabel = {},
      const std::string &difficultyLabel = {},
      const std::optional<ResultPreviousBestData> &previousBest = std::nullopt,
      const std::optional<std::string> &currentClearLabelOverride =
          std::nullopt,
      const std::optional<int> &currentClearRankOverride = std::nullopt,
      const std::optional<std::string> &headerDifficultyLabelOverride =
          std::nullopt,
      const std::optional<ResultPacemakerData> &pacemaker = std::nullopt,
      const std::optional<practice::ResultModel> &analyticsModel =
          std::nullopt);
  static ResultImageExportResult
  ExportReplay(ApplicationContext &context, bms_parser::Chart &chart,
               const JudgedPlaybackData &replay,
               const std::string &pacemakerTarget = {},
               std::optional<ReplayResultContext> resultContext = std::nullopt);
  static ResultImageExportResult ExportCourseReplay(
      ApplicationContext &context, const JudgedCoursePlaybackData &replay,
      std::optional<ReplayResultContext> resultContext = std::nullopt);
};
