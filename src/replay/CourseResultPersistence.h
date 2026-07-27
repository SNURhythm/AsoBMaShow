#pragma once

#include "CourseReplayPersistence.h"

#include "../repositories/ScoreRepository.h"

#include <functional>
#include <optional>
#include <string>

namespace replay {

enum class CourseResultPersistenceState {
  SavedWithReplay,
  SavedWithoutReplay,
  PendingScore,
  Retryable,
  InvalidAttempt,
  IntegrityConflict,
};

struct CourseResultPersistenceOutcome {
  CourseResultPersistenceState state = CourseResultPersistenceState::Retryable;
  std::optional<ModernCourseStageReceipt> receipt;
  bool replayAttached = false;
  std::string diagnostic;

  [[nodiscard]] bool saved() const noexcept {
    return state == CourseResultPersistenceState::SavedWithReplay ||
           state == CourseResultPersistenceState::SavedWithoutReplay;
  }

  [[nodiscard]] bool durableResult() const noexcept {
    return receipt.has_value();
  }

  [[nodiscard]] bool retryable() const noexcept {
    return state == CourseResultPersistenceState::Retryable ||
           state == CourseResultPersistenceState::PendingScore;
  }

  bool operator==(const CourseResultPersistenceOutcome &) const = default;
};

struct CourseResultPersistenceDependencies {
  std::function<CourseReplayPersistenceOutcome(
      const CapturedCourseReplayAttempt &)>
      persistResult;
  std::function<result_persistence::ProjectionOutcome(
      const result_persistence::PendingCourseScoreWrite &)>
      projectScore;
};

class CourseResultPersistence {
public:
  CourseResultPersistence(ScoreRepository &score,
                          ReplayRepository &replayRepository);
  explicit CourseResultPersistence(
      CourseResultPersistenceDependencies dependencies);

  [[nodiscard]] CourseResultPersistenceOutcome
  persist(const CapturedCourseReplayAttempt &attempt) const;

private:
  CourseResultPersistenceDependencies dependencies_;
};

} // namespace replay
