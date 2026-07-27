#include "ReplayProfileInventory.h"

#include "ReplayReferenceAgreement.h"

#include <utility>

namespace replay {
namespace {

ModernReplayFileInventoryOutcome conflict(std::string diagnostic) {
  return {.status = ModernReplayFileInventoryStatus::IntegrityConflict,
          .diagnostic = std::move(diagnostic)};
}

} // namespace

ModernReplayFileInventoryOutcome
loadAgreedModernReplayFileInventory(ReplayRepository &repository) noexcept {
  try {
    auto inventory = repository.ListModernReplayFileReferences();
    if (inventory.status != ModernReplayFileInventoryStatus::Loaded) {
      return inventory;
    }
    for (const auto &entry : inventory.entries) {
      if (entry.owner == ModernReplayOwnerKind::ChartResult) {
        const auto loaded =
            repository.LoadModernChartResultByAttempt(entry.attemptId);
        if (loaded.status != ModernChartResultReadStatus::Loaded ||
            !loaded.record || !loaded.record->replayFile ||
            *loaded.record->replayFile != entry.reference) {
          return conflict(
              "Chart replay inventory does not match its exact result");
        }
        const auto agreement = compareChartReplayReferenceToResult(
            entry.reference, loaded.record->result);
        if (!agreement.matches) {
          return conflict(agreement.diagnostic);
        }
        continue;
      }
      const auto loaded =
          repository.LoadModernCourseResultByAttempt(entry.attemptId);
      if (loaded.status != ModernCourseResultReadStatus::Loaded ||
          !loaded.record || !loaded.record->replayFile ||
          *loaded.record->replayFile != entry.reference) {
        return conflict(
            "Course replay inventory does not match its exact result");
      }
      const auto agreement = compareCourseReplayReferenceToResult(
          entry.reference, loaded.record->result);
      if (!agreement.matches) {
        return conflict(agreement.diagnostic);
      }
    }
    return inventory;
  } catch (...) {
    return {.status = ModernReplayFileInventoryStatus::StorageFailure,
            .diagnostic = "Replay ownership inventory failed"};
  }
}

} // namespace replay
