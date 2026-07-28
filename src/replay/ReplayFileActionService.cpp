#include "ReplayFileActionService.h"

#include "BeatorajaReplayCodec.h"
#include "BeatorajaReplayPath.h"
#include "ReplayReferenceAgreement.h"

namespace replay {
namespace {

ReplayFileActionState actionState(ReplayFileState state) noexcept {
  switch (state) {
  case ReplayFileState::Available:
    return ReplayFileActionState::Verified;
  case ReplayFileState::Missing:
    return ReplayFileActionState::Missing;
  case ReplayFileState::Corrupt:
    return ReplayFileActionState::Corrupt;
  case ReplayFileState::Unsafe:
    return ReplayFileActionState::Mismatched;
  case ReplayFileState::IoFailure:
    return ReplayFileActionState::IoFailure;
  }
  return ReplayFileActionState::IoFailure;
}

} // namespace

ReplayState replayStateForFileAction(ReplayFileActionState state) noexcept {
  switch (state) {
  case ReplayFileActionState::Verified:
    return ReplayState::Verified;
  case ReplayFileActionState::UserDeleted:
    return ReplayState::UserDeleted;
  case ReplayFileActionState::Missing:
  case ReplayFileActionState::IoFailure:
    return ReplayState::Missing;
  case ReplayFileActionState::Corrupt:
    return ReplayState::Corrupt;
  case ReplayFileActionState::Mismatched:
    return ReplayState::Mismatched;
  case ReplayFileActionState::UnsupportedCodecVersion:
    return ReplayState::UnsupportedExtension;
  case ReplayFileActionState::ResultNotFound:
  case ReplayFileActionState::Invalid:
    return ReplayState::NotApplicable;
  }
  return ReplayState::NotApplicable;
}

ReplayFileActionService::ReplayFileActionService(ReplayRepository &repository)
    : ownedStore_(std::make_unique<ReplayFileStore>(
          repository.GetResolvedProfileRoot())),
      repository_(repository), store_(*ownedStore_) {}

ReplayFileActionService::ReplayFileActionService(ReplayRepository &repository,
                                                 ReplayFileStore &store)
    : repository_(repository), store_(store) {}

std::optional<ReplayFileActionService::ResolvedReference>
ReplayFileActionService::resolve(const ReplayFileActionRequest &request,
                                 ReplayFileActionOutcome &outcome) {
  std::optional<ModernReplayFileReference> reference;
  ReplayReferenceAgreement referenceAgreement;
  if (request.owner == ModernReplayOwnerKind::ChartResult) {
    const auto loaded =
        repository_.LoadModernChartResultByAttempt(request.attemptId);
    if (loaded.status == ModernChartResultReadStatus::NotFound) {
      outcome.state = ReplayFileActionState::ResultNotFound;
      outcome.diagnostic = loaded.diagnostic;
      return std::nullopt;
    }
    if (loaded.status != ModernChartResultReadStatus::Loaded ||
        !loaded.record) {
      outcome.state = loaded.status == ModernChartResultReadStatus::Invalid ||
                              loaded.status ==
                                  ModernChartResultReadStatus::IntegrityConflict
                          ? ReplayFileActionState::Invalid
                          : ReplayFileActionState::IoFailure;
      outcome.diagnostic = loaded.diagnostic;
      return std::nullopt;
    }
    reference = loaded.record->replayFile;
    if (reference) {
      referenceAgreement = compareChartReplayReferenceToResult(
          *reference, loaded.record->result);
    }
  } else {
    const auto loaded =
        repository_.LoadModernCourseResultByAttempt(request.attemptId);
    if (loaded.status == ModernCourseResultReadStatus::NotFound) {
      outcome.state = ReplayFileActionState::ResultNotFound;
      outcome.diagnostic = loaded.diagnostic;
      return std::nullopt;
    }
    if (loaded.status != ModernCourseResultReadStatus::Loaded ||
        !loaded.record) {
      outcome.state = loaded.status == ModernCourseResultReadStatus::Invalid ||
                              loaded.status == ModernCourseResultReadStatus::IntegrityConflict
                          ? ReplayFileActionState::Invalid
                          : ReplayFileActionState::IoFailure;
      outcome.diagnostic = loaded.diagnostic;
      return std::nullopt;
    }
    reference = loaded.record->replayFile;
    if (reference) {
      referenceAgreement = compareCourseReplayReferenceToResult(
          *reference, loaded.record->result);
    }
  }
  if (!reference) {
    outcome.state = ReplayFileActionState::Missing;
    outcome.diagnostic = "The result has no replay file reference.";
    return std::nullopt;
  }
  if (!referenceAgreement.matches) {
    outcome.state = ReplayFileActionState::Invalid;
    outcome.diagnostic = referenceAgreement.diagnostic.empty()
                             ? "The replay reference is inconsistent."
                             : std::move(referenceAgreement.diagnostic);
    return std::nullopt;
  }
  return ResolvedReference{.owner = request.owner,
                           .attemptId = request.attemptId,
                           .reference = std::move(*reference)};
}

ReplayFileActionOutcome ReplayFileActionService::inspectResolved(
    const ResolvedReference &resolved,
    std::optional<ReplayFileMetadata> *observedMetadata) const {
  if (observedMetadata != nullptr) {
    observedMetadata->reset();
  }
  if (resolved.reference.userDeleted) {
    if (observedMetadata != nullptr) {
      *observedMetadata = resolved.reference.metadata;
    }
    return {.state = ReplayFileActionState::UserDeleted};
  }
  if (resolved.reference.metadata.codecVersion !=
      BeatorajaReplayCodec::kCodecVersion) {
    if (observedMetadata != nullptr) {
      *observedMetadata =
          store_.inspect(resolved.reference.metadata).observedMetadata;
    }
    return {.state = ReplayFileActionState::UnsupportedCodecVersion,
            .diagnostic =
                "The replay uses an unsupported codec version."};
  }
  const auto inspected = store_.inspect(resolved.reference.metadata);
  if (observedMetadata != nullptr) {
    *observedMetadata = inspected.observedMetadata;
  }
  return {.state = actionState(inspected.state),
          .diagnostic = inspected.diagnostic};
}

ReplayFileActionOutcome
ReplayFileActionService::inspect(const ReplayFileActionRequest &request) {
  ReplayFileActionOutcome outcome;
  const auto resolved = resolve(request, outcome);
  return resolved ? inspectResolved(*resolved) : outcome;
}

ReplayFileActionOutcome ReplayFileActionService::prepareShare(
    const ReplayFileActionRequest &request) {
  ReplayFileActionOutcome outcome;
  const auto resolved = resolve(request, outcome);
  if (!resolved) {
    return outcome;
  }
  outcome = inspectResolved(*resolved);
  if (outcome.state != ReplayFileActionState::Verified) {
    return outcome;
  }
  auto snapshot = store_.stageVerifiedSnapshot(resolved->reference.metadata);
  outcome.state = actionState(snapshot.state);
  outcome.diagnostic = snapshot.diagnostic;
  if (snapshot.state == ReplayFileState::Available && snapshot.snapshot) {
    outcome.share = ReplaySharePreparation{
        .sourcePath = snapshot.snapshot->sourcePath,
        .sourceLifetime = snapshot.snapshot->sourceLifetime,
        .suggestedFilename =
            resolved->reference.metadata.relativePath.filename().string()};
  }
  return outcome;
}

ReplayFileActionOutcome
ReplayFileActionService::remove(const ReplayFileActionRequest &request) {
  ReplayFileActionOutcome outcome;
  const auto resolved = resolve(request, outcome);
  if (!resolved) {
    return outcome;
  }
  std::optional<ReplayFileMetadata> observedMetadata;
  const auto inspected = inspectResolved(*resolved, &observedMetadata);
  if (inspected.state != ReplayFileActionState::Verified &&
      inspected.state != ReplayFileActionState::Corrupt &&
      inspected.state != ReplayFileActionState::Mismatched &&
      inspected.state != ReplayFileActionState::UnsupportedCodecVersion &&
      inspected.state != ReplayFileActionState::UserDeleted) {
    return inspected;
  }
  const auto mutation = repository_.MarkModernReplayFileUserDeleted(
      resolved->owner, resolved->attemptId, resolved->reference);
  switch (mutation.status) {
  case ModernReplayFileMutationStatus::Changed:
    outcome.changed = true;
    break;
  case ModernReplayFileMutationStatus::AlreadyChanged:
    break;
  case ModernReplayFileMutationStatus::NotFound:
    outcome.state = ReplayFileActionState::ResultNotFound;
    outcome.diagnostic = mutation.diagnostic;
    return outcome;
  case ModernReplayFileMutationStatus::Invalid:
  case ModernReplayFileMutationStatus::IntegrityConflict:
    outcome.state = ReplayFileActionState::Invalid;
    outcome.diagnostic = mutation.diagnostic;
    return outcome;
  case ModernReplayFileMutationStatus::StorageFailure:
    outcome.state = ReplayFileActionState::IoFailure;
    outcome.diagnostic = mutation.diagnostic;
    return outcome;
  }

  std::string cleanupDiagnostic;
  const bool removed =
      observedMetadata.has_value() &&
      store_.removeIfMatches(*observedMetadata, cleanupDiagnostic);
  if (!observedMetadata && cleanupDiagnostic.empty()) {
    cleanupDiagnostic =
        "Replay cleanup is pending because the selected occupant could not "
        "be proven.";
  }
  outcome.state = ReplayFileActionState::UserDeleted;
  outcome.cleanupPending = !removed;
  outcome.diagnostic = std::move(cleanupDiagnostic);
  return outcome;
}

} // namespace replay
