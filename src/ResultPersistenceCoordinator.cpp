#include "ResultPersistenceCoordinator.h"

#include "CourseConstraintUtils.h"
#include "FileChecksum.h"
#include "ProfileDatabaseActivity.h"
#include "replay/ReplaySetupAuthority.h"

#include <algorithm>
#include <set>
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
constexpr std::string_view kUnfinalizedReplayMessage =
    "The replay file could not be finished. Retry before leaving to avoid "
    "losing this play.";
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

replay::ReplayFileMetadata
ownershipMarkerFor(const replay::ReplayPathIdentity &identity,
                   std::span<const std::byte> encoded) {
  file_checksum::Sha256 hash;
  hash.update(encoded);
  return {
      .relativePath = identity.relativePath,
      .sha256 = hash.finalHex(),
      .compressedSize = static_cast<std::uint64_t>(encoded.size()),
      .codecVersion = replay::BeatorajaReplayCodec::kCodecVersion,
  };
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

bool validateCompletedAttempt(const CompletedChartAttempt &attempt,
                              std::span<const ir::IrOutboxDraft> drafts,
                              std::string &diagnostic) {
  diagnostic.clear();
  if (attempt.result.resultId != 0 ||
      !attempt.result.attemptId.has_value() ||
      !validatePersistedChartResult(attempt.result, diagnostic) ||
      attempt.result.resultFingerprint.empty()) {
    if (diagnostic.empty()) {
      diagnostic = "completed result identity is invalid";
    }
    return false;
  }
  const auto expectedSnapshot =
      ir::captureIrSubmissionSnapshot(attempt.result, diagnostic);
  if (!expectedSnapshot.has_value() ||
      *expectedSnapshot != attempt.irSnapshot) {
    if (diagnostic.empty()) {
      diagnostic = "IR snapshot differs from the completed result";
    }
    return false;
  }
  const auto setup = replay::setup_authority::resolveForResult(
      attempt.replay.setup, attempt.result.score, attempt.result.keyMode,
      replay::setup_authority::Source::CapturedAttempt, false);
  if (!setup.resolved()) {
    diagnostic = setup.diagnostic.empty()
                     ? "replay setup differs from the completed result"
                     : setup.diagnostic;
    return false;
  }
  std::set<std::string> providers;
  for (const ir::IrOutboxDraft &draft : drafts) {
    if (!ir::validateIrOutboxDraft(draft, diagnostic) ||
        !providers.insert(draft.providerId).second ||
        draft.attemptId != *attempt.result.attemptId ||
        draft.chartMd5 != attempt.irSnapshot.submission.chartMd5 ||
        draft.chartSha256 != attempt.irSnapshot.submission.chartSha256) {
      if (diagnostic.empty()) {
        diagnostic = "IR draft differs from the completed result snapshot";
      }
      return false;
    }
  }
  return true;
}

bool validateCompletedCourseAttempt(const CompletedCourseAttempt &attempt,
                                    std::string &diagnostic) {
  diagnostic.clear();
  const auto &result = attempt.result;
  if (result.resultId != 0 || !result.attemptId.has_value() ||
      !validatePersistedCourseResult(result, diagnostic) ||
      result.resultFingerprint.empty() || attempt.replay.stages.empty() ||
      attempt.replay.stages.size() != result.stages.size() ||
      attempt.replay.restMicrosAfterStage.size() != result.stages.size()) {
    if (diagnostic.empty()) {
      diagnostic = "completed course result/replay envelope is invalid";
    }
    return false;
  }
  for (std::size_t index = 0; index < result.stages.size(); ++index) {
    const auto &stage = result.stages[index];
    if (stage.stageIndex != static_cast<int>(index) ||
        attempt.replay.restMicrosAfterStage[index] < 0) {
      diagnostic = "course replay stage differs from its compact result";
      return false;
    }
    const auto setup = replay::setup_authority::resolveForResult(
        attempt.replay.stages[index].setup, stage.score, stage.keyMode,
        replay::setup_authority::Source::CapturedAttempt, index > 0U);
    if (!setup.resolved()) {
      diagnostic = setup.diagnostic.empty()
                       ? "course replay stage differs from its compact result"
                       : setup.diagnostic;
      return false;
    }
  }
  return true;
}

} // namespace

std::string_view saveStateUserMessage(SaveState state) noexcept {
  switch (state) {
  case SaveState::Saved:
    return {};
  case SaveState::InvalidAttempt:
    return kInvalidAttemptMessage;
  case SaveState::UnfinalizedReplay:
    return kUnfinalizedReplayMessage;
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
  case SaveState::UnfinalizedReplay:
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
  case SaveState::UnfinalizedReplay:
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
    const CompletedChartAttempt &attempt) const noexcept {
  if (!durable() || !receipt.has_value() ||
      !attempt.result.attemptId.has_value() ||
      receipt->attemptId != *attempt.result.attemptId ||
      receipt->resultId <= 0 ||
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
  case SaveState::UnfinalizedReplay:
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
  if (outcome.receipt.has_value() && outcome.receipt->resultId > 0) {
    details.resultId = outcome.receipt->resultId;
  }
  return details;
}

Coordinator::Coordinator(ScoreRepository &score, ReplayRepository &replay)
    : Coordinator(Dependencies{
          .reserve =
              [&replay](std::string_view attemptId, std::string_view stem) {
                return replay.reserveReplayFile(attemptId, stem);
              },
          .encodeReplay =
              [](const replay::ReplayPlaybackData &playback,
                 std::int64_t playedAtUnixMillis, std::string &diagnostic) {
                return replay::BeatorajaReplayCodec{}.encodeChart(
                    playback, playedAtUnixMillis, diagnostic);
              },
          .encodeCourseReplay =
              [](const replay::CourseReplayPlaybackData &playback,
                 std::int64_t playedAtUnixMillis, std::string &diagnostic) {
                return replay::BeatorajaReplayCodec{}.encodeCourse(
                    playback, playedAtUnixMillis, diagnostic);
              },
          .finalizeReplay =
              [&replay](const replay::ReplayPathIdentity &identity,
                        std::span<const std::byte> encoded,
                        const replay::ExpectedReplayIdentity &expected,
                        std::string_view attemptToken) {
                replay::ReplayFileStore store(
                    replay.GetResolvedDatabasePath().parent_path());
                replay::BeatorajaReplayCodec codec;
                return store.finalize(identity, encoded, codec, expected,
                                      attemptToken);
              },
          .recordFinalizedReplay =
              [&replay](const ReplayFileReservation &reservation,
                        const replay::ReplayFileMetadata &metadata,
                        std::string &diagnostic) {
                return replay.markReplayFileReservationFinalized(
                    reservation, metadata, diagnostic);
              },
          .stage =
              [&replay](const PersistedChartResult &result,
                        const ir::IrSubmissionSnapshot &snapshot,
                        const ReplayFileReference &file,
                        std::span<const ir::IrOutboxDraft> irDrafts) {
                return replay.stageCompletedChartAttempt(result, snapshot,
                                                         file, irDrafts);
              },
          .stageCourse =
              [&replay](const PersistedCourseResult &result,
                        const ReplayFileReference &file) {
                return replay.stageCompletedCourseAttempt(result, file);
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
              [&replay](std::string_view attemptId, int resultId) {
                return replay.AcknowledgePendingChartScoreAndActivateIr(
                    attemptId, resultId);
              },
          .recordRecoveryAttempt =
              [&replay](std::string_view attemptId, RecoveryAttemptKind kind) {
                return replay.RecordPendingChartScoreRecoveryAttempt(attemptId,
                                                                     kind);
              },
      }) {}

Coordinator::Coordinator(ScoreRepository &score, ReplayRepository &replay,
                         replay::ReplayFileStore &fileStore,
                         replay::BeatorajaReplayCodec &codec)
    : Coordinator(Dependencies{
          .reserve =
              [&replay](std::string_view attemptId, std::string_view stem) {
                return replay.reserveReplayFile(attemptId, stem);
              },
          .encodeReplay =
              [&codec](const replay::ReplayPlaybackData &playback,
                       std::int64_t playedAtUnixMillis,
                       std::string &diagnostic) {
                return codec.encodeChart(playback, playedAtUnixMillis,
                                         diagnostic);
              },
          .encodeCourseReplay =
              [&codec](const replay::CourseReplayPlaybackData &playback,
                       std::int64_t playedAtUnixMillis,
                       std::string &diagnostic) {
                return codec.encodeCourse(playback, playedAtUnixMillis,
                                          diagnostic);
              },
          .finalizeReplay =
              [&fileStore, &codec](
                  const replay::ReplayPathIdentity &identity,
                  std::span<const std::byte> encoded,
                  const replay::ExpectedReplayIdentity &expected,
                  std::string_view attemptToken) {
                return fileStore.finalize(identity, encoded, codec, expected,
                                          attemptToken);
              },
          .recordFinalizedReplay =
              [&replay](const ReplayFileReservation &reservation,
                        const replay::ReplayFileMetadata &metadata,
                        std::string &diagnostic) {
                return replay.markReplayFileReservationFinalized(
                    reservation, metadata, diagnostic);
              },
          .stage =
              [&replay](const PersistedChartResult &result,
                        const ir::IrSubmissionSnapshot &snapshot,
                        const ReplayFileReference &file,
                        std::span<const ir::IrOutboxDraft> irDrafts) {
                return replay.stageCompletedChartAttempt(result, snapshot,
                                                         file, irDrafts);
              },
          .stageCourse =
              [&replay](const PersistedCourseResult &result,
                        const ReplayFileReference &file) {
                return replay.stageCompletedCourseAttempt(result, file);
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
              [&replay](std::string_view attemptId, int resultId) {
                return replay.AcknowledgePendingChartScoreAndActivateIr(
                    attemptId, resultId);
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
    const CompletedChartAttempt &attempt,
    std::span<const ir::IrOutboxDraft> irDrafts) {
  profile_database_activity::WriteGuard bindingLease;
  std::string diagnostic;
  if (!validateCompletedAttempt(attempt, irDrafts, diagnostic)) {
    return unstagedOutcome(SaveState::InvalidAttempt, std::move(diagnostic));
  }
  const std::string &attemptId = *attempt.result.attemptId;
  const auto stem = replay::chartStem(
      attempt.replay.setup.chartSha256, attempt.replay.setup.longNoteMode,
      attempt.replay.setup.hasUndefinedLongNotes, diagnostic);
  if (!stem.has_value()) {
    return unstagedOutcome(SaveState::InvalidAttempt, std::move(diagnostic));
  }
  if (!dependencies_.reserve || !dependencies_.encodeReplay ||
      !dependencies_.finalizeReplay || !dependencies_.recordFinalizedReplay ||
      !dependencies_.stage ||
      !dependencies_.loadPending || !dependencies_.project ||
      !dependencies_.acknowledgeAndActivate) {
    return unstagedOutcome(SaveState::UnstagedConflict,
                           "persistence dependencies are incomplete");
  }
  ReservationOutcome reserved = dependencies_.reserve(attemptId, *stem);
  switch (reserved.status) {
  case ReservationOutcome::Status::StorageFailure:
    return unstagedOutcome(SaveState::Unstaged,
                           std::move(reserved.diagnostic));
  case ReservationOutcome::Status::IntegrityConflict:
    return unstagedOutcome(
        SaveState::UnstagedConflict,
        phaseDiagnostic("replay reservation", reserved.diagnostic,
                        "integrity conflict reported without details"));
  case ReservationOutcome::Status::Invalid:
    return unstagedOutcome(SaveState::InvalidAttempt,
                           std::move(reserved.diagnostic));
  case ReservationOutcome::Status::Reserved:
  case ReservationOutcome::Status::AlreadyReserved:
    break;
  }
  if (!reserved.reservation.has_value() ||
      reserved.reservation->attemptId != attemptId ||
      reserved.reservation->stem != *stem) {
    return unstagedOutcome(SaveState::UnstagedConflict,
                           "replay reservation metadata is inconsistent");
  }
  auto encoded = dependencies_.encodeReplay(
      attempt.replay, attempt.result.playedAtUnixMillis, diagnostic);
  if (!encoded.has_value()) {
    return unstagedOutcome(SaveState::InvalidAttempt,
                           phaseDiagnostic("replay encoding", diagnostic,
                                           "replay is invalid"));
  }
  const replay::ReplayPathIdentity identity{
      .stem = reserved.reservation->stem,
      .historyIndex = reserved.reservation->historyIndex,
      .relativePath = reserved.reservation->relativePath,
  };
  const replay::ExpectedReplayIdentity expected{
      .stageSha256 = {attempt.replay.setup.chartSha256},
      .stageLongNoteModes = {attempt.replay.setup.longNoteMode},
      .course = false};
  const replay::ReplayFileMetadata ownership =
      ownershipMarkerFor(identity, *encoded);
  if (!dependencies_.recordFinalizedReplay(*reserved.reservation, ownership,
                                           diagnostic)) {
    return unstagedOutcome(
        SaveState::UnfinalizedReplay,
        phaseDiagnostic("replay ownership marker", diagnostic,
                        "ownership metadata could not be stored"));
  }
  replay::FinalizeOutcome finalized = dependencies_.finalizeReplay(
      identity, *encoded, expected, attemptId);
  if (!finalized.metadata.has_value()) {
    return unstagedOutcome(
        SaveState::UnfinalizedReplay,
        phaseDiagnostic("replay file finalization", finalized.diagnostic,
                        "file validation failed"));
  }
  if (*finalized.metadata != ownership) {
    return unstagedOutcome(SaveState::UnstagedConflict,
                           "finalized replay metadata differs from its "
                           "pre-install ownership marker");
  }
  const ReplayFileReference replayFile{
      .stem = identity.stem,
      .historyIndex = identity.historyIndex,
      .relativePath = finalized.metadata->relativePath,
      .contentSha256 = finalized.metadata->sha256,
      .compressedSize = finalized.metadata->compressedSize,
      .codecVersion = finalized.metadata->codecVersion,
  };
  StageOutcome staged = dependencies_.stage(
      attempt.result, attempt.irSnapshot, replayFile, irDrafts);
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
      staged.receipt->attemptId != attemptId ||
      staged.receipt->resultId <= 0 || staged.receipt->createdAt.empty() ||
      (staged.status == StageStatus::Staged && !staged.receipt->scorePending)) {
    std::string receiptDiagnostic = "inconsistent success metadata";
    if (!staged.receipt.has_value()) {
      receiptDiagnostic += ": receipt is missing";
    } else if (staged.receipt->attemptId != attemptId) {
      receiptDiagnostic += ": attempt ID expected=" + attemptId +
                           " actual=" + staged.receipt->attemptId;
    } else if (staged.receipt->resultId <= 0) {
      receiptDiagnostic +=
          ": result ID must be positive actual=" +
          std::to_string(staged.receipt->resultId);
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

  PendingReadOutcome loaded = dependencies_.loadPending(attemptId);
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
  if (pending.attemptId != attemptId) {
    appendPhaseDiagnostic(
        staged.diagnostic, "pending score validation",
        "attempt ID expected=" + attemptId +
            " actual=" + pending.attemptId,
        "attempt ID differs");
    return durableOutcome(SaveState::PendingConflict, receipt,
                          std::move(staged.diagnostic));
  }
  if (pending.resultId != receipt.resultId) {
    appendPhaseDiagnostic(
        staged.diagnostic, "pending score validation",
        "result ID expected=" + std::to_string(receipt.resultId) +
            " actual=" + std::to_string(pending.resultId),
        "result ID differs");
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
  if (!(pending.score == attempt.result.score)) {
    appendPhaseDiagnostic(
        staged.diagnostic, "pending score validation",
        describeChartScoreDifference(attempt.result.score, pending.score),
        "score payload differs without an identifiable field");
    return durableOutcome(SaveState::PendingConflict, receipt,
                          std::move(staged.diagnostic));
  }

  ProjectionOutcome projected = dependencies_.project(pending);
  switch (projected.status) {
  case ProjectionStatus::StorageFailure:
    appendDiagnostic(staged.diagnostic, projected.diagnostic);
    return durableOutcome(SaveState::PendingScore, receipt,
                          std::move(staged.diagnostic));
  case ProjectionStatus::IntegrityConflict:
    appendPhaseDiagnostic(staged.diagnostic, "score projection",
                          projected.diagnostic,
                          "integrity conflict reported without details");
    return durableOutcome(SaveState::PendingConflict, receipt,
                          std::move(staged.diagnostic));
  case ProjectionStatus::Inserted:
  case ProjectionStatus::AlreadyPresent:
    appendDiagnostic(staged.diagnostic, projected.diagnostic);
    break;
  default:
    appendPhaseDiagnostic(staged.diagnostic, "score projection", {},
                          "unknown projection status");
    return durableOutcome(SaveState::PendingConflict, receipt,
                          std::move(staged.diagnostic));
  }

  AcknowledgeOutcome acknowledged =
      dependencies_.acknowledgeAndActivate(pending.attemptId,
                                           pending.resultId);
  switch (acknowledged.status) {
  case AcknowledgeStatus::StorageFailure:
    appendDiagnostic(staged.diagnostic, acknowledged.diagnostic);
    return durableOutcome(SaveState::PendingAcknowledgement, receipt,
                          std::move(staged.diagnostic));
  case AcknowledgeStatus::IntegrityConflict:
    appendPhaseDiagnostic(staged.diagnostic, "acknowledgement",
                          acknowledged.diagnostic,
                          "integrity conflict reported without details");
    return durableOutcome(SaveState::PendingConflict, receipt,
                          std::move(staged.diagnostic));
  case AcknowledgeStatus::Acknowledged:
  case AcknowledgeStatus::AlreadyAcknowledged:
    appendDiagnostic(staged.diagnostic, acknowledged.diagnostic);
    break;
  default:
    appendPhaseDiagnostic(staged.diagnostic, "acknowledgement", {},
                          "unknown acknowledgement status");
    return durableOutcome(SaveState::PendingConflict, receipt,
                          std::move(staged.diagnostic));
  }

  receipt.scorePending = false;
  return durableOutcome(SaveState::Saved, receipt,
                        std::move(staged.diagnostic));
}

SaveOutcome Coordinator::persistCourse(const CompletedCourseAttempt &attempt) {
  profile_database_activity::WriteGuard bindingLease;
  std::string diagnostic;
  if (!validateCompletedCourseAttempt(attempt, diagnostic)) {
    return unstagedOutcome(SaveState::InvalidAttempt, std::move(diagnostic));
  }
  if (!dependencies_.reserve || !dependencies_.encodeCourseReplay ||
      !dependencies_.finalizeReplay || !dependencies_.recordFinalizedReplay ||
      !dependencies_.stageCourse) {
    return unstagedOutcome(SaveState::UnstagedConflict,
                           "course persistence dependencies are incomplete");
  }

  replay::CoursePathInput pathInput;
  pathInput.longNoteMode = attempt.result.longNoteMode;
  pathInput.beatorajaConstraintIds =
      beatorajaCourseConstraintIds(attempt.result.constraintJson);
  pathInput.stageSha256.reserve(attempt.replay.stages.size());
  std::vector<int> stageLongNoteModes;
  stageLongNoteModes.reserve(attempt.replay.stages.size());
  for (const auto &stage : attempt.replay.stages) {
    pathInput.stageSha256.push_back(stage.setup.chartSha256);
    stageLongNoteModes.push_back(stage.setup.longNoteMode);
    pathInput.hasUndefinedLongNotes =
        pathInput.hasUndefinedLongNotes || stage.setup.hasUndefinedLongNotes;
  }
  const auto stem = replay::courseStem(pathInput, diagnostic);
  if (!stem.has_value()) {
    return unstagedOutcome(SaveState::InvalidAttempt, std::move(diagnostic));
  }

  const std::string &attemptId = *attempt.result.attemptId;
  ReservationOutcome reserved = dependencies_.reserve(attemptId, *stem);
  switch (reserved.status) {
  case ReservationOutcome::Status::StorageFailure:
    return unstagedOutcome(SaveState::Unstaged,
                           std::move(reserved.diagnostic));
  case ReservationOutcome::Status::IntegrityConflict:
    return unstagedOutcome(SaveState::UnstagedConflict,
                           std::move(reserved.diagnostic));
  case ReservationOutcome::Status::Invalid:
    return unstagedOutcome(SaveState::InvalidAttempt,
                           std::move(reserved.diagnostic));
  case ReservationOutcome::Status::Reserved:
  case ReservationOutcome::Status::AlreadyReserved:
    break;
  }
  if (!reserved.reservation.has_value() ||
      reserved.reservation->attemptId != attemptId ||
      reserved.reservation->stem != *stem) {
    return unstagedOutcome(SaveState::UnstagedConflict,
                           "course replay reservation is inconsistent");
  }

  auto encoded = dependencies_.encodeCourseReplay(
      attempt.replay, attempt.result.playedAtUnixMillis, diagnostic);
  if (!encoded.has_value()) {
    return unstagedOutcome(
        SaveState::InvalidAttempt,
        phaseDiagnostic("course replay encoding", diagnostic,
                        "course replay is invalid"));
  }
  const replay::ReplayPathIdentity identity{
      .stem = reserved.reservation->stem,
      .historyIndex = reserved.reservation->historyIndex,
      .relativePath = reserved.reservation->relativePath,
  };
  const replay::ReplayFileMetadata ownership =
      ownershipMarkerFor(identity, *encoded);
  if (!dependencies_.recordFinalizedReplay(*reserved.reservation, ownership,
                                           diagnostic)) {
    return unstagedOutcome(
        SaveState::UnfinalizedReplay,
        phaseDiagnostic("course replay ownership marker", diagnostic,
                        "ownership metadata could not be stored"));
  }
  replay::FinalizeOutcome finalized = dependencies_.finalizeReplay(
      identity, *encoded,
      {.stageSha256 = pathInput.stageSha256,
       .stageLongNoteModes = std::move(stageLongNoteModes),
       .course = true},
      attemptId);
  if (!finalized.metadata.has_value()) {
    return unstagedOutcome(
        SaveState::UnfinalizedReplay,
        phaseDiagnostic("course replay file finalization",
                        finalized.diagnostic, "file validation failed"));
  }
  if (*finalized.metadata != ownership) {
    return unstagedOutcome(
        SaveState::UnstagedConflict,
        "finalized course replay metadata differs from its pre-install "
        "ownership marker");
  }
  const ReplayFileReference replayFile{
      .recordKind = ReplayFileReference::RecordKind::CourseResult,
      .stem = identity.stem,
      .historyIndex = identity.historyIndex,
      .relativePath = finalized.metadata->relativePath,
      .contentSha256 = finalized.metadata->sha256,
      .compressedSize = finalized.metadata->compressedSize,
      .codecVersion = finalized.metadata->codecVersion,
  };
  StageOutcome staged = dependencies_.stageCourse(attempt.result, replayFile);
  if (staged.status == StageStatus::StorageFailure) {
    return unstagedOutcome(SaveState::Unstaged,
                           std::move(staged.diagnostic));
  }
  if (staged.status == StageStatus::IntegrityConflict) {
    return unstagedOutcome(SaveState::UnstagedConflict,
                           std::move(staged.diagnostic));
  }
  if ((staged.status != StageStatus::Staged &&
       staged.status != StageStatus::AlreadyStaged) ||
      !staged.receipt.has_value() ||
      staged.receipt->attemptId != attemptId ||
      staged.receipt->resultId <= 0 || staged.receipt->createdAt.empty() ||
      staged.receipt->scorePending) {
    return unstagedOutcome(
        SaveState::UnstagedConflict,
        staged.diagnostic.empty() ? "course staging receipt is inconsistent"
                                  : std::move(staged.diagnostic));
  }
  return durableOutcome(SaveState::Saved, *staged.receipt,
                        std::move(staged.diagnostic));
}

RecoverySummary Coordinator::recoverAll(std::size_t limit) {
  profile_database_activity::WriteGuard bindingLease;
  if (!dependencies_.listPending || !dependencies_.project ||
      !dependencies_.acknowledgeAndActivate ||
      !dependencies_.recordRecoveryAttempt) {
    return recoveryFailureSummary(
        "recovery dependencies are incomplete");
  }
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
    if (pending.resultId <= 0 || pending.createdAt.empty()) {
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
                                             pending.resultId);
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
