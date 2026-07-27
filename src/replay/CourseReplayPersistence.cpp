#include "CourseReplayPersistence.h"

#include "ReplayFileStore.h"
#include "ReplaySetupProvenance.h"

#include "../ProfileDatabaseActivity.h"

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

CourseReplayPersistenceState savedState(bool replayAttached) noexcept {
  return replayAttached ? CourseReplayPersistenceState::SavedWithReplay
                        : CourseReplayPersistenceState::SavedWithoutReplay;
}

bool pathAgreesWithResult(const CoursePathInput &path,
                          const result_persistence::ModernCourseResult &result,
                          std::string &stem, std::string &diagnostic) {
  if (path.longNoteMode != result.longNoteMode ||
      path.stageSha256.size() != result.stages.size()) {
    diagnostic = "course replay path shape differs from its result";
    return false;
  }
  for (std::size_t index = 0; index < result.stages.size(); ++index) {
    if (path.stageSha256[index] != result.stages[index].score.chartSha256) {
      diagnostic = "course replay path stage identity differs from its result";
      return false;
    }
  }
  auto expected = courseStem(path, diagnostic);
  if (!expected) {
    return false;
  }
  stem = std::move(*expected);
  return true;
}

bool replayAgreesWithResult(
    const ReplayCourseDocument &replay, const CoursePathInput &path,
    const result_persistence::ModernCourseResult &result,
    std::string &diagnostic) {
  if (replay.playback.stages.size() != result.stages.size()) {
    diagnostic = "course replay completed prefix differs from its result";
    return false;
  }
  std::vector<ReplaySetupSource> sources(
      replay.playback.stages.size(), ReplaySetupSource::LocalCapture);
  if (!validateCourseReplayPlayback(replay.playback, sources, replay.timeBounds)
           .valid()) {
    diagnostic = "course replay playback envelope is invalid";
    return false;
  }

  bool hasUndefinedLongNotes = false;
  for (std::size_t index = 0; index < result.stages.size(); ++index) {
    const auto &setup = replay.playback.stages[index].setup;
    const auto &stage = result.stages[index];
    const ReplayChartIdentity expected{.md5 = stage.score.chartMd5,
                                       .sha256 = stage.score.chartSha256,
                                       .keyMode = stage.keyMode};
    if (compareReplayChartIdentity(setup.chart, expected) !=
            ReplayChartMatch::Match ||
        setup.longNoteMode != stage.score.longNoteMode ||
        setup.longNoteMode != result.longNoteMode ||
        setup.player1.option != result.requestedPlayOption ||
        setup.assistOption != assist_options::normalize(result.assistOption) ||
        setup.initialGaugeType != result.initialGaugeType ||
        setup.gaugeProfile != result.gaugeProfile ||
        setup.gaugeAutoShift != result.gaugeAutoShift ||
        setup.gaugeAutoShiftLowerBound !=
            result.gaugeAutoShiftLowerBound ||
        !replaySetupAgreesWithProvenance(setup, stage.score.provenance)) {
      diagnostic = "course replay stage setup differs from its result";
      return false;
    }
    hasUndefinedLongNotes =
        hasUndefinedLongNotes || setup.hasUndefinedLongNotes;
  }
  if (hasUndefinedLongNotes != path.hasUndefinedLongNotes) {
    diagnostic = "course replay undefined-LN path fact differs from setup";
    return false;
  }
  return true;
}

} // namespace

CourseReplayPersistence::CourseReplayPersistence(
    ReplayRepository &repository) {
  auto store = std::make_shared<ReplayFileStore>(
      repository.GetResolvedProfileRoot());
  auto codec = std::make_shared<BeatorajaReplayCodec>();
  dependencies_ = {
      .loadResult =
          [&repository](std::string_view attemptId) {
            return repository.LoadModernCourseResultByAttempt(attemptId);
          },
      .encode =
          [codec](const ReplayCourseDocument &replay, std::int64_t playedAt,
                  std::string &diagnostic) {
            return codec->encodeCourse(replay, playedAt, diagnostic);
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
      .stage =
          [&repository](const auto &result, const auto &file,
                        const auto &path) {
            return repository.StageModernCourseResult(result, file, path);
          },
  };
}

CourseReplayPersistence::CourseReplayPersistence(
    CourseReplayPersistenceDependencies dependencies)
    : dependencies_(std::move(dependencies)) {}

CourseReplayPersistenceOutcome CourseReplayPersistence::persist(
    const CapturedCourseReplayAttempt &attempt) {
  profile_database_activity::WriteGuard writeGuard;
  std::string diagnostic;
  if (attempt.result.resultId != 0 ||
      !result_persistence::validateModernCourseResult(attempt.result,
                                                      diagnostic)) {
    return {.state = CourseReplayPersistenceState::InvalidAttempt,
            .diagnostic = diagnostic.empty()
                              ? "modern course result is invalid"
                              : std::move(diagnostic)};
  }

  std::optional<ModernReplayFileAttachment> attachment;
  bool suppressNewReplay = false;
  const auto existing = dependencies_.loadResult(attempt.result.attemptId);
  switch (existing.status) {
  case ModernCourseResultReadStatus::Loaded: {
    if (!existing.record) {
      return {.state = CourseReplayPersistenceState::IntegrityConflict,
              .diagnostic = "loaded modern course result has no payload"};
    }
    auto expected = attempt.result;
    expected.resultId = existing.record->result.resultId;
    if (existing.record->result != expected) {
      return {.state = CourseReplayPersistenceState::IntegrityConflict,
              .diagnostic =
                  "attempt ID already names a different modern course result"};
    }
    suppressNewReplay = !existing.record->replayFile.has_value();
    if (existing.record->replayFile) {
      attachment = ModernReplayFileAttachment{
          .identity = existing.record->replayFile->identity,
          .metadata = existing.record->replayFile->metadata};
    }
    break;
  }
  case ModernCourseResultReadStatus::NotFound:
    break;
  case ModernCourseResultReadStatus::Invalid:
  case ModernCourseResultReadStatus::IntegrityConflict:
    return {.state = CourseReplayPersistenceState::IntegrityConflict,
            .diagnostic = existing.diagnostic.empty()
                              ? "existing modern course result is inconsistent"
                              : existing.diagnostic};
  case ModernCourseResultReadStatus::StorageFailure:
    return {.state = CourseReplayPersistenceState::Retryable,
            .diagnostic = existing.diagnostic.empty()
                              ? "could not inspect modern course result retry"
                              : existing.diagnostic};
  }

  std::string stem;
  std::string pathDiagnostic;
  const bool pathAgrees =
      pathAgreesWithResult(attempt.pathInput, attempt.result, stem,
                           pathDiagnostic);
  if (attachment && !pathAgrees) {
    return {.state = CourseReplayPersistenceState::IntegrityConflict,
            .diagnostic = std::move(pathDiagnostic)};
  }

  ReplayFileAssociationCoordinator fileCoordinator(
      dependencies_.fileAssociation);
  std::optional<ReplayFileAssociation> fileAssociation;
  if (!attachment && !suppressNewReplay && attempt.replay) {
    std::string replayDiagnostic;
    if (!pathAgrees ||
        !replayAgreesWithResult(*attempt.replay, attempt.pathInput,
                                attempt.result, replayDiagnostic)) {
      appendDiagnostic(diagnostic, "replay omitted",
                       !pathAgrees ? pathDiagnostic : replayDiagnostic);
    } else {
      const auto associated = fileCoordinator.associate(
          attempt.result.attemptId, stem, attempt.result.playedAtUnixMillis,
          [this, &attempt](std::string &encodeDiagnostic) {
            return dependencies_.encode(
                *attempt.replay, attempt.result.playedAtUnixMillis,
                encodeDiagnostic);
          });
      appendDiagnostic(diagnostic, "file association",
                       associated.diagnostic);
      if (associated.status ==
          ReplayFileAssociationStatus::IntegrityConflict) {
        return {.state = CourseReplayPersistenceState::IntegrityConflict,
                .diagnostic = std::move(diagnostic)};
      }
      if (associated.status == ReplayFileAssociationStatus::Attached &&
          associated.association) {
        fileAssociation = *associated.association;
        attachment = fileAssociation->attachment;
      }
    }
  }

  const auto staged = dependencies_.stage(
      attempt.result, attachment,
      attachment ? std::optional<CoursePathInput>(attempt.pathInput)
                 : std::nullopt);
  switch (staged.status) {
  case ModernCourseStageStatus::Invalid:
    if (fileAssociation) {
      fileCoordinator.abandonDefinitively(*fileAssociation, diagnostic);
    }
    appendDiagnostic(diagnostic, "staging", staged.diagnostic);
    return {.state = CourseReplayPersistenceState::InvalidAttempt,
            .diagnostic = std::move(diagnostic)};
  case ModernCourseStageStatus::IntegrityConflict:
    appendDiagnostic(diagnostic, "staging", staged.diagnostic);
    return {.state = CourseReplayPersistenceState::IntegrityConflict,
            .diagnostic = std::move(diagnostic)};
  case ModernCourseStageStatus::StorageFailure:
    appendDiagnostic(diagnostic, "staging", staged.diagnostic);
    return {.state = CourseReplayPersistenceState::Retryable,
            .diagnostic = std::move(diagnostic)};
  case ModernCourseStageStatus::Staged:
  case ModernCourseStageStatus::AlreadyStaged:
    break;
  }
  if (!staged.receipt ||
      staged.receipt->attemptId != attempt.result.attemptId ||
      staged.receipt->resultId <= 0 || staged.receipt->createdAt.empty()) {
    appendDiagnostic(diagnostic, "staging",
                     "success receipt is inconsistent");
    return {.state = CourseReplayPersistenceState::IntegrityConflict,
            .replayAttached = attachment.has_value(),
            .diagnostic = std::move(diagnostic)};
  }
  return {.state = savedState(attachment.has_value()),
          .receipt = staged.receipt,
          .replayAttached = attachment.has_value(),
          .diagnostic = std::move(diagnostic)};
}

} // namespace replay
