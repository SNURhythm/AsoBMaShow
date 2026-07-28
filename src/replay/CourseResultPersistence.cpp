#include "CourseResultPersistence.h"

#include "../ProfileDatabaseActivity.h"

#include <memory>
#include <string>
#include <utility>

namespace replay {
namespace {

constexpr std::size_t kMaximumRecoveryDiagnosticBytes = 4096;

void appendDiagnostic(std::string &destination, std::string_view phase,
                      std::string_view diagnostic) {
  if (diagnostic.empty() ||
      destination.size() >= kMaximumRecoveryDiagnosticBytes) {
    return;
  }
  const auto appendBounded = [&](std::string_view value) {
    const std::size_t available =
        kMaximumRecoveryDiagnosticBytes - destination.size();
    destination.append(value.substr(0, available));
  };
  if (!destination.empty()) {
    appendBounded("\n");
  }
  appendBounded(phase);
  appendBounded(": ");
  appendBounded(diagnostic);
}

std::optional<result_persistence::PendingCourseScoreWrite>
makePendingScoreWrite(result_persistence::ModernCourseResult result,
                      std::string_view expectedAttemptId, int resultId,
                      std::string createdAt, std::string &diagnostic) {
  if (resultId <= 0 || createdAt.empty() ||
      result.attemptId != expectedAttemptId ||
      (result.resultId != 0 && result.resultId != resultId)) {
    diagnostic = "course result identity disagrees with its durable owner";
    return std::nullopt;
  }
  result.resultId = 0;
  if (!result_persistence::validateModernCourseResult(result, diagnostic)) {
    if (diagnostic.empty()) {
      diagnostic = "course result facts are invalid";
    }
    return std::nullopt;
  }
  return result_persistence::PendingCourseScoreWrite{
      .attemptId = std::string(expectedAttemptId),
      .modernResultId = resultId,
      .createdAt = std::move(createdAt),
      .result = std::move(result),
  };
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
      .acknowledgeScore =
          [&replayRepository](std::string_view attemptId, int resultId) {
            return replayRepository.AcknowledgePendingModernCourseScore(
                attemptId, resultId);
          },
      .listScoreSources =
          [&replayRepository](int afterResultId, std::size_t limit) {
            return replayRepository.ListModernCourseScoreSourcesAfter(
                afterResultId, limit);
          },
  };
}

CourseResultPersistence::CourseResultPersistence(
    CourseResultPersistenceDependencies dependencies)
    : dependencies_(std::move(dependencies)) {}

CourseResultPersistenceOutcome CourseResultPersistence::persist(
    const CapturedCourseReplayAttempt &attempt) const {
  profile_database_activity::WriteGuard bindingLease;
  auto resultOutcome = dependencies_.persistResult(attempt);
  if (!resultOutcome.saved()) {
    return {.state = failedResultState(resultOutcome.state),
            .receipt = resultOutcome.receipt,
            .replayAttached = resultOutcome.replayAttached,
            .diagnostic = std::move(resultOutcome.diagnostic)};
  }
  std::string pendingDiagnostic;
  std::optional<result_persistence::PendingCourseScoreWrite> pending;
  if (!resultOutcome.receipt ||
      resultOutcome.receipt->attemptId != attempt.result.attemptId) {
    pendingDiagnostic =
        "durable receipt disagrees with the captured attempt";
  } else {
    pending = makePendingScoreWrite(
        attempt.result, resultOutcome.receipt->attemptId,
        resultOutcome.receipt->resultId, resultOutcome.receipt->createdAt,
        pendingDiagnostic);
  }
  if (!pending) {
    appendDiagnostic(resultOutcome.diagnostic, "course result",
                     pendingDiagnostic);
    return {.state = CourseResultPersistenceState::IntegrityConflict,
            .receipt = resultOutcome.receipt,
            .replayAttached = resultOutcome.replayAttached,
            .diagnostic = std::move(resultOutcome.diagnostic)};
  }

  const auto projection = dependencies_.projectScore(*pending);
  appendDiagnostic(resultOutcome.diagnostic, "course score projection",
                   projection.diagnostic);
  switch (projection.status) {
  case result_persistence::ProjectionStatus::Inserted:
  case result_persistence::ProjectionStatus::AlreadyPresent: {
    if (!dependencies_.acknowledgeScore) {
      appendDiagnostic(resultOutcome.diagnostic, "course score checkpoint",
                       "course score acknowledgement is not configured");
      return {.state = CourseResultPersistenceState::PendingScore,
              .receipt = resultOutcome.receipt,
              .replayAttached = resultOutcome.replayAttached,
              .diagnostic = std::move(resultOutcome.diagnostic)};
    }
    const auto acknowledged = dependencies_.acknowledgeScore(
        pending->attemptId, pending->modernResultId);
    appendDiagnostic(resultOutcome.diagnostic, "course score checkpoint",
                     acknowledged.diagnostic);
    if (acknowledged.status ==
            result_persistence::AcknowledgeStatus::StorageFailure ||
        acknowledged.status ==
            result_persistence::AcknowledgeStatus::IntegrityConflict) {
      return {.state =
                  acknowledged.status ==
                          result_persistence::AcknowledgeStatus::StorageFailure
                      ? CourseResultPersistenceState::PendingScore
                      : CourseResultPersistenceState::IntegrityConflict,
              .receipt = resultOutcome.receipt,
              .replayAttached = resultOutcome.replayAttached,
              .diagnostic = std::move(resultOutcome.diagnostic)};
    }
    return {.state = resultOutcome.replayAttached
                         ? CourseResultPersistenceState::SavedWithReplay
                         : CourseResultPersistenceState::SavedWithoutReplay,
            .receipt = resultOutcome.receipt,
            .replayAttached = resultOutcome.replayAttached,
            .diagnostic = std::move(resultOutcome.diagnostic)};
  }
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

CourseResultRecoverySummary
CourseResultPersistence::recoverAll(std::size_t pageLimit) const {
  profile_database_activity::WriteGuard bindingLease;
  CourseResultRecoverySummary summary;
  if (!dependencies_.listScoreSources || !dependencies_.projectScore ||
      !dependencies_.acknowledgeScore || pageLimit == 0 ||
      pageLimit > kMaximumModernCourseScoreSourceRows) {
    summary.pending = 1;
    summary.diagnostic = "course score recovery is not configured";
    return summary;
  }

  int cursor = 0;
  while (true) {
    auto batch = dependencies_.listScoreSources(cursor, pageLimit);
    appendDiagnostic(summary.diagnostic, "course score source",
                     batch.diagnostic);
    if (batch.status != ModernCourseScoreSourceBatchStatus::Loaded) {
      if (batch.status == ModernCourseScoreSourceBatchStatus::StorageFailure) {
        ++summary.pending;
      } else {
        ++summary.conflicts;
      }
      return summary;
    }
    if (batch.entries.empty()) {
      if (batch.hasMore) {
        ++summary.conflicts;
        appendDiagnostic(summary.diagnostic, "course score source",
                         "pagination made no forward progress");
      }
      return summary;
    }

    bool pageOrderInvalid = false;
    for (auto &entry : batch.entries) {
      ++summary.attempted;
      if (entry.resultId <= cursor) {
        ++summary.conflicts;
        appendDiagnostic(summary.diagnostic, "course score source",
                         "result IDs are not strictly increasing");
        pageOrderInvalid = true;
        break;
      }
      cursor = entry.resultId;
      const std::string phase =
          "course result " + std::to_string(entry.resultId);
      if (entry.status != ModernCourseScoreSourceEntryStatus::Loaded ||
          !entry.source || entry.source->resultId != entry.resultId) {
        ++summary.conflicts;
        appendDiagnostic(summary.diagnostic, phase,
                         entry.diagnostic.empty()
                             ? "stored score source is inconsistent"
                             : entry.diagnostic);
        continue;
      }

      std::string pendingDiagnostic;
      const std::string attemptId = entry.source->result.attemptId;
      const int resultId = entry.source->resultId;
      std::string createdAt = std::move(entry.source->createdAt);
      auto storedResult = std::move(entry.source->result);
      auto pending =
          makePendingScoreWrite(std::move(storedResult), attemptId, resultId,
                                std::move(createdAt), pendingDiagnostic);
      if (!pending) {
        ++summary.conflicts;
        appendDiagnostic(summary.diagnostic, phase, pendingDiagnostic);
        continue;
      }
      const auto projected = dependencies_.projectScore(*pending);
      appendDiagnostic(summary.diagnostic, phase, projected.diagnostic);
      switch (projected.status) {
      case result_persistence::ProjectionStatus::Inserted:
      case result_persistence::ProjectionStatus::AlreadyPresent: {
        const auto acknowledged = dependencies_.acknowledgeScore(
            pending->attemptId, pending->modernResultId);
        appendDiagnostic(summary.diagnostic, phase, acknowledged.diagnostic);
        if (acknowledged.status ==
                result_persistence::AcknowledgeStatus::Acknowledged ||
            acknowledged.status ==
                result_persistence::AcknowledgeStatus::AlreadyAcknowledged) {
          ++summary.saved;
        } else if (acknowledged.status ==
                   result_persistence::AcknowledgeStatus::StorageFailure) {
          ++summary.pending;
        } else {
          ++summary.conflicts;
        }
        break;
      }
      case result_persistence::ProjectionStatus::StorageFailure:
        ++summary.pending;
        break;
      case result_persistence::ProjectionStatus::IntegrityConflict:
        ++summary.conflicts;
        break;
      }
    }
    if (pageOrderInvalid || !batch.hasMore) {
      return summary;
    }
  }
}

} // namespace replay
