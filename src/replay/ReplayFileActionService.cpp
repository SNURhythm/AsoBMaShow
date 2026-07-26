#include "ReplayFileActionService.h"

#include "../ProfileDatabaseActivity.h"
#include "../Uuid.h"

#include <utility>

namespace {

ReplayAvailability availabilityFor(replay::ReplayFileState state) {
  switch (state) {
  case replay::ReplayFileState::Available:
    return ReplayAvailability::Available;
  case replay::ReplayFileState::Missing:
    return ReplayAvailability::Missing;
  case replay::ReplayFileState::Corrupt:
    return ReplayAvailability::Corrupt;
  case replay::ReplayFileState::Unsafe:
    return ReplayAvailability::Unsafe;
  case replay::ReplayFileState::IoFailure:
    return ReplayAvailability::IoFailure;
  }
  return ReplayAvailability::IoFailure;
}

} // namespace

ReplayFileActionService::ReplayFileActionService(
    ReplayRepository &repository, replay::ReplayFileStore &fileStore)
    : repository_(repository), fileStore_(fileStore) {}

std::optional<ReplayFileActionService::ResolvedReference>
ReplayFileActionService::resolve(LocalResultRecordId record,
                                 ReplayFileActionOutcome &outcome) {
  if (record.resultId <= 0) {
    outcome.availability = ReplayAvailability::Missing;
    outcome.diagnostic = "Replay result identity is invalid";
    return std::nullopt;
  }

  std::optional<ReplayFileReference> reference;
  if (record.kind == ReplayFileReference::RecordKind::CourseResult) {
    auto loaded = repository_.loadCourseResult(record.resultId);
    if (loaded.status != CourseResultReadOutcome::Status::Loaded ||
        !loaded.record.has_value()) {
      outcome.availability = ReplayAvailability::IoFailure;
      outcome.diagnostic = loaded.diagnostic.empty()
                               ? "Course result is unavailable"
                               : std::move(loaded.diagnostic);
      return std::nullopt;
    }
    reference = loaded.record->replayFile;
  } else {
    auto loaded = repository_.loadChartResult(record.resultId);
    if (loaded.status != ResultReadOutcome::Status::Loaded ||
        !loaded.record.has_value()) {
      outcome.availability = ReplayAvailability::IoFailure;
      outcome.diagnostic = loaded.diagnostic.empty()
                               ? "Chart result is unavailable"
                               : std::move(loaded.diagnostic);
      return std::nullopt;
    }
    reference = loaded.record->replayFile;
  }
  if (!reference.has_value()) {
    outcome.availability = ReplayAvailability::Missing;
    outcome.diagnostic = "This result has no replay file reference";
    return std::nullopt;
  }

  return ResolvedReference{
      .reference = *reference,
      .metadata =
          {
              .relativePath = reference->relativePath,
              .sha256 = reference->contentSha256,
              .compressedSize = reference->compressedSize,
              .codecVersion = reference->codecVersion,
          },
  };
}

ReplayFileActionOutcome ReplayFileActionService::inspectResolved(
    const ResolvedReference &resolved) const {
  const auto inspection = fileStore_.inspect(resolved.metadata);
  ReplayFileActionOutcome outcome{
      .availability = availabilityFor(inspection.state),
      .diagnostic = inspection.diagnostic,
  };
  if (inspection.state == replay::ReplayFileState::Available) {
    outcome.sourcePath = repository_.GetResolvedDatabasePath().parent_path() /
                         resolved.metadata.relativePath;
    outcome.suggestedFilename =
        resolved.metadata.relativePath.filename().string();
  }
  return outcome;
}

ReplayFileActionOutcome
ReplayFileActionService::inspect(LocalResultRecordId record) {
  profile_database_activity::ReadGuard operation;
  ReplayFileActionOutcome outcome;
  const auto resolved = resolve(record, outcome);
  return resolved.has_value() ? inspectResolved(*resolved) : outcome;
}

ReplayFileActionOutcome
ReplayFileActionService::remove(LocalResultRecordId record) {
  profile_database_activity::WriteGuard operation;
  ReplayFileActionOutcome outcome;
  const auto resolved = resolve(record, outcome);
  if (!resolved.has_value()) {
    return outcome;
  }
  outcome = inspectResolved(*resolved);
  if (outcome.availability == ReplayAvailability::Missing) {
    return outcome;
  }
  if (outcome.availability != ReplayAvailability::Available &&
      outcome.availability != ReplayAvailability::Corrupt) {
    return outcome;
  }
  std::string diagnostic;
  if (!fileStore_.remove(resolved->metadata, diagnostic)) {
    return {.availability = ReplayAvailability::IoFailure,
            .diagnostic = std::move(diagnostic)};
  }
  return {.availability = ReplayAvailability::Missing, .changed = true};
}

ReplayFileActionOutcome
ReplayFileActionService::copyToBeatorajaSlot(LocalResultRecordId record,
                                             int slot) {
  profile_database_activity::WriteGuard operation;
  ReplayFileActionOutcome outcome;
  const auto resolved = resolve(record, outcome);
  if (!resolved.has_value()) {
    return outcome;
  }
  outcome = inspectResolved(*resolved);
  if (outcome.availability != ReplayAvailability::Available) {
    return outcome;
  }
  std::string diagnostic;
  const auto destination =
      replay::pathForStem(resolved->reference.stem, slot, diagnostic);
  if (!destination.has_value()) {
    return {.availability = ReplayAvailability::IoFailure,
            .diagnostic = std::move(diagnostic)};
  }
  if (destination->relativePath != resolved->reference.relativePath) {
    auto owner = repository_.loadReplayFileReference(resolved->reference.stem,
                                                     destination->historyIndex);
    if (owner.status == ReplayFileReferenceLookupOutcome::Status::Loaded &&
        owner.reference.has_value() &&
        (owner.reference->contentSha256 != resolved->reference.contentSha256 ||
         owner.reference->compressedSize !=
             resolved->reference.compressedSize ||
         owner.reference->codecVersion != resolved->reference.codecVersion)) {
      const replay::ReplayFileMetadata displacedMetadata{
          .relativePath = owner.reference->relativePath,
          .sha256 = owner.reference->contentSha256,
          .compressedSize = owner.reference->compressedSize,
          .codecVersion = owner.reference->codecVersion,
      };
      const auto displacedInspection = fileStore_.inspect(displacedMetadata);
      if (displacedInspection.state != replay::ReplayFileState::Available) {
        return {.availability = availabilityFor(displacedInspection.state),
                .diagnostic = displacedInspection.diagnostic.empty()
                                  ? "Occupied replay slot is unavailable"
                                  : displacedInspection.diagnostic};
      }

      auto reservation = repository_.reserveReplayFile(uuid::generateV4(),
                                                       owner.reference->stem);
      if ((reservation.status != ReservationOutcome::Status::Reserved &&
           reservation.status != ReservationOutcome::Status::AlreadyReserved) ||
          !reservation.reservation.has_value()) {
        return {.availability = ReplayAvailability::IoFailure,
                .diagnostic = reservation.diagnostic.empty()
                                  ? "Could not reserve displaced replay path"
                                  : std::move(reservation.diagnostic)};
      }
      const replay::ReplayPathIdentity relocationDestination{
          .stem = reservation.reservation->stem,
          .historyIndex = reservation.reservation->historyIndex,
          .relativePath = reservation.reservation->relativePath,
      };
      if (!fileStore_.copyToReservedReplayPath(
              displacedMetadata, relocationDestination, diagnostic)) {
        std::string cleanupDiagnostic;
        (void)repository_.discardReplayFileReservation(*reservation.reservation,
                                                       cleanupDiagnostic);
        return {.availability = ReplayAvailability::IoFailure,
                .diagnostic = std::move(diagnostic)};
      }
      const auto relocation = repository_.relocateReplayFileReference(
          *owner.reference, *reservation.reservation);
      if (relocation.status != ReplayFileRelocationOutcome::Status::Relocated) {
        const replay::ReplayFileMetadata relocationMetadata{
            .relativePath = reservation.reservation->relativePath,
            .sha256 = owner.reference->contentSha256,
            .compressedSize = owner.reference->compressedSize,
            .codecVersion = owner.reference->codecVersion,
        };
        std::string cleanupDiagnostic;
        (void)fileStore_.remove(relocationMetadata, cleanupDiagnostic);
        (void)repository_.discardReplayFileReservation(*reservation.reservation,
                                                       cleanupDiagnostic);
        return {.availability = ReplayAvailability::IoFailure,
                .diagnostic = relocation.diagnostic.empty()
                                  ? "Could not relocate occupied replay slot"
                                  : relocation.diagnostic};
      }
    } else if (owner.status !=
                   ReplayFileReferenceLookupOutcome::Status::NotFound &&
               owner.status !=
                   ReplayFileReferenceLookupOutcome::Status::Loaded) {
      return {.availability = ReplayAvailability::IoFailure,
              .diagnostic = owner.diagnostic.empty()
                                ? "Could not inspect occupied replay slot"
                                : std::move(owner.diagnostic)};
    }
  }
  if (!fileStore_.copyToBeatorajaSlot(
          resolved->metadata, resolved->reference.stem, slot, diagnostic)) {
    return {.availability = ReplayAvailability::IoFailure,
            .diagnostic = std::move(diagnostic)};
  }
  outcome.changed = true;
  return outcome;
}
