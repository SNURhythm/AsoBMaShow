#include "ChartReplayPersistence.h"

#include "BeatorajaReplayPath.h"
#include "ReplayFileStore.h"

#include "../ProfileDatabaseActivity.h"
#include "../ResultPersistenceCoordinator.h"

#include <memory>
#include <string>
#include <utility>

namespace replay {
namespace {

constexpr std::string_view kRecoveryMessage =
    "Some previously completed results are still waiting to be saved. They "
    "were kept safely and will be retried later.";

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

ChartReplayPersistenceState savedState(bool replayAttached) noexcept {
  return replayAttached ? ChartReplayPersistenceState::SavedWithReplay
                        : ChartReplayPersistenceState::SavedWithoutReplay;
}

} // namespace

std::string_view chartReplayRecoveryUserMessage() noexcept {
  return kRecoveryMessage;
}

ChartReplayPersistence::ChartReplayPersistence(
    ScoreRepository &score, ReplayRepository &repository) {
  auto store = std::make_shared<ReplayFileStore>(
      repository.GetResolvedProfileRoot());
  auto codec = std::make_shared<BeatorajaReplayCodec>();
  dependencies_ = {
      .loadResult =
          [&repository](std::string_view attemptId) {
            return repository.LoadModernChartResultByAttempt(attemptId);
          },
      .fileAssociation =
          {.reservePath =
               [&repository](std::string_view attemptId,
                             std::string_view stem, std::int64_t playedAt) {
                 return repository.ReserveModernReplayPath(attemptId, stem,
                                                           playedAt);
               },
           .releasePath =
               [&repository](const auto &reservation) {
                 return repository.ReleaseModernReplayPathReservation(
                     reservation);
               },
           .reserveFile =
               [store](const ReplayPathIdentity &identity,
                       std::span<const std::byte> bytes,
                       std::string_view attemptToken) {
                 return store->reserve(identity, bytes, attemptToken);
               },
           .installFile =
               [store](const ReplayFileReservation &reservation,
                       std::span<const std::byte> bytes) {
                 return store->install(reservation, bytes);
               },
           .inspectFile =
               [store](const ReplayFileMetadata &metadata) {
                 return store->inspect(metadata);
               },
           .removeIfMatches =
               [store](const ReplayFileMetadata &metadata,
                       std::string &diagnostic) {
                 return store->removeIfMatches(metadata, diagnostic);
               }},
      .encode =
          [codec](const ReplayChartDocument &replay, std::int64_t playedAt,
                  std::string &diagnostic) {
            return codec->encodeChart(replay, playedAt, diagnostic);
          },
      .stage =
          [&repository](const auto &result, const auto &snapshot,
                        const auto &replayFile, auto drafts) {
            return repository.StageModernChartResult(result, snapshot,
                                                     replayFile, drafts);
          },
      .loadPending =
          [&repository](std::string_view attemptId) {
            return repository.LoadPendingModernChartScore(attemptId);
          },
      .listPending =
          [&repository](std::size_t limit) {
            return repository.ListPendingModernChartScores(limit);
          },
      .project =
          [&score](const auto &pending) {
            return score.SaveProjectedScore(pending);
          },
      .acknowledge =
          [&repository](std::string_view attemptId, int resultId) {
            return repository.AcknowledgePendingModernChartScore(attemptId,
                                                                 resultId);
          },
      .recordRecoveryAttempt =
          [&repository](std::string_view attemptId,
                        result_persistence::RecoveryAttemptKind kind) {
            return repository.RecordPendingModernChartScoreRecoveryAttempt(
                attemptId, kind);
          },
  };
}

ChartReplayPersistence::ChartReplayPersistence(
    ChartReplayPersistenceDependencies dependencies)
    : dependencies_(std::move(dependencies)) {}

ChartReplayPersistenceOutcome
ChartReplayPersistence::persist(const ChartReplayPersistenceAttempt &attempt,
                                std::span<const ir::IrOutboxDraft> irDrafts) {
  profile_database_activity::WriteGuard writeGuard;
  std::string diagnostic;
  if (attempt.result.resultId != 0 ||
      !result_persistence::validateModernChartResult(attempt.result,
                                                     diagnostic)) {
    return {.state = ChartReplayPersistenceState::InvalidAttempt,
            .diagnostic = diagnostic.empty() ? "modern result is invalid"
                                             : std::move(diagnostic)};
  }
  if (attempt.irSnapshot.has_value()) {
    const auto expected =
        ir::captureIrSubmissionSnapshot(attempt.result, diagnostic);
    if (!expected || *expected != *attempt.irSnapshot) {
      return {.state = ChartReplayPersistenceState::InvalidAttempt,
              .diagnostic = diagnostic.empty()
                                ? "IR snapshot differs from modern result"
                                : std::move(diagnostic)};
    }
  }

  std::optional<ModernReplayFileAttachment> attachment;
  bool suppressNewReplay = false;
  const auto existing = dependencies_.loadResult(attempt.result.attemptId);
  switch (existing.status) {
  case ModernChartResultReadStatus::Loaded: {
    if (!existing.record) {
      return {.state = ChartReplayPersistenceState::IntegrityConflict,
              .diagnostic = "loaded modern result has no payload"};
    }
    auto expected = attempt.result;
    expected.resultId = existing.record->result.resultId;
    if (existing.record->result != expected) {
      return {.state = ChartReplayPersistenceState::IntegrityConflict,
              .diagnostic = "attempt ID already names a different result"};
    }
    suppressNewReplay = !existing.record->replayFile.has_value();
    if (existing.record->replayFile) {
      attachment = ModernReplayFileAttachment{
          .identity = existing.record->replayFile->identity,
          .metadata = existing.record->replayFile->metadata};
    }
    break;
  }
  case ModernChartResultReadStatus::NotFound:
    break;
  case ModernChartResultReadStatus::Invalid:
  case ModernChartResultReadStatus::IntegrityConflict:
    return {.state = ChartReplayPersistenceState::IntegrityConflict,
            .diagnostic = existing.diagnostic.empty()
                              ? "existing modern result is inconsistent"
                              : existing.diagnostic};
  case ModernChartResultReadStatus::StorageFailure:
    return {.state = ChartReplayPersistenceState::Retryable,
            .diagnostic = existing.diagnostic.empty()
                              ? "could not inspect modern result retry"
                              : existing.diagnostic};
  }

  ReplayFileAssociationCoordinator fileCoordinator(
      dependencies_.fileAssociation);
  std::optional<ReplayFileAssociation> fileAssociation;
  if (!attachment && !suppressNewReplay && attempt.replay.has_value()) {
    const auto agreement =
        compareChartReplayToResult(*attempt.replay, attempt.result);
    if (!agreement.agrees()) {
      appendDiagnostic(diagnostic, "replay omitted", agreement.diagnostic);
    } else {
      std::string pathDiagnostic;
      const auto stem = chartStem(
          attempt.result.score.chartSha256, attempt.result.score.longNoteMode,
          attempt.replay->playback.setup.hasUndefinedLongNotes, pathDiagnostic);
      if (!stem) {
        appendDiagnostic(diagnostic, "replay omitted", pathDiagnostic);
      } else {
        const auto associated = fileCoordinator.associate(
            attempt.result.attemptId, *stem,
            attempt.result.playedAtUnixMillis,
            [this, &attempt](std::string &encodeDiagnostic) {
              return dependencies_.encode(
                  *attempt.replay, attempt.result.playedAtUnixMillis,
                  encodeDiagnostic);
            });
        appendDiagnostic(diagnostic, "file association",
                         associated.diagnostic);
        if (associated.status ==
            ReplayFileAssociationStatus::IntegrityConflict) {
          return {.state = ChartReplayPersistenceState::IntegrityConflict,
                  .diagnostic = std::move(diagnostic)};
        }
        if (associated.status == ReplayFileAssociationStatus::Attached &&
            associated.association) {
          fileAssociation = *associated.association;
          attachment = fileAssociation->attachment;
        }
      }
    }
  }

  const auto staged = dependencies_.stage(attempt.result, attempt.irSnapshot,
                                          attachment, irDrafts);
  switch (staged.status) {
  case ModernChartStageStatus::Invalid:
    if (fileAssociation) {
      fileCoordinator.abandonDefinitively(*fileAssociation, diagnostic);
    }
    appendDiagnostic(diagnostic, "staging", staged.diagnostic);
    return {.state = ChartReplayPersistenceState::InvalidAttempt,
            .diagnostic = std::move(diagnostic)};
  case ModernChartStageStatus::IntegrityConflict:
    appendDiagnostic(diagnostic, "staging", staged.diagnostic);
    return {.state = ChartReplayPersistenceState::IntegrityConflict,
            .diagnostic = std::move(diagnostic)};
  case ModernChartStageStatus::StorageFailure:
    appendDiagnostic(diagnostic, "staging", staged.diagnostic);
    return {.state = ChartReplayPersistenceState::Retryable,
            .diagnostic = std::move(diagnostic)};
  case ModernChartStageStatus::Staged:
  case ModernChartStageStatus::AlreadyStaged:
    break;
  }
  if (!staged.receipt ||
      staged.receipt->attemptId != attempt.result.attemptId ||
      staged.receipt->resultId <= 0 || staged.receipt->createdAt.empty()) {
    appendDiagnostic(diagnostic, "staging", "success receipt is inconsistent");
    return {.state = ChartReplayPersistenceState::IntegrityConflict,
            .replayAttached = attachment.has_value(),
            .diagnostic = std::move(diagnostic)};
  }

  auto pending = dependencies_.loadPending(attempt.result.attemptId);
  if (pending.status == result_persistence::PendingReadStatus::StorageFailure) {
    appendDiagnostic(diagnostic, "pending score", pending.diagnostic);
    return {.state = ChartReplayPersistenceState::PendingScore,
            .receipt = staged.receipt,
            .replayAttached = attachment.has_value(),
            .diagnostic = std::move(diagnostic)};
  }
  if (pending.status ==
      result_persistence::PendingReadStatus::IntegrityConflict) {
    appendDiagnostic(diagnostic, "pending score", pending.diagnostic);
    return {.state = ChartReplayPersistenceState::IntegrityConflict,
            .receipt = staged.receipt,
            .replayAttached = attachment.has_value(),
            .diagnostic = std::move(diagnostic)};
  }
  if (pending.status == result_persistence::PendingReadStatus::NotFound) {
    const auto acknowledged = dependencies_.acknowledge(
        staged.receipt->attemptId, staged.receipt->resultId);
    if (acknowledged.status ==
        result_persistence::AcknowledgeStatus::AlreadyAcknowledged) {
      return {.state = savedState(attachment.has_value()),
              .receipt = staged.receipt,
              .replayAttached = attachment.has_value(),
              .diagnostic = std::move(diagnostic)};
    }
    appendDiagnostic(diagnostic, "pending score",
                     acknowledged.diagnostic.empty()
                         ? "pending score is missing"
                         : acknowledged.diagnostic);
    return {.state =
                acknowledged.status ==
                        result_persistence::AcknowledgeStatus::StorageFailure
                    ? ChartReplayPersistenceState::PendingAcknowledgement
                    : ChartReplayPersistenceState::IntegrityConflict,
            .receipt = staged.receipt,
            .replayAttached = attachment.has_value(),
            .diagnostic = std::move(diagnostic)};
  }
  if (!pending.value || pending.value->attemptId != attempt.result.attemptId ||
      pending.value->modernResultId != staged.receipt->resultId ||
      pending.value->createdAt != staged.receipt->createdAt ||
      !(pending.value->score == attempt.result.score)) {
    appendDiagnostic(diagnostic, "pending score",
                     "payload differs from the staged modern result");
    return {.state = ChartReplayPersistenceState::IntegrityConflict,
            .receipt = staged.receipt,
            .replayAttached = attachment.has_value(),
            .diagnostic = std::move(diagnostic)};
  }

  const auto completed = result_persistence::completePendingChartScore(
      *pending.value, {.project = dependencies_.project,
                       .acknowledge = dependencies_.acknowledge});
  appendDiagnostic(diagnostic, "score completion", completed.diagnostic);
  ChartReplayPersistenceState state =
      ChartReplayPersistenceState::IntegrityConflict;
  switch (completed.status) {
  case result_persistence::PendingScoreCompletionStatus::Saved:
    state = savedState(attachment.has_value());
    break;
  case result_persistence::PendingScoreCompletionStatus::PendingScore:
    state = ChartReplayPersistenceState::PendingScore;
    break;
  case result_persistence::PendingScoreCompletionStatus::PendingAcknowledgement:
    state = ChartReplayPersistenceState::PendingAcknowledgement;
    break;
  case result_persistence::PendingScoreCompletionStatus::IntegrityConflict:
    state = ChartReplayPersistenceState::IntegrityConflict;
    break;
  }
  return {.state = state,
          .receipt = staged.receipt,
          .replayAttached = attachment.has_value(),
          .diagnostic = std::move(diagnostic)};
}

ChartReplayRecoverySummary
ChartReplayPersistence::recoverAll(std::size_t limit) {
  profile_database_activity::WriteGuard bindingLease;
  const auto recovered = result_persistence::recoverPendingChartScores(
      {.listPending = dependencies_.listPending,
       .completion = {.project = dependencies_.project,
                      .acknowledge = dependencies_.acknowledge},
       .recordRecoveryAttempt = dependencies_.recordRecoveryAttempt},
      limit);
  return {.attempted = recovered.attempted,
          .saved = recovered.saved,
          .pending = recovered.pending,
          .conflicts = recovered.conflicts,
          .diagnostic = recovered.diagnostic};
}

} // namespace replay
