#include "ReplayProfileInventory.h"

#include "ReplayReferenceAgreement.h"

#include <utility>

namespace replay {
namespace {

ModernReplayFileInventoryOutcome conflict(std::string diagnostic) {
  return {.status = ModernReplayFileInventoryStatus::IntegrityConflict,
          .diagnostic = std::move(diagnostic)};
}

ModernReplayReservationReconciliationOutcome reservationConflict(
    std::string diagnostic) {
  return {.status = ModernReplayFileInventoryStatus::IntegrityConflict,
          .diagnostic = std::move(diagnostic)};
}

ModernReplayReservationReconciliationOutcome reservationStorageFailure(
    std::string diagnostic) {
  return {.status = ModernReplayFileInventoryStatus::StorageFailure,
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

ModernReplayReservationReconciliationOutcome
loadAgreedModernReplayPathReservationInventory(
    ReplayRepository &repository) noexcept {
  try {
    auto reservations = repository.ListModernReplayPathReservations();
    if (reservations.status != ModernReplayFileInventoryStatus::Loaded) {
      return {.status = reservations.status,
              .diagnostic = std::move(reservations.diagnostic)};
    }
    ModernReplayReservationReconciliationOutcome outcome{
        .status = ModernReplayFileInventoryStatus::Loaded};
    outcome.entries.reserve(reservations.reservations.size());
    for (auto &reservation : reservations.reservations) {
      const auto chart =
          repository.LoadModernChartResultByAttempt(reservation.attemptId);
      const auto course =
          repository.LoadModernCourseResultByAttempt(reservation.attemptId);
      if (chart.status == ModernChartResultReadStatus::StorageFailure ||
          course.status == ModernCourseResultReadStatus::StorageFailure) {
        return reservationStorageFailure(
            !chart.diagnostic.empty() ? chart.diagnostic : course.diagnostic);
      }
      if (chart.status == ModernChartResultReadStatus::Invalid ||
          chart.status == ModernChartResultReadStatus::IntegrityConflict ||
          course.status == ModernCourseResultReadStatus::Invalid ||
          course.status == ModernCourseResultReadStatus::IntegrityConflict) {
        return reservationConflict(
            !chart.diagnostic.empty() ? chart.diagnostic : course.diagnostic);
      }
      const bool chartLoaded =
          chart.status == ModernChartResultReadStatus::Loaded;
      const bool courseLoaded =
          course.status == ModernCourseResultReadStatus::Loaded;
      if (chartLoaded && courseLoaded) {
        return reservationConflict(
            "Replay reservation attempt has both chart and course results");
      }

      ModernReplayReservationCommitState state =
          ModernReplayReservationCommitState::Unassociated;
      if (chartLoaded) {
        if (!chart.record) {
          return reservationConflict(
              "Chart replay reservation result has no payload");
        }
        state = ModernReplayReservationCommitState::ResultOnly;
        if (chart.record->replayFile) {
          const auto agreement = compareChartReplayReferenceToResult(
              *chart.record->replayFile, chart.record->result);
          if (!agreement.matches ||
              chart.record->replayFile->identity != reservation.identity) {
            return reservationConflict(
                agreement.diagnostic.empty()
                    ? "Chart replay reservation differs from its committed "
                      "attachment"
                    : agreement.diagnostic);
          }
          state = ModernReplayReservationCommitState::ReplayAttached;
        }
      } else if (courseLoaded) {
        if (!course.record) {
          return reservationConflict(
              "Course replay reservation result has no payload");
        }
        state = ModernReplayReservationCommitState::ResultOnly;
        if (course.record->replayFile) {
          const auto agreement = compareCourseReplayReferenceToResult(
              *course.record->replayFile, course.record->result);
          if (!agreement.matches ||
              course.record->replayFile->identity != reservation.identity) {
            return reservationConflict(
                agreement.diagnostic.empty()
                    ? "Course replay reservation differs from its committed "
                      "attachment"
                    : agreement.diagnostic);
          }
          state = ModernReplayReservationCommitState::ReplayAttached;
        }
      }
      outcome.entries.push_back({.reservation = std::move(reservation),
                                 .commitState = state});
    }
    return outcome;
  } catch (...) {
    return reservationStorageFailure(
        "Replay reservation ownership inventory failed");
  }
}

} // namespace replay
