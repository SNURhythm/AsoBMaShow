#include "ReplayFileReconciler.h"

#include <utility>

namespace replay {

ReplayFileReconciler::ReplayFileReconciler(
    ReplayFileReconcilerDependencies dependencies)
    : dependencies_(std::move(dependencies)) {}

ReplayFileReconciliationReport ReplayFileReconciler::reconcile(
    std::chrono::system_clock::time_point staleTemporaryCutoff) const noexcept {
  ReplayFileReconciliationReport report;
  try {
    if (dependencies_.removeStaleTemporaryFiles) {
      dependencies_.removeStaleTemporaryFiles(staleTemporaryCutoff);
    }
  } catch (...) {
    report.failures.emplace_back("stale replay temporary cleanup failed");
  }

  ModernReplayFileInventoryOutcome inventory;
  bool tombstoneInventoryReturned = false;
  try {
    if (!dependencies_.listTombstones) {
      report.failures.emplace_back("replay tombstone inventory is unavailable");
    } else {
      inventory = dependencies_.listTombstones();
      tombstoneInventoryReturned = true;
    }
  } catch (...) {
    report.failures.emplace_back("replay tombstone inventory failed");
  }
  if (tombstoneInventoryReturned &&
      inventory.status != ModernReplayFileInventoryStatus::Loaded) {
    report.failures.push_back(
        inventory.diagnostic.empty() ? "replay tombstone inventory failed"
                                     : std::move(inventory.diagnostic));
  } else if (tombstoneInventoryReturned) {
    report.referencesScanned = inventory.entries.size();
    for (const auto &entry : inventory.entries) {
      if (!entry.reference.userDeleted) {
        report.failures.emplace_back(
            "replay tombstone inventory contained an active reference");
        continue;
      }
      ++report.tombstonesFound;
      if (!dependencies_.removeReferencedEntry) {
        report.failures.emplace_back(
            "replay tombstone cleanup is unavailable");
        continue;
      }
      std::string diagnostic;
      try {
        if (dependencies_.removeReferencedEntry(entry.reference.metadata,
                                                diagnostic)) {
          ++report.filesRemoved;
          continue;
        }
      } catch (...) {
        diagnostic = "replay tombstone cleanup failed";
      }
      report.failures.push_back(
          diagnostic.empty() ? "replay tombstone cleanup failed"
                             : std::move(diagnostic));
    }
  }

  ModernReplayReservationReconciliationOutcome reservations;
  try {
    if (!dependencies_.listReservations) {
      report.failures.emplace_back(
          "replay reservation inventory is unavailable");
      return report;
    }
    reservations = dependencies_.listReservations();
  } catch (...) {
    report.failures.emplace_back("replay reservation inventory failed");
    return report;
  }
  if (reservations.status != ModernReplayFileInventoryStatus::Loaded) {
    report.failures.push_back(
        reservations.diagnostic.empty()
            ? "replay reservation inventory failed"
            : std::move(reservations.diagnostic));
    return report;
  }

  report.reservationsScanned = reservations.entries.size();
  for (const auto &entry : reservations.entries) {
    const bool attached = entry.commitState ==
                          ModernReplayReservationCommitState::ReplayAttached;
    if (attached) {
      ++report.attachedReservationsFound;
    } else {
      ++report.unassociatedReservationsFound;
      if (!entry.reservation.ownedFile) {
        // A path reservation alone is never proof that this attempt created
        // whatever currently occupies the final path.
      } else if (!dependencies_.removeOwnedReservedEntry) {
        report.failures.emplace_back(
            "unassociated replay cleanup is unavailable");
        continue;
      } else {
        std::string diagnostic;
        try {
          if (!dependencies_.removeOwnedReservedEntry(
                  *entry.reservation.ownedFile, diagnostic)) {
            report.failures.push_back(
                diagnostic.empty() ? "unassociated replay cleanup failed"
                                   : std::move(diagnostic));
            continue;
          }
          ++report.unassociatedFilesRemoved;
        } catch (...) {
          report.failures.push_back(
              "unassociated replay cleanup failed");
          continue;
        }
      }
    }

    if (!dependencies_.releaseReservation) {
      report.failures.emplace_back(
          "replay reservation release is unavailable");
      continue;
    }
    try {
      const auto released =
          dependencies_.releaseReservation(entry.reservation);
      if (released.status == ModernReplayReservationReleaseStatus::Released ||
          released.status == ModernReplayReservationReleaseStatus::NotFound) {
        ++report.reservationsReleased;
        continue;
      }
      report.failures.push_back(
          released.diagnostic.empty() ? "replay reservation release failed"
                                      : released.diagnostic);
    } catch (...) {
      report.failures.emplace_back("replay reservation release failed");
    }
  }
  return report;
}

} // namespace replay
