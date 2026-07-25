#pragma once

#include "ResultPersistenceModel.h"
#include "ir/IrSubmissionSnapshot.h"
#include "replay/ReplayPlaybackData.h"

namespace result_persistence {

struct CompletedChartAttempt {
  PersistedChartResult result;
  replay::ReplayPlaybackData replay;
  ir::IrSubmissionSnapshot irSnapshot;
};

struct CompletedCourseAttempt {
  PersistedCourseResult result;
  replay::CourseReplayPlaybackData replay;
};

} // namespace result_persistence
