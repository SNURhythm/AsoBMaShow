#pragma once

#include "ModernResult.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace result_recall {

using ModernChartLoader = std::function<std::unique_ptr<bms_parser::Chart>(
    const std::filesystem::path &, std::atomic_bool &)>;

struct ModernChartResultView {
  std::unique_ptr<bms_parser::Chart> chart;
  result_persistence::ModernChartResult result;
  RhythmState state;
};

struct ModernChartBuildOutcome {
  std::optional<ModernChartResultView> value;
  std::string diagnostic;
};

struct ModernCourseStageView {
  std::shared_ptr<bms_parser::Chart> chart;
  result_persistence::ModernCourseStageResult result;
  RhythmState state;
};

struct ModernCourseResultView {
  result_persistence::ModernCourseResult result;
  std::vector<ModernCourseStageView> completedStages;
};

struct ModernCourseBuildOutcome {
  std::optional<ModernCourseResultView> value;
  std::string diagnostic;
};

[[nodiscard]] ModernChartBuildOutcome
BuildChartResult(result_persistence::ModernChartResult result,
                 std::atomic_bool &cancelled, ModernChartLoader loader = {});

[[nodiscard]] ModernCourseBuildOutcome
BuildCourseResult(result_persistence::ModernCourseResult result,
                  std::atomic_bool &cancelled, ModernChartLoader loader = {});

} // namespace result_recall
