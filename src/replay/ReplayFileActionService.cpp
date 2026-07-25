#include "ReplayFileActionService.h"

#include "../ProfileDatabaseActivity.h"

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
    outcome.sourcePath =
        repository_.GetResolvedDatabasePath().parent_path() /
        resolved.metadata.relativePath;
    outcome.suggestedFilename = resolved.metadata.relativePath.filename().string();
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

ReplayFileActionOutcome ReplayFileActionService::copyToBeatorajaSlot(
    LocalResultRecordId record, int slot) {
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
  if (!fileStore_.copyToBeatorajaSlot(resolved->metadata,
                                      resolved->reference.stem, slot,
                                      diagnostic)) {
    return {.availability = ReplayAvailability::IoFailure,
            .diagnostic = std::move(diagnostic)};
  }
  outcome.changed = true;
  return outcome;
}
