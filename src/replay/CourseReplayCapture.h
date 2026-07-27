#pragma once

#include "BeatorajaReplayCodec.h"
#include "BeatorajaReplayPath.h"
#include "CourseContinuation.h"

#include "../ModernResult.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace replay {

struct CourseReplayStageCapture {
  std::optional<ReplayPlaybackData> playback;
  ReplayTimeBounds timeBounds;
  std::int64_t restMicrosAfterStage = 0;
};

struct CourseReplayCapture {
  result_persistence::ModernCourseResult result;
  std::vector<CourseReplayStageCapture> stages;
  CourseContinuationConstraints constraints;
};

struct CapturedCourseReplayAttempt {
  result_persistence::ModernCourseResult result;
  std::optional<ReplayCourseDocument> replay;
  CoursePathInput pathInput;
};

[[nodiscard]] std::optional<CapturedCourseReplayAttempt>
captureCourseReplayAttempt(const CourseReplayCapture &capture,
                           std::string &diagnostic) noexcept;

} // namespace replay
