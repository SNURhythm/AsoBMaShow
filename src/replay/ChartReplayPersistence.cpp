#include "ChartReplayPersistence.h"

#include "BeatorajaReplayPath.h"

#include "../ProfileDatabaseActivity.h"
#include "../ResultPersistenceCoordinator.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <ranges>
#include <string>
#include <utility>

namespace replay {
namespace {

constexpr std::size_t kMaximumOccupiedSlotRetries = 64;

enum class InstalledOwnership {
  None,
  CreatedByAttempt,
  PreexistingIdentical,
  Ambiguous,
};

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

ChartReplayAgreement disagreement(ChartReplayAgreementIssue issue,
                                  std::string diagnostic) {
  return {.issue = issue, .diagnostic = std::move(diagnostic)};
}

bool samePlayerOption(const ReplayPlayerOption &replay,
                      const PlayerOptionProvenance &result) noexcept {
  return replay.option == result.option && replay.seed == result.seed;
}

bool sameStartingGauge(const ReplaySetup &setup,
                       const ScoreProvenance &provenance) noexcept {
  if (!provenance.startingGaugePercent.has_value()) {
    return true;
  }
  return setup.startingGaugePercent ==
         static_cast<float>(*provenance.startingGaugePercent);
}

bool sharedSetupAgrees(const ReplaySetup &setup,
                       const result_persistence::ModernChartResult &result) {
  const auto &provenance = result.score.provenance;
  if (provenance.stages.size() != 1) {
    return false;
  }
  const auto &stage = provenance.stages.front();
  return setup.chartRandomSeed == stage.chartRandomSeed &&
         setup.chartRandomPrng == stage.chartRandomPrng &&
         setup.chartRandomValues == stage.chartRandomValues &&
         samePlayerOption(setup.player1, provenance.player1) &&
         samePlayerOption(setup.player2, provenance.player2) &&
         setup.assistOption == provenance.assistOption &&
         setup.initialGaugeType == provenance.gaugeType &&
         setup.gaugeProfile == provenance.gaugeProfile &&
         setup.gaugeAutoShift == provenance.gaugeAutoShift &&
         setup.gaugeAutoShiftLowerBound ==
             provenance.gaugeAutoShiftLowerBound &&
         setup.ruleset == provenance.ruleset &&
         setup.playback == provenance.playback &&
         setup.candidateSelection == stage.candidateSelection &&
         setup.judgeWindowScalePercent == provenance.judgeWindowScalePercent &&
         setup.clubMode == provenance.clubMode &&
         sameStartingGauge(setup, provenance);
}

ChartReplayPersistenceState savedState(bool replayAttached) noexcept {
  return replayAttached ? ChartReplayPersistenceState::SavedWithReplay
                        : ChartReplayPersistenceState::SavedWithoutReplay;
}

bool releaseReservation(const ChartReplayPersistenceDependencies &dependencies,
                        const ModernReplayPathReservation &reservation,
                        std::string &diagnostic) {
  const auto released = dependencies.releasePath(reservation);
  if (released.status == ModernReplayReservationReleaseStatus::Released ||
      released.status == ModernReplayReservationReleaseStatus::NotFound) {
    return true;
  }
  appendDiagnostic(diagnostic, "reservation release", released.diagnostic);
  return false;
}

} // namespace

ChartReplayAgreement compareChartReplayToResult(
    const ReplayChartDocument &replay,
    const result_persistence::ModernChartResult &result) noexcept {
  try {
    std::string diagnostic;
    if (!result_persistence::validateModernChartResult(result, diagnostic)) {
      return disagreement(ChartReplayAgreementIssue::Result,
                          diagnostic.empty() ? "modern result is invalid"
                                             : std::move(diagnostic));
    }
    const auto playback = validateReplayPlayback(
        replay.playback, ReplaySetupSource::LocalCapture, replay.timeBounds);
    if (!playback.valid()) {
      return disagreement(ChartReplayAgreementIssue::Replay,
                          "captured replay playback is invalid");
    }
    const ReplayChartIdentity expected{.md5 = result.score.chartMd5,
                                       .sha256 = result.score.chartSha256,
                                       .keyMode = result.keyMode};
    if (compareReplayChartIdentity(replay.playback.setup.chart, expected) !=
        ReplayChartMatch::Match) {
      return disagreement(ChartReplayAgreementIssue::ChartIdentity,
                          "replay chart identity differs from the result");
    }
    if (replay.playback.setup.longNoteMode != result.score.longNoteMode) {
      return disagreement(ChartReplayAgreementIssue::LongNoteMode,
                          "replay long-note mode differs from the result");
    }
    if (!sharedSetupAgrees(replay.playback.setup, result)) {
      return disagreement(ChartReplayAgreementIssue::SharedSetup,
                          "replay setup differs from result provenance");
    }
    return {};
  } catch (...) {
    return disagreement(ChartReplayAgreementIssue::Replay,
                        "chart replay agreement validation failed");
  }
}

ChartReplayPersistence::ChartReplayPersistence(
    ScoreRepository &score, ReplayRepository &repository,
    std::filesystem::path profileRoot) {
  auto store = std::make_shared<ReplayFileStore>(std::move(profileRoot));
  auto codec = std::make_shared<BeatorajaReplayCodec>();
  dependencies_ = {
      .loadResult =
          [&repository](std::string_view attemptId) {
            return repository.LoadModernChartResultByAttempt(attemptId);
          },
      .reservePath =
          [&repository](std::string_view attemptId, std::string_view stem,
                        std::int64_t playedAt) {
            return repository.ReserveModernReplayPath(attemptId, stem,
                                                      playedAt);
          },
      .releasePath =
          [&repository](const auto &reservation) {
            return repository.ReleaseModernReplayPathReservation(reservation);
          },
      .encode =
          [codec](const ReplayChartDocument &replay, std::int64_t playedAt,
                  std::string &diagnostic) {
            return codec->encodeChart(replay, playedAt, diagnostic);
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
          [store](const ReplayFileMetadata &metadata, std::string &diagnostic) {
            return store->removeIfMatches(metadata, diagnostic);
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

  std::optional<ModernReplayPathReservation> pathReservation;
  std::optional<ReplayFileMetadata> installedMetadata;
  InstalledOwnership installedOwnership = InstalledOwnership::None;
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
        auto reserved = dependencies_.reservePath(
            attempt.result.attemptId, *stem, attempt.result.playedAtUnixMillis);
        if (reserved.reservation &&
            (reserved.status == ModernReplayReservationStatus::Reserved ||
             reserved.status ==
                 ModernReplayReservationStatus::AlreadyReserved)) {
          pathReservation = *reserved.reservation;
        } else if (reserved.status ==
                   ModernReplayReservationStatus::IntegrityConflict) {
          return {.state = ChartReplayPersistenceState::IntegrityConflict,
                  .diagnostic = reserved.diagnostic.empty()
                                    ? "replay reservation conflicts"
                                    : std::move(reserved.diagnostic)};
        } else {
          appendDiagnostic(diagnostic, "replay reservation",
                           reserved.diagnostic);
        }
      }
    }
  }

  std::optional<std::vector<std::byte>> encoded;
  if (pathReservation) {
    std::string encodeDiagnostic;
    encoded = dependencies_.encode(
        *attempt.replay, attempt.result.playedAtUnixMillis, encodeDiagnostic);
    if (!encoded) {
      appendDiagnostic(diagnostic, "replay omitted", encodeDiagnostic);
      releaseReservation(dependencies_, *pathReservation, diagnostic);
      pathReservation.reset();
    }
  }

  for (std::size_t occupied = 0; pathReservation && encoded && !attachment;
       ++occupied) {
    auto fileReservation = dependencies_.reserveFile(
        pathReservation->identity, *encoded, attempt.result.attemptId);
    if (!fileReservation.reservation) {
      appendDiagnostic(diagnostic, "replay omitted",
                       fileReservation.diagnostic);
      releaseReservation(dependencies_, *pathReservation, diagnostic);
      pathReservation.reset();
      break;
    }
    const auto installed =
        dependencies_.installFile(*fileReservation.reservation, *encoded);
    if (installed.state == ReplayInstallState::InstalledVerified &&
        installed.file) {
      installedMetadata = installed.file->metadata;
      installedOwnership = installed.existingIdenticalFile
                               ? InstalledOwnership::PreexistingIdentical
                               : InstalledOwnership::CreatedByAttempt;
      attachment =
          ModernReplayFileAttachment{.identity = pathReservation->identity,
                                     .metadata = installed.file->metadata};
      break;
    }
    if (installed.state == ReplayInstallState::Occupied &&
        occupied < kMaximumOccupiedSlotRetries) {
      if (!releaseReservation(dependencies_, *pathReservation, diagnostic)) {
        break;
      }
      auto next = dependencies_.reservePath(attempt.result.attemptId,
                                            pathReservation->identity.stem,
                                            attempt.result.playedAtUnixMillis);
      if (!next.reservation ||
          (next.status != ModernReplayReservationStatus::Reserved &&
           next.status != ModernReplayReservationStatus::AlreadyReserved)) {
        appendDiagnostic(diagnostic, "replay reservation", next.diagnostic);
        pathReservation.reset();
        break;
      }
      pathReservation = *next.reservation;
      continue;
    }

    const auto inspection = dependencies_.inspectFile(
        fileReservation.reservation->expectedMetadata);
    if (inspection.state == ReplayFileState::Available) {
      installedMetadata = fileReservation.reservation->expectedMetadata;
      installedOwnership = InstalledOwnership::Ambiguous;
      attachment = ModernReplayFileAttachment{
          .identity = pathReservation->identity,
          .metadata = fileReservation.reservation->expectedMetadata};
      break;
    }
    appendDiagnostic(diagnostic, "replay omitted",
                     installed.diagnostic.empty() ? inspection.diagnostic
                                                  : installed.diagnostic);
    if (inspection.state == ReplayFileState::Missing) {
      releaseReservation(dependencies_, *pathReservation, diagnostic);
      pathReservation.reset();
    }
    break;
  }

  const auto staged = dependencies_.stage(attempt.result, attempt.irSnapshot,
                                          attachment, irDrafts);
  switch (staged.status) {
  case ModernChartStageStatus::Invalid:
    if (installedMetadata && pathReservation &&
        installedOwnership == InstalledOwnership::CreatedByAttempt) {
      std::string cleanupDiagnostic;
      if (dependencies_.removeIfMatches(*installedMetadata,
                                        cleanupDiagnostic)) {
        releaseReservation(dependencies_, *pathReservation, diagnostic);
      } else {
        appendDiagnostic(diagnostic, "replay cleanup", cleanupDiagnostic);
      }
    } else if (pathReservation &&
               installedOwnership == InstalledOwnership::PreexistingIdentical) {
      releaseReservation(dependencies_, *pathReservation, diagnostic);
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
      result_persistence::PendingScoreOwnerKind::ModernResult,
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
