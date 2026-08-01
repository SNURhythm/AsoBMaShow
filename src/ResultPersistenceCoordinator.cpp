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

std::string phaseDiagnostic(std::string_view phase,
                            std::string_view diagnostic,
                            std::string_view fallback) {
  std::string result(phase);
  result += ": ";
  result += diagnostic.empty() ? fallback : diagnostic;
  return result;
}

void appendPhaseDiagnostic(std::string &destination, std::string_view phase,
                           std::string_view diagnostic,
                           std::string_view fallback) {
  appendDiagnostic(destination,
                   phaseDiagnostic(phase, diagnostic, fallback));
}

bool isConflictState(SaveState state) noexcept {
  return state == SaveState::UnstagedConflict ||
         state == SaveState::PendingConflict;
}

std::string_view completionPhaseName(PendingScoreCompletionPhase phase) {
  switch (phase) {
  case PendingScoreCompletionPhase::Validation:
    return "pending score validation";
  case PendingScoreCompletionPhase::Projection:
    return "score projection";
  case PendingScoreCompletionPhase::Acknowledgement:
    return "acknowledgement";
  }
  return "pending score completion";
}

void ensureConflictDiagnostic(SaveState state, std::string &diagnostic) {
  if (isConflictState(state) && diagnostic.empty()) {
    diagnostic = "persistence conflict reported without details";
  }
}

SaveOutcome unstagedOutcome(SaveState state, std::string diagnostic) {
  ensureConflictDiagnostic(state, diagnostic);
  return {.state = state,
          .receipt = std::nullopt,
          .userMessage = std::string(saveStateUserMessage(state)),
          .diagnostic = std::move(diagnostic)};
}

SaveOutcome durableOutcome(SaveState state, const StageReceipt &receipt,
                           std::string diagnostic) {
  ensureConflictDiagnostic(state, diagnostic);
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

std::optional<SaveConflictDetails>
saveConflictDetails(const SaveOutcome &outcome, std::string_view attemptId) {
  SaveConflictDetails details;
  switch (outcome.state) {
  case SaveState::UnstagedConflict:
    details.state = "UnstagedConflict";
    break;
  case SaveState::PendingConflict:
    details.state = "PendingConflict";
    break;
  case SaveState::Saved:
  case SaveState::InvalidAttempt:
  case SaveState::Unstaged:
  case SaveState::PendingScore:
  case SaveState::PendingAcknowledgement:
    return std::nullopt;
  }

  details.reason = outcome.diagnostic.empty()
                       ? "No diagnostic was provided."
                       : outcome.diagnostic;
  if (!attemptId.empty()) {
    details.attemptId = attemptId;
  } else if (outcome.receipt.has_value()) {
    details.attemptId = outcome.receipt->attemptId;
  }
  if (outcome.receipt.has_value() && outcome.receipt->replayId > 0) {
    details.replayId = outcome.receipt->replayId;
  }
  return details;
}

PendingScoreCompletionOutcome completePendingChartScore(
    const PendingChartScoreWrite &pending,
    const PendingScoreCompletionDependencies &dependencies) {
  if (!pending.hasExactlyOneOwner() || pending.attemptId.empty() ||
      pending.createdAt.empty()) {
    return {.status = PendingScoreCompletionStatus::IntegrityConflict,
            .phase = PendingScoreCompletionPhase::Validation,
            .diagnostic = "pending score durable owner is invalid"};
  }
  const ProjectionOutcome projected = dependencies.project(pending);
  switch (projected.status) {
  case ProjectionStatus::StorageFailure:
    return {.status = PendingScoreCompletionStatus::PendingScore,
            .phase = PendingScoreCompletionPhase::Projection,
            .diagnostic = projected.diagnostic};
  case ProjectionStatus::IntegrityConflict:
    return {.status = PendingScoreCompletionStatus::IntegrityConflict,
            .phase = PendingScoreCompletionPhase::Projection,
            .diagnostic = projected.diagnostic.empty()
                              ? "integrity conflict reported without details"
                              : projected.diagnostic};
  case ProjectionStatus::Inserted:
  case ProjectionStatus::AlreadyPresent:
    break;
  default:
    return {.status = PendingScoreCompletionStatus::IntegrityConflict,
            .phase = PendingScoreCompletionPhase::Projection,
            .diagnostic = "unknown projection status"};
  }
  const AcknowledgeOutcome acknowledged =
      dependencies.acknowledge(pending.attemptId, pending.ownerId());
  switch (acknowledged.status) {
  case AcknowledgeStatus::Acknowledged:
  case AcknowledgeStatus::AlreadyAcknowledged:
    return {.status = PendingScoreCompletionStatus::Saved,
            .phase = PendingScoreCompletionPhase::Acknowledgement,
            .diagnostic = acknowledged.diagnostic};
  case AcknowledgeStatus::StorageFailure:
    return {.status = PendingScoreCompletionStatus::PendingAcknowledgement,
            .phase = PendingScoreCompletionPhase::Acknowledgement,
            .diagnostic = acknowledged.diagnostic};
  case AcknowledgeStatus::IntegrityConflict:
    return {.status = PendingScoreCompletionStatus::IntegrityConflict,
            .phase = PendingScoreCompletionPhase::Acknowledgement,
            .diagnostic = acknowledged.diagnostic.empty()
                              ? "integrity conflict reported without details"
                              : acknowledged.diagnostic};
  default:
    return {.status = PendingScoreCompletionStatus::IntegrityConflict,
            .phase = PendingScoreCompletionPhase::Acknowledgement,
            .diagnostic = "unknown acknowledgement status"};
  }
  return {.status = PendingScoreCompletionStatus::IntegrityConflict,
          .phase = PendingScoreCompletionPhase::Acknowledgement,
          .diagnostic = "pending score completion state is unknown"};
}

RecoverySummary recoverPendingChartScores(
    const PendingScoreRecoveryDependencies &dependencies,
    std::size_t limit) {
  const std::size_t effectiveLimit =
      std::min(limit, kPendingChartScoreRecoveryPageRows);
  PendingBatchOutcome batch = dependencies.listPending(effectiveLimit);
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
        dependencies.recordRecoveryAttempt(entry.attemptId, kind);
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
    const bool ownerAgrees = pending.hasExactlyOneOwner() &&
                             pending.replayId == 0 &&
                             pending.modernResultId > 0;
    if (!ownerAgrees || pending.createdAt.empty()) {
      retain(entry, RecoveryAttemptKind::IntegrityConflict,
             "pending recovery payload has invalid modern result metadata");
      continue;
    }
    const auto completed =
        completePendingChartScore(pending, dependencies.completion);
    switch (completed.status) {
    case PendingScoreCompletionStatus::Saved:
      ++summary.saved;
      break;
    case PendingScoreCompletionStatus::PendingScore:
    case PendingScoreCompletionStatus::PendingAcknowledgement:
      retain(entry, RecoveryAttemptKind::StorageFailure, completed.diagnostic);
      break;
    case PendingScoreCompletionStatus::IntegrityConflict:
      retain(entry, RecoveryAttemptKind::IntegrityConflict,
             completed.diagnostic);
      break;
    }
  }

  if (summary.pending != 0 || summary.conflicts != 0) {
    summary.userMessage = recoveryUserMessage();
  }
  return summary;
}

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
    return unstagedOutcome(
        SaveState::UnstagedConflict,
        phaseDiagnostic("staging", staged.diagnostic,
                        "integrity conflict reported without details"));
  case StageStatus::Staged:
  case StageStatus::AlreadyStaged:
    break;
  default:
    return unstagedOutcome(
        SaveState::UnstagedConflict,
        phaseDiagnostic("staging", staged.diagnostic,
                        "unknown staging status"));
  }
  if (!staged.diagnostic.empty()) {
    staged.diagnostic =
        phaseDiagnostic("staging", staged.diagnostic,
                        "successful staging reported an empty diagnostic");
  }
  if (!staged.receipt.has_value() ||
      staged.receipt->attemptId != attempt.attemptId ||
      staged.receipt->replayId <= 0 || staged.receipt->createdAt.empty() ||
      (staged.status == StageStatus::Staged && !staged.receipt->scorePending)) {
    std::string receiptDiagnostic = "inconsistent success metadata";
    if (!staged.receipt.has_value()) {
      receiptDiagnostic += ": receipt is missing";
    } else if (staged.receipt->attemptId != attempt.attemptId) {
      receiptDiagnostic += ": attempt ID expected=" + attempt.attemptId +
                           " actual=" + staged.receipt->attemptId;
    } else if (staged.receipt->replayId <= 0) {
      receiptDiagnostic +=
          ": replay ID must be positive actual=" +
          std::to_string(staged.receipt->replayId);
    } else if (staged.receipt->createdAt.empty()) {
      receiptDiagnostic += ": timestamp is empty";
    } else {
      receiptDiagnostic +=
          ": newly staged receipt reports score already confirmed";
    }
    appendPhaseDiagnostic(staged.diagnostic, "staging receipt validation",
                          receiptDiagnostic,
                          "inconsistent success metadata");
    return unstagedOutcome(SaveState::UnstagedConflict,
                           std::move(staged.diagnostic));
  }

  StageReceipt receipt = *staged.receipt;
  if (!receipt.scorePending) {
    return durableOutcome(SaveState::Saved, receipt,
                          std::move(staged.diagnostic));
  }

  PendingReadOutcome loaded = dependencies_.loadPending(attempt.attemptId);
  switch (loaded.status) {
  case PendingReadStatus::StorageFailure:
    appendDiagnostic(staged.diagnostic, loaded.diagnostic);
    return durableOutcome(SaveState::PendingScore, receipt,
                          std::move(staged.diagnostic));
  case PendingReadStatus::NotFound:
    appendPhaseDiagnostic(staged.diagnostic, "pending score read",
                          loaded.diagnostic,
                          "no pending score was found");
    return durableOutcome(SaveState::PendingConflict, receipt,
                          std::move(staged.diagnostic));
  case PendingReadStatus::IntegrityConflict:
    appendPhaseDiagnostic(staged.diagnostic, "pending score read",
                          loaded.diagnostic,
                          "integrity conflict reported without details");
    return durableOutcome(SaveState::PendingConflict, receipt,
                          std::move(staged.diagnostic));
  case PendingReadStatus::Found:
    if (!loaded.diagnostic.empty()) {
      appendPhaseDiagnostic(staged.diagnostic, "pending score read",
                            loaded.diagnostic,
                            "Found reported without details");
    }
    if (!loaded.value.has_value()) {
      appendPhaseDiagnostic(staged.diagnostic, "pending score read", {},
                            "Found without a payload");
      return durableOutcome(SaveState::PendingConflict, receipt,
                            std::move(staged.diagnostic));
    }
    break;
  default:
    appendPhaseDiagnostic(staged.diagnostic, "pending score read", {},
                          "unknown pending read status");
    return durableOutcome(SaveState::PendingConflict, receipt,
                          std::move(staged.diagnostic));
  }

  const PendingChartScoreWrite &pending = *loaded.value;
  if (pending.attemptId != attempt.attemptId) {
    appendPhaseDiagnostic(
        staged.diagnostic, "pending score validation",
        "attempt ID expected=" + attempt.attemptId +
            " actual=" + pending.attemptId,
        "attempt ID differs");
    return durableOutcome(SaveState::PendingConflict, receipt,
                          std::move(staged.diagnostic));
  }
  if (pending.replayId != receipt.replayId) {
    appendPhaseDiagnostic(
        staged.diagnostic, "pending score validation",
        "replay ID expected=" + std::to_string(receipt.replayId) +
            " actual=" + std::to_string(pending.replayId),
        "replay ID differs");
    return durableOutcome(SaveState::PendingConflict, receipt,
                          std::move(staged.diagnostic));
  }
  if (pending.createdAt != receipt.createdAt) {
    appendPhaseDiagnostic(
        staged.diagnostic, "pending score validation",
        "timestamp expected=" + receipt.createdAt +
            " actual=" + pending.createdAt,
        "timestamp differs");
    return durableOutcome(SaveState::PendingConflict, receipt,
                          std::move(staged.diagnostic));
  }
  if (!(pending.score == attempt.score)) {
    appendPhaseDiagnostic(
        staged.diagnostic, "pending score validation",
        describeChartScoreDifference(attempt.score, pending.score),
        "score payload differs without an identifiable field");
    return durableOutcome(SaveState::PendingConflict, receipt,
                          std::move(staged.diagnostic));
  }

  const auto completed = completePendingChartScore(
      pending, {.project = dependencies_.project,
                .acknowledge = dependencies_.acknowledgeAndActivate});
  if (completed.status == PendingScoreCompletionStatus::IntegrityConflict) {
    appendPhaseDiagnostic(
        staged.diagnostic, completionPhaseName(completed.phase),
        completed.diagnostic, "integrity conflict reported without details");
  } else {
    appendDiagnostic(staged.diagnostic, completed.diagnostic);
  }
  switch (completed.status) {
  case PendingScoreCompletionStatus::PendingScore:
    return durableOutcome(SaveState::PendingScore, receipt,
                          std::move(staged.diagnostic));
  case PendingScoreCompletionStatus::PendingAcknowledgement:
    return durableOutcome(SaveState::PendingAcknowledgement, receipt,
                          std::move(staged.diagnostic));
  case PendingScoreCompletionStatus::IntegrityConflict:
    return durableOutcome(SaveState::PendingConflict, receipt,
                          std::move(staged.diagnostic));
  case PendingScoreCompletionStatus::Saved:
    break;
  }

  receipt.scorePending = false;
  return durableOutcome(SaveState::Saved, receipt,
                        std::move(staged.diagnostic));
}

} // namespace result_persistence
