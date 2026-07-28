#include "ReplayFileReconciler.h"

#include "ReplayFileStore.h"

namespace replay {

ReplayFileReconciliationReport reconcileProfileReplayFiles(
    ReplayRepository &repository,
    std::chrono::system_clock::time_point staleTemporaryCutoff) noexcept {
  try {
    ReplayFileStore store(repository.GetResolvedProfileRoot());
    ReplayFileReconciler reconciler({
        .listTombstones =
            [&repository] {
              return loadAgreedModernReplayTombstoneInventory(repository);
            },
        .removeTombstonedEntryIfMatches =
            [&store](const ReplayFileMetadata &metadata,
                     std::string &diagnostic) {
              return store.removeIfMatches(metadata, diagnostic);
            },
        .removeStaleTemporaryFiles =
            [&store](auto cutoff) { store.removeStaleTemporaryFiles(cutoff); },
        .listReservations =
            [&repository] {
              return loadAgreedModernReplayPathReservationInventory(repository);
            },
        .removeOwnedReservedEntry =
            [&store](const ReplayFileMetadata &metadata,
                     std::string &diagnostic) {
              return store.removeIfMatches(metadata, diagnostic);
            },
        .releaseReservation =
            [&repository](const auto &reservation) {
              return repository.ReleaseModernReplayPathReservation(reservation);
            },
    });
    return reconciler.reconcile(staleTemporaryCutoff);
  } catch (...) {
    ReplayFileReconciliationReport report;
    report.failures.emplace_back("replay reconciliation setup failed");
    return report;
  }
}

} // namespace replay
