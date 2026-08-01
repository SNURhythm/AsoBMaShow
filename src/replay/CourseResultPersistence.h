#pragma once

#include "CourseReplayPersistence.h"

#include "../repositories/ScoreRepository.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

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
  std::function<result_persistence::AcknowledgeOutcome(std::string_view, int)>
      acknowledgeScore;
  std::function<ModernCourseScoreSourceBatchOutcome(int, std::size_t)>
      listScoreSources;
};

struct CourseResultRecoverySummary {
  std::size_t attempted = 0;
  std::size_t saved = 0;
  std::size_t pending = 0;
  std::size_t conflicts = 0;
  std::string diagnostic;
};

class CourseResultPersistence {
public:
  CourseResultPersistence(ScoreRepository &score,
                          ReplayRepository &replayRepository);
  explicit CourseResultPersistence(
      CourseResultPersistenceDependencies dependencies);

  [[nodiscard]] CourseResultPersistenceOutcome
  persist(const CapturedCourseReplayAttempt &attempt) const;
  [[nodiscard]] CourseResultRecoverySummary
  recoverAll(std::size_t pageLimit = 256) const;

private:
  CourseResultPersistenceDependencies dependencies_;
};

} // namespace replay
