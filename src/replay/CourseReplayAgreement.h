#pragma once

#include "BeatorajaReplayCodec.h"
#include "BeatorajaReplayPath.h"
#include "ReplayReferenceAgreement.h"

#include "../ModernResult.h"

#include <span>
#include <string>

namespace replay {

enum class CourseReplayAgreementIssue {
  None,
  Result,
  Path,
  Playback,
  CourseShape,
  StageIdentity,
  LongNoteMode,
  SharedSetup,
};

struct CourseReplayAgreement {
  CourseReplayAgreementIssue issue = CourseReplayAgreementIssue::None;
  std::size_t stageIndex = 0;
  std::string diagnostic;

  [[nodiscard]] bool agrees() const noexcept {
    return issue == CourseReplayAgreementIssue::None;
  }
};

[[nodiscard]] CourseReplayAgreement compareCourseReplayPathToResult(
    const CoursePathInput &path,
    const result_persistence::ModernCourseResult &result,
    const ReplayLimits &limits = kReplayLimits) noexcept;

[[nodiscard]] CourseReplayAgreement compareCourseReplayToResult(
    const ReplayCourseDocument &replay,
    const result_persistence::ModernCourseResult &result,
    std::span<const ReplaySetupSource> sources,
    const ReplayLimits &limits = kReplayLimits) noexcept;

} // namespace replay
