#pragma once

#include "../CoursePlaySession.h"
#include "../ResultPersistenceCoordinator.h"

#include <memory>

namespace result_scene_detail {

[[nodiscard]] inline bool applyCoursePersistenceReceipt(
    const std::shared_ptr<const result_persistence::CompletedCourseAttempt>
        &attempt,
    const result_persistence::SaveOutcome &outcome,
    CoursePlaySession &session) noexcept {
  if (attempt == nullptr || !outcome.saved() || !outcome.receipt.has_value() ||
      !attempt->result.attemptId.has_value() ||
      outcome.receipt->attemptId != *attempt->result.attemptId ||
      outcome.receipt->resultId <= 0 || outcome.receipt->createdAt.empty()) {
    return false;
  }
  session.savedCourseReplayId = outcome.receipt->resultId;
  session.courseReplaySaved = true;
  session.courseReplayPlaybackData =
      std::make_shared<replay::CourseReplayPlaybackData>(attempt->replay);
  return true;
}

} // namespace result_scene_detail
