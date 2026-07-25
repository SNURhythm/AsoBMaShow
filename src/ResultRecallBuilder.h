#pragma once

#include "CoursePlaySession.h"
#include "ResultPersistenceModel.h"

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace result_recall {

using ResultChartLoader = std::function<std::unique_ptr<bms_parser::Chart>(
    const result_persistence::PersistedChartResult &, std::atomic_bool &)>;

struct ChartResult {
  std::unique_ptr<bms_parser::Chart> chart;
  result_persistence::PersistedChartResult result;
  RhythmState state;
};

struct ChartBuildOutcome {
  std::optional<ChartResult> value;
  std::string diagnostic;
};

struct CourseResult {
  std::shared_ptr<CoursePlaySession> session;
  result_persistence::PersistedCourseResult result;
};

struct CourseBuildOutcome {
  std::optional<CourseResult> value;
  std::string diagnostic;
};

[[nodiscard]] ChartBuildOutcome
BuildChartResult(result_persistence::PersistedChartResult result,
                 std::atomic_bool &cancelled,
                 ResultChartLoader loader = {});

[[nodiscard]] CourseBuildOutcome
BuildCourseResult(result_persistence::PersistedCourseResult result,
                  std::atomic_bool &cancelled,
                  ResultChartLoader loader = {});

} // namespace result_recall
