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
  try {
    if (!dependencies_.listReferences) {
      report.failures.emplace_back("replay reference inventory is unavailable");
      return report;
    }
    inventory = dependencies_.listReferences();
  } catch (...) {
    report.failures.emplace_back("replay reference inventory failed");
    return report;
  }
  if (inventory.status != ModernReplayFileInventoryStatus::Loaded) {
    report.failures.push_back(
        inventory.diagnostic.empty() ? "replay reference inventory failed"
                                     : std::move(inventory.diagnostic));
    return report;
  }

  report.referencesScanned = inventory.entries.size();
  for (const auto &entry : inventory.entries) {
    if (!entry.reference.userDeleted) {
      continue;
    }
    ++report.tombstonesFound;
    if (!dependencies_.removeReferencedEntry) {
      report.failures.emplace_back("replay tombstone cleanup is unavailable");
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
  return report;
}

} // namespace replay
