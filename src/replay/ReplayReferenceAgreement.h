#pragma once

#include "BeatorajaReplayPath.h"

#include "../ModernResult.h"

#include <optional>
#include <string>

struct ModernReplayFileReference;

namespace replay {

struct ReplayReferenceAgreement {
  bool matches = false;
  std::string diagnostic;
};

[[nodiscard]] CoursePathInput courseReplayPathInputForResult(
    const result_persistence::ModernCourseResult &result,
    bool hasUndefinedLongNotes);

[[nodiscard]] ReplayReferenceAgreement compareChartReplayReferenceToResult(
    const ModernReplayFileReference &reference,
    const result_persistence::ModernChartResult &result,
    const ReplayLimits &limits = kReplayLimits) noexcept;

[[nodiscard]] ReplayReferenceAgreement compareCourseReplayReferenceToResult(
    const ModernReplayFileReference &reference,
    const result_persistence::ModernCourseResult &result,
    std::optional<bool> hasUndefinedLongNotes = std::nullopt,
    const ReplayLimits &limits = kReplayLimits) noexcept;

} // namespace replay
