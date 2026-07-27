#include "ReplayFileAssociationCoordinator.h"

#include <utility>

namespace replay {
namespace {

constexpr std::size_t kMaximumOccupiedSlotRetries = 64;

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

bool releaseReservation(
    const ReplayFileAssociationCoordinatorDependencies &dependencies,
    const ModernReplayPathReservation &reservation, std::string &diagnostic) {
  if (!dependencies.releasePath) {
    appendDiagnostic(diagnostic, "reservation release",
                     "release dependency is unavailable");
    return false;
  }
  const auto released = dependencies.releasePath(reservation);
  if (released.status == ModernReplayReservationReleaseStatus::Released ||
      released.status == ModernReplayReservationReleaseStatus::NotFound) {
    return true;
  }
  appendDiagnostic(diagnostic, "reservation release", released.diagnostic);
  return false;
}

bool validInstalledFile(const ReplayInstalledFile &file,
                        const ReplayFileReservation &reservation) {
  const ReplayFileOwnershipReceipt expectedReceipt{
      .attemptToken = reservation.attemptToken,
      .metadata = reservation.expectedMetadata,
  };
  return file.metadata == reservation.expectedMetadata &&
         file.attemptToken == reservation.attemptToken &&
         file.lifecycle.state ==
             ReplayFileLifecycleState::InstalledUnassociated &&
         file.lifecycle.attemptToken == reservation.attemptToken &&
         file.lifecycle.receipt == expectedReceipt;
}

ReplayFileAssociation attached(
    const ModernReplayPathReservation &pathReservation,
    const ReplayFileMetadata &metadata,
    ReplayFileInstalledOwnership ownership) {
  return {
      .reservation = pathReservation,
      .attachment = {.identity = pathReservation.identity,
                     .metadata = metadata},
      .ownership = ownership,
  };
}

} // namespace

ReplayFileAssociationCoordinator::ReplayFileAssociationCoordinator(
    ReplayFileAssociationCoordinatorDependencies dependencies)
    : dependencies_(std::move(dependencies)) {}

ReplayFileAssociationOutcome ReplayFileAssociationCoordinator::associate(
    std::string_view attemptId, std::string_view stem,
    std::int64_t playedAtUnixMillis, const ReplayFileEncoder &encode) const {
  ReplayFileAssociationOutcome outcome;
  if (!dependencies_.reservePath || !dependencies_.reserveFile ||
      !dependencies_.installFile || !dependencies_.inspectFile || !encode) {
    outcome.diagnostic = "replay file association dependencies are incomplete";
    return outcome;
  }

  auto reserved =
      dependencies_.reservePath(attemptId, stem, playedAtUnixMillis);
  if (!reserved.reservation ||
      (reserved.status != ModernReplayReservationStatus::Reserved &&
       reserved.status != ModernReplayReservationStatus::AlreadyReserved)) {
    if (reserved.status == ModernReplayReservationStatus::IntegrityConflict) {
      outcome.status = ReplayFileAssociationStatus::IntegrityConflict;
    }
    outcome.diagnostic = reserved.diagnostic.empty()
                             ? "replay path reservation failed"
                             : std::move(reserved.diagnostic);
    return outcome;
  }
  ModernReplayPathReservation pathReservation = *reserved.reservation;

  std::string encodeDiagnostic;
  auto encoded = encode(encodeDiagnostic);
  if (!encoded) {
    appendDiagnostic(outcome.diagnostic, "replay omitted", encodeDiagnostic);
    releaseReservation(dependencies_, pathReservation, outcome.diagnostic);
    return outcome;
  }

  for (std::size_t occupied = 0;; ++occupied) {
    auto fileReservation =
        dependencies_.reserveFile(pathReservation.identity, *encoded, attemptId);
    if (!fileReservation.reservation) {
      appendDiagnostic(outcome.diagnostic, "replay omitted",
                       fileReservation.diagnostic);
      releaseReservation(dependencies_, pathReservation, outcome.diagnostic);
      return outcome;
    }

    auto installed =
        dependencies_.installFile(*fileReservation.reservation, *encoded);
    if (installed.state == ReplayInstallState::InstalledVerified &&
        installed.file &&
        validInstalledFile(*installed.file, *fileReservation.reservation)) {
      outcome.status = ReplayFileAssociationStatus::Attached;
      outcome.association = attached(
          pathReservation, installed.file->metadata,
          installed.existingIdenticalFile
              ? ReplayFileInstalledOwnership::PreexistingIdentical
              : ReplayFileInstalledOwnership::CreatedByAttempt);
      return outcome;
    }

    if (installed.state == ReplayInstallState::Occupied &&
        occupied < kMaximumOccupiedSlotRetries) {
      if (!releaseReservation(dependencies_, pathReservation,
                              outcome.diagnostic)) {
        return outcome;
      }
      auto next =
          dependencies_.reservePath(attemptId, stem, playedAtUnixMillis);
      if (!next.reservation ||
          (next.status != ModernReplayReservationStatus::Reserved &&
           next.status != ModernReplayReservationStatus::AlreadyReserved)) {
        if (next.status == ModernReplayReservationStatus::IntegrityConflict) {
          outcome.status = ReplayFileAssociationStatus::IntegrityConflict;
        }
        appendDiagnostic(outcome.diagnostic, "replay reservation",
                         next.diagnostic);
        return outcome;
      }
      pathReservation = *next.reservation;
      continue;
    }

    const auto inspection =
        dependencies_.inspectFile(fileReservation.reservation->expectedMetadata);
    if (inspection.state == ReplayFileState::Available) {
      outcome.status = ReplayFileAssociationStatus::Attached;
      outcome.association =
          attached(pathReservation,
                   fileReservation.reservation->expectedMetadata,
                   ReplayFileInstalledOwnership::Ambiguous);
      return outcome;
    }
    appendDiagnostic(outcome.diagnostic, "replay omitted",
                     installed.diagnostic.empty() ? inspection.diagnostic
                                                  : installed.diagnostic);
    if (inspection.state == ReplayFileState::Missing) {
      releaseReservation(dependencies_, pathReservation, outcome.diagnostic);
    }
    return outcome;
  }
}

bool ReplayFileAssociationCoordinator::abandonDefinitively(
    const ReplayFileAssociation &association, std::string &diagnostic) const {
  switch (association.ownership) {
  case ReplayFileInstalledOwnership::CreatedByAttempt: {
    if (!dependencies_.removeIfMatches) {
      appendDiagnostic(diagnostic, "replay cleanup",
                       "cleanup dependency is unavailable");
      return false;
    }
    std::string cleanupDiagnostic;
    if (!dependencies_.removeIfMatches(association.attachment.metadata,
                                       cleanupDiagnostic)) {
      appendDiagnostic(diagnostic, "replay cleanup", cleanupDiagnostic);
      return false;
    }
    return releaseReservation(dependencies_, association.reservation,
                              diagnostic);
  }
  case ReplayFileInstalledOwnership::PreexistingIdentical:
    return releaseReservation(dependencies_, association.reservation,
                              diagnostic);
  case ReplayFileInstalledOwnership::Ambiguous:
    appendDiagnostic(diagnostic, "replay cleanup",
                     "ambiguous installed ownership was retained");
    return false;
  }
  return false;
}

} // namespace replay
