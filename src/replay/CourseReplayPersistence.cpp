#include "CourseReplayPersistence.h"

#include "CourseReplayAgreement.h"
#include "ReplayFileStore.h"

#include "../ProfileDatabaseActivity.h"

#include <memory>
#include <ranges>
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
                       std::span<const std::byte> bytes,
                       const ReplayInstallOwnershipJournal &journal) {
                 return store->install(reservation, bytes, journal);
               },
           .recordInstallIntent =
               [&repository](const auto &reservation, const auto &receipt) {
                 return repository.RecordModernReplayInstallIntent(
                     reservation, receipt);
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
  const auto pathAgreement =
      compareCourseReplayPathToResult(attempt.pathInput, attempt.result);
  bool pathAgrees = pathAgreement.agrees();
  if (pathAgrees) {
    const auto expectedStem = courseStem(attempt.pathInput, pathDiagnostic);
    pathAgrees = expectedStem.has_value();
    if (expectedStem) {
      stem = *expectedStem;
    }
  } else {
    pathDiagnostic = pathAgreement.diagnostic;
  }
  if (attachment && !pathAgrees) {
    return {.state = CourseReplayPersistenceState::IntegrityConflict,
            .diagnostic = std::move(pathDiagnostic)};
  }

  ReplayFileAssociationCoordinator fileCoordinator(
      dependencies_.fileAssociation);
  std::optional<ReplayFileAssociation> fileAssociation;
  if (!attachment && attempt.replay) {
    std::string replayDiagnostic;
    const auto replayAgreement = compareCourseReplayToResult(
        *attempt.replay, attempt.result);
    const bool undefinedLongNotes = std::ranges::any_of(
        attempt.replay->playback.stages, [](const auto &stage) {
          return stage.setup.hasUndefinedLongNotes;
        });
    if (!replayAgreement.agrees()) {
      replayDiagnostic = replayAgreement.diagnostic;
    } else if (undefinedLongNotes != attempt.pathInput.hasUndefinedLongNotes) {
      replayDiagnostic =
          "course replay undefined-LN path fact differs from setup";
    }
    if (!pathAgrees || !replayAgreement.agrees() ||
        undefinedLongNotes != attempt.pathInput.hasUndefinedLongNotes) {
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
