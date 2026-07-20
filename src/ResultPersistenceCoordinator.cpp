#include "ResultPersistenceCoordinator.h"

#include "ProfileDatabaseActivity.h"

#include <algorithm>
#include <string>
#include <utility>

namespace result_persistence {
namespace {

constexpr std::string_view kUnstagedMessage =
    "This result could not be stored. Retry before leaving to avoid losing "
    "it.";
constexpr std::string_view kInvalidAttemptMessage =
    "This result could not be prepared for saving and cannot be retried. "
    "Continuing will discard it.";
constexpr std::string_view kPendingScoreMessage =
    "The replay is safe, but the score is still pending. Retry now or it will "
    "be retried automatically later.";
constexpr std::string_view kPendingAcknowledgementMessage =
    "The result was stored, but save confirmation is pending. Retrying is "
    "safe.";
constexpr std::string_view kUnstagedConflictMessage =
    "This result conflicts with an existing save and was not stored. Retry "
    "before leaving; continuing will discard this result.";
constexpr std::string_view kPendingConflictMessage =
    "This saved replay could not be verified against its score. It was kept "
    "for recovery and was not overwritten.";
constexpr std::string_view kRecoveryMessage =
    "Some previously completed results are still waiting to be saved. They "
    "were kept safely and will be retried later.";

void appendDiagnostic(std::string &destination, std::string_view diagnostic) {
  if (diagnostic.empty()) {
    return;
  }
  if (!destination.empty()) {
    destination += '\n';
  }
  destination += diagnostic;
}

SaveOutcome unstagedOutcome(SaveState state, std::string diagnostic) {
  return {.state = state,
          .receipt = std::nullopt,
          .userMessage = std::string(saveStateUserMessage(state)),
          .diagnostic = std::move(diagnostic)};
}

SaveOutcome durableOutcome(SaveState state, const StageReceipt &receipt,
                           std::string diagnostic) {
  return {.state = state,
          .receipt = receipt,
          .userMessage = std::string(saveStateUserMessage(state)),
          .diagnostic = std::move(diagnostic)};
}

} // namespace

std::string_view saveStateUserMessage(SaveState state) noexcept {
  switch (state) {
  case SaveState::Saved:
    return {};
  case SaveState::InvalidAttempt:
    return kInvalidAttemptMessage;
  case SaveState::Unstaged:
    return kUnstagedMessage;
  case SaveState::PendingScore:
    return kPendingScoreMessage;
  case SaveState::PendingAcknowledgement:
    return kPendingAcknowledgementMessage;
  case SaveState::UnstagedConflict:
    return kUnstagedConflictMessage;
  case SaveState::PendingConflict:
    return kPendingConflictMessage;
  }
  return kUnstagedConflictMessage;
}

std::string_view recoveryUserMessage() noexcept { return kRecoveryMessage; }

RecoverySummary recoveryFailureSummary(std::string diagnostic) {
  return {
      .attempted = 0,
      .saved = 0,
      .pending = 1,
      .conflicts = 0,
      .userMessage = std::string(recoveryUserMessage()),
      .diagnostic = std::move(diagnostic),
  };
}

bool SaveOutcome::durable() const noexcept {
  switch (state) {
  case SaveState::Saved:
  case SaveState::PendingScore:
  case SaveState::PendingAcknowledgement:
  case SaveState::PendingConflict:
    return true;
  case SaveState::Unstaged:
  case SaveState::UnstagedConflict:
  case SaveState::InvalidAttempt:
    return false;
  }
  return false;
}

bool SaveOutcome::retryable() const noexcept {
  switch (state) {
  case SaveState::Saved:
  case SaveState::InvalidAttempt:
    return false;
  case SaveState::Unstaged:
  case SaveState::PendingScore:
  case SaveState::PendingAcknowledgement:
  case SaveState::UnstagedConflict:
  case SaveState::PendingConflict:
    return true;
  }
  return false;
}

bool SaveOutcome::requiresUserDecision(bool attemptAvailable,
                                       bool continueChosen) const noexcept {
  const bool hasPersistenceResult = attemptAvailable || !userMessage.empty();
  return hasPersistenceResult && !saved() && !continueChosen;
}

const StageReceipt *SaveOutcome::validatedReceiptFor(
    const ChartResultAttempt &attempt) const noexcept {
  if (!durable() || !receipt.has_value() ||
      receipt->attemptId != attempt.attemptId || receipt->replayId <= 0 ||
      receipt->createdAt.empty()) {
    return nullptr;
  }
  return &*receipt;
}

Coordinator::Coordinator(ScoreRepository &score, ReplayRepository &replay)
    : Coordinator(Dependencies{
          .stage =
              [&replay](const ChartResultAttempt &attempt,
                        std::span<const ir::IrOutboxDraft> irDrafts) {
                return replay.StageChartResult(attempt, irDrafts);
              },
          .loadPending =
              [&replay](std::string_view attemptId) {
                return replay.LoadPendingChartScore(attemptId);
              },
          .listPending =
              [&replay](std::size_t limit) {
                return replay.ListPendingChartScores(limit);
              },
          .project =
              [&score](const PendingChartScoreWrite &pending) {
                return score.SaveProjectedScore(pending);
              },
          .acknowledgeAndActivate =
              [&replay](std::string_view attemptId, int replayId) {
                return replay.AcknowledgePendingChartScoreAndActivateIr(
                    attemptId, replayId);
              },
          .recordRecoveryAttempt =
              [&replay](std::string_view attemptId, RecoveryAttemptKind kind) {
                return replay.RecordPendingChartScoreRecoveryAttempt(attemptId,
                                                                     kind);
              },
      }) {}

Coordinator::Coordinator(Dependencies dependencies)
    : dependencies_(std::move(dependencies)) {}

SaveOutcome Coordinator::persist(
    const ChartResultAttempt &attempt,
    std::span<const ir::IrOutboxDraft> irDrafts) {
  profile_database_activity::WriteGuard bindingLease;
  StageOutcome staged = dependencies_.stage(attempt, irDrafts);
  switch (staged.status) {
  case StageStatus::StorageFailure:
    return unstagedOutcome(SaveState::Unstaged, std::move(staged.diagnostic));
  case StageStatus::IntegrityConflict:
    return unstagedOutcome(SaveState::UnstagedConflict,
                           std::move(staged.diagnostic));
  case StageStatus::Staged:
  case StageStatus::AlreadyStaged:
    break;
  default:
    appendDiagnostic(staged.diagnostic, "unknown staging status");
    return unstagedOutcome(SaveState::UnstagedConflict,
                           std::move(staged.diagnostic));
  }
  if (!staged.receipt.has_value() ||
      staged.receipt->attemptId != attempt.attemptId ||
      staged.receipt->replayId <= 0 || staged.receipt->createdAt.empty() ||
      (staged.status == StageStatus::Staged && !staged.receipt->scorePending)) {
    appendDiagnostic(staged.diagnostic,
                     "staging returned inconsistent success metadata");
    return unstagedOutcome(SaveState::UnstagedConflict,
                           std::move(staged.diagnostic));
  }

  StageReceipt receipt = *staged.receipt;
  if (!receipt.scorePending) {
    return durableOutcome(SaveState::Saved, receipt,
                          std::move(staged.diagnostic));
  }

  PendingReadOutcome loaded = dependencies_.loadPending(attempt.attemptId);
  appendDiagnostic(staged.diagnostic, loaded.diagnostic);
  switch (loaded.status) {
  case PendingReadStatus::StorageFailure:
    return durableOutcome(SaveState::PendingScore, receipt,
                          std::move(staged.diagnostic));
  case PendingReadStatus::NotFound:
  case PendingReadStatus::IntegrityConflict:
    return durableOutcome(SaveState::PendingConflict, receipt,
                          std::move(staged.diagnostic));
  case PendingReadStatus::Found:
    if (!loaded.value.has_value()) {
      appendDiagnostic(staged.diagnostic,
                       "pending read returned Found without a payload");
      return durableOutcome(SaveState::PendingConflict, receipt,
                            std::move(staged.diagnostic));
    }
    break;
  default:
    appendDiagnostic(staged.diagnostic, "unknown pending read status");
    return durableOutcome(SaveState::PendingConflict, receipt,
                          std::move(staged.diagnostic));
  }

  const PendingChartScoreWrite &pending = *loaded.value;
  if (pending.attemptId != attempt.attemptId ||
      pending.attemptId != receipt.attemptId ||
      pending.replayId != receipt.replayId) {
    appendDiagnostic(staged.diagnostic,
                     "pending score identity does not match its receipt");
    return durableOutcome(SaveState::PendingConflict, receipt,
                          std::move(staged.diagnostic));
  }
  if (pending.createdAt != receipt.createdAt) {
    appendDiagnostic(staged.diagnostic,
                     "pending score timestamp does not match its receipt");
    return durableOutcome(SaveState::PendingConflict, receipt,
                          std::move(staged.diagnostic));
  }
  if (!(pending.score == attempt.score)) {
    appendDiagnostic(
        staged.diagnostic,
        "pending score payload does not match the current attempt");
    return durableOutcome(SaveState::PendingConflict, receipt,
                          std::move(staged.diagnostic));
  }

  ProjectionOutcome projected = dependencies_.project(pending);
  appendDiagnostic(staged.diagnostic, projected.diagnostic);
  switch (projected.status) {
  case ProjectionStatus::StorageFailure:
    return durableOutcome(SaveState::PendingScore, receipt,
                          std::move(staged.diagnostic));
  case ProjectionStatus::IntegrityConflict:
    return durableOutcome(SaveState::PendingConflict, receipt,
                          std::move(staged.diagnostic));
  case ProjectionStatus::Inserted:
  case ProjectionStatus::AlreadyPresent:
    break;
  default:
    appendDiagnostic(staged.diagnostic, "unknown projection status");
    return durableOutcome(SaveState::PendingConflict, receipt,
                          std::move(staged.diagnostic));
  }

  AcknowledgeOutcome acknowledged =
      dependencies_.acknowledgeAndActivate(pending.attemptId,
                                           pending.replayId);
  appendDiagnostic(staged.diagnostic, acknowledged.diagnostic);
  switch (acknowledged.status) {
  case AcknowledgeStatus::StorageFailure:
    return durableOutcome(SaveState::PendingAcknowledgement, receipt,
                          std::move(staged.diagnostic));
  case AcknowledgeStatus::IntegrityConflict:
    return durableOutcome(SaveState::PendingConflict, receipt,
                          std::move(staged.diagnostic));
  case AcknowledgeStatus::Acknowledged:
  case AcknowledgeStatus::AlreadyAcknowledged:
    break;
  default:
    appendDiagnostic(staged.diagnostic, "unknown acknowledgement status");
    return durableOutcome(SaveState::PendingConflict, receipt,
                          std::move(staged.diagnostic));
  }

  receipt.scorePending = false;
  return durableOutcome(SaveState::Saved, receipt,
                        std::move(staged.diagnostic));
}

RecoverySummary Coordinator::recoverAll(std::size_t limit) {
  profile_database_activity::WriteGuard bindingLease;
  const std::size_t effectiveLimit = std::min(limit, std::size_t{256});
  PendingBatchOutcome batch = dependencies_.listPending(effectiveLimit);
  RecoverySummary summary;
  appendDiagnostic(summary.diagnostic, batch.diagnostic);
  if (!batch.storageAvailable) {
    summary.pending = 1;
    summary.userMessage = recoveryUserMessage();
    return summary;
  }
  summary.pending = batch.remaining;
  if (batch.remaining != 0) {
    appendDiagnostic(summary.diagnostic,
                     std::to_string(batch.remaining) +
                         " pending result rows remain beyond the recovery "
                         "batch");
  }

  const auto retain = [&](const PendingBatchEntry &entry,
                          RecoveryAttemptKind kind,
                          std::string_view diagnostic) {
    if (kind == RecoveryAttemptKind::StorageFailure) {
      ++summary.pending;
    } else {
      ++summary.conflicts;
    }
    appendDiagnostic(summary.diagnostic, diagnostic);
    const RecoveryMarkOutcome marked =
        dependencies_.recordRecoveryAttempt(entry.attemptId, kind);
    appendDiagnostic(summary.diagnostic, marked.diagnostic);
    const auto reclassifyAsConflict = [&] {
      if (kind == RecoveryAttemptKind::StorageFailure) {
        --summary.pending;
        ++summary.conflicts;
      }
    };
    switch (marked.status) {
    case RecoveryMarkStatus::Recorded:
      break;
    case RecoveryMarkStatus::NotFound:
      reclassifyAsConflict();
      appendDiagnostic(summary.diagnostic,
                       "recovery marker did not find the pending row");
      break;
    case RecoveryMarkStatus::StorageFailure:
      appendDiagnostic(summary.diagnostic,
                       "recovery marker could not be recorded");
      break;
    default:
      reclassifyAsConflict();
      appendDiagnostic(summary.diagnostic, "unknown recovery marker status");
      break;
    }
  };

  const std::size_t entryCount = std::min(effectiveLimit, batch.entries.size());
  for (std::size_t entryIndex = 0; entryIndex < entryCount; ++entryIndex) {
    const PendingBatchEntry &entry = batch.entries[entryIndex];
    ++summary.attempted;
    switch (entry.status) {
    case PendingReadStatus::StorageFailure:
      retain(entry, RecoveryAttemptKind::StorageFailure, entry.diagnostic);
      continue;
    case PendingReadStatus::NotFound:
    case PendingReadStatus::IntegrityConflict:
      retain(entry, RecoveryAttemptKind::IntegrityConflict, entry.diagnostic);
      continue;
    case PendingReadStatus::Found:
      if (!entry.value.has_value()) {
        std::string diagnostic = entry.diagnostic;
        appendDiagnostic(
            diagnostic,
            "pending recovery entry returned Found without a payload");
        retain(entry, RecoveryAttemptKind::IntegrityConflict, diagnostic);
        continue;
      }
      break;
    default: {
      std::string diagnostic = entry.diagnostic;
      appendDiagnostic(diagnostic, "unknown pending read status");
      retain(entry, RecoveryAttemptKind::IntegrityConflict, diagnostic);
      continue;
    }
    }

    const PendingChartScoreWrite &pending = *entry.value;
    if (pending.attemptId != entry.attemptId) {
      retain(entry, RecoveryAttemptKind::IntegrityConflict,
             "pending recovery entry identity does not match its payload");
      continue;
    }
    if (pending.replayId <= 0 || pending.createdAt.empty()) {
      retain(entry, RecoveryAttemptKind::IntegrityConflict,
             "pending recovery payload has invalid replay metadata");
      continue;
    }

    ProjectionOutcome projected = dependencies_.project(pending);
    switch (projected.status) {
    case ProjectionStatus::StorageFailure:
      retain(entry, RecoveryAttemptKind::StorageFailure, projected.diagnostic);
      continue;
    case ProjectionStatus::IntegrityConflict:
      retain(entry, RecoveryAttemptKind::IntegrityConflict,
             projected.diagnostic);
      continue;
    case ProjectionStatus::Inserted:
    case ProjectionStatus::AlreadyPresent:
      break;
    default:
      appendDiagnostic(projected.diagnostic, "unknown projection status");
      retain(entry, RecoveryAttemptKind::IntegrityConflict,
             projected.diagnostic);
      continue;
    }

    AcknowledgeOutcome acknowledged =
        dependencies_.acknowledgeAndActivate(pending.attemptId,
                                             pending.replayId);
    switch (acknowledged.status) {
    case AcknowledgeStatus::StorageFailure:
      retain(entry, RecoveryAttemptKind::StorageFailure,
             acknowledged.diagnostic);
      continue;
    case AcknowledgeStatus::IntegrityConflict:
      retain(entry, RecoveryAttemptKind::IntegrityConflict,
             acknowledged.diagnostic);
      continue;
    case AcknowledgeStatus::Acknowledged:
    case AcknowledgeStatus::AlreadyAcknowledged:
      break;
    default:
      appendDiagnostic(acknowledged.diagnostic,
                       "unknown acknowledgement status");
      retain(entry, RecoveryAttemptKind::IntegrityConflict,
             acknowledged.diagnostic);
      continue;
    }

    ++summary.saved;
  }

  if (summary.pending != 0 || summary.conflicts != 0) {
    summary.userMessage = recoveryUserMessage();
  }
  return summary;
}

} // namespace result_persistence
