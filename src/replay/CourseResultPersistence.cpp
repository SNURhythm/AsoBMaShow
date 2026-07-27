#include "CourseResultPersistence.h"

#include <memory>
#include <utility>

namespace replay {
namespace {

void appendDiagnostic(std::string &destination, std::string_view phase,
                      std::string_view diagnostic) {
  if (diagnostic.empty()) {
    return;
  }
  if (!destination.empty()) {
    destination.push_back('\n');
  }
  destination.append(phase);
  destination.append(": ");
  destination.append(diagnostic);
}

CourseResultPersistenceState
failedResultState(CourseReplayPersistenceState state) noexcept {
  switch (state) {
  case CourseReplayPersistenceState::InvalidAttempt:
    return CourseResultPersistenceState::InvalidAttempt;
  case CourseReplayPersistenceState::IntegrityConflict:
    return CourseResultPersistenceState::IntegrityConflict;
  case CourseReplayPersistenceState::Retryable:
    return CourseResultPersistenceState::Retryable;
  case CourseReplayPersistenceState::SavedWithReplay:
  case CourseReplayPersistenceState::SavedWithoutReplay:
    break;
  }
  return CourseResultPersistenceState::IntegrityConflict;
}

} // namespace

CourseResultPersistence::CourseResultPersistence(
    ScoreRepository &score, ReplayRepository &replayRepository) {
  auto resultPersistence =
      std::make_shared<CourseReplayPersistence>(replayRepository);
  dependencies_ = {
      .persistResult =
          [resultPersistence](const CapturedCourseReplayAttempt &attempt) {
            return resultPersistence->persist(attempt);
          },
      .projectScore =
          [&score](const result_persistence::PendingCourseScoreWrite &pending) {
            return score.SaveProjectedCourseScore(pending);
          },
  };
}

CourseResultPersistence::CourseResultPersistence(
    CourseResultPersistenceDependencies dependencies)
    : dependencies_(std::move(dependencies)) {}

CourseResultPersistenceOutcome CourseResultPersistence::persist(
    const CapturedCourseReplayAttempt &attempt) const {
  auto resultOutcome = dependencies_.persistResult(attempt);
  if (!resultOutcome.saved()) {
    return {.state = failedResultState(resultOutcome.state),
            .receipt = resultOutcome.receipt,
            .replayAttached = resultOutcome.replayAttached,
            .diagnostic = std::move(resultOutcome.diagnostic)};
  }
  if (!resultOutcome.receipt ||
      resultOutcome.receipt->attemptId != attempt.result.attemptId ||
      resultOutcome.receipt->resultId <= 0 ||
      resultOutcome.receipt->createdAt.empty()) {
    appendDiagnostic(resultOutcome.diagnostic, "course result",
                     "durable receipt disagrees with the captured attempt");
    return {.state = CourseResultPersistenceState::IntegrityConflict,
            .receipt = resultOutcome.receipt,
            .replayAttached = resultOutcome.replayAttached,
            .diagnostic = std::move(resultOutcome.diagnostic)};
  }

  const result_persistence::PendingCourseScoreWrite pending{
      .attemptId = attempt.result.attemptId,
      .modernResultId = resultOutcome.receipt->resultId,
      .createdAt = resultOutcome.receipt->createdAt,
      .result = attempt.result,
  };
  const auto projection = dependencies_.projectScore(pending);
  appendDiagnostic(resultOutcome.diagnostic, "course score projection",
                   projection.diagnostic);
  switch (projection.status) {
  case result_persistence::ProjectionStatus::Inserted:
  case result_persistence::ProjectionStatus::AlreadyPresent:
    return {.state = resultOutcome.replayAttached
                         ? CourseResultPersistenceState::SavedWithReplay
                         : CourseResultPersistenceState::SavedWithoutReplay,
            .receipt = resultOutcome.receipt,
            .replayAttached = resultOutcome.replayAttached,
            .diagnostic = std::move(resultOutcome.diagnostic)};
  case result_persistence::ProjectionStatus::StorageFailure:
    return {.state = CourseResultPersistenceState::PendingScore,
            .receipt = resultOutcome.receipt,
            .replayAttached = resultOutcome.replayAttached,
            .diagnostic = std::move(resultOutcome.diagnostic)};
  case result_persistence::ProjectionStatus::IntegrityConflict:
    return {.state = CourseResultPersistenceState::IntegrityConflict,
            .receipt = resultOutcome.receipt,
            .replayAttached = resultOutcome.replayAttached,
            .diagnostic = std::move(resultOutcome.diagnostic)};
  }
  return {.state = CourseResultPersistenceState::IntegrityConflict,
          .receipt = resultOutcome.receipt,
          .replayAttached = resultOutcome.replayAttached,
          .diagnostic = "course score projection returned an unknown state"};
}

} // namespace replay
