#pragma once

#include "CoursePlaySession.h"
#include "ModernResultRecallBuilder.h"
#include "ResultPersistenceCoordinator.h"
#include "ir/IrSubmission.h"
#include "repositories/ReplayRepository.h"

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace result_recall {

using ReplayChartLoader = std::function<std::unique_ptr<bms_parser::Chart>(
    const ReplayData &, std::atomic_bool &)>;

struct HistoricalIrContext {
  std::shared_ptr<const result_persistence::ChartResultAttempt> attempt;
  std::shared_ptr<const ir::IrSubmission> submission;
  result_persistence::SaveOutcome saveOutcome;
};

struct ChartResult {
  std::unique_ptr<bms_parser::Chart> chart;
  ReplayData replay;
  RhythmState state;
  std::optional<HistoricalIrContext> historicalIr;
  std::string historicalIrDiagnostic;
};

struct ChartBuildOutcome {
  std::optional<ChartResult> value;
  std::string diagnostic;
};

struct CourseResult {
  std::shared_ptr<CoursePlaySession> session;
};

struct CourseBuildOutcome {
  std::optional<CourseResult> value;
  std::string diagnostic;
};

[[nodiscard]] ChartBuildOutcome
BuildChartResult(ReplayResultRecord record, std::atomic_bool &cancelled,
                 ReplayChartLoader loader = {});

[[nodiscard]] CourseBuildOutcome
BuildCourseResult(CourseReplayData replay, std::atomic_bool &cancelled,
                  ReplayChartLoader loader = {});

} // namespace result_recall
