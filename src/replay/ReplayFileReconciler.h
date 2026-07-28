#pragma once

#include "ReplayFileLifecycle.h"
#include "ReplayProfileInventory.h"

#include "../repositories/ReplayRepository.h"

#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace replay {

struct ReplayFileReconcilerDependencies {
  std::function<ModernReplayFileInventoryOutcome()> listTombstones;
  std::function<bool(const ReplayFileMetadata &, std::string &)>
      removeReferencedEntry;
  std::function<void(std::chrono::system_clock::time_point)>
      removeStaleTemporaryFiles;
  std::function<ModernReplayReservationReconciliationOutcome()>
      listReservations;
  std::function<bool(const ReplayFileMetadata &, std::string &)>
      removeOwnedReservedEntry;
  std::function<ModernReplayReservationReleaseOutcome(
      const ModernReplayPathReservation &)>
      releaseReservation;
};

struct ReplayFileReconciliationReport {
  std::size_t referencesScanned = 0;
  std::size_t tombstonesFound = 0;
  std::size_t filesRemoved = 0;
  std::size_t reservationsScanned = 0;
  std::size_t unassociatedReservationsFound = 0;
  std::size_t attachedReservationsFound = 0;
  std::size_t unassociatedFilesRemoved = 0;
  std::size_t reservationsReleased = 0;
  std::vector<std::string> failures;
};

class ReplayFileReconciler {
public:
  explicit ReplayFileReconciler(ReplayFileReconcilerDependencies dependencies);

  [[nodiscard]] ReplayFileReconciliationReport
  reconcile(std::chrono::system_clock::time_point staleTemporaryCutoff) const
      noexcept;

private:
  ReplayFileReconcilerDependencies dependencies_;
};

} // namespace replay
