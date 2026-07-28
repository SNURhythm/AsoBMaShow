#include "replay/ReplayFileReconciler.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace {

ModernReplayFileInventoryEntry entry(int id, bool deleted) {
  const std::string stem(64, static_cast<char>('a' + id));
  return {
      .owner = ModernReplayOwnerKind::ChartResult,
      .attemptId = "123e4567-e89b-42d3-a456-42661417400" +
                   std::to_string(id),
      .reference =
          {.id = id,
           .resultId = id,
           .userDeleted = deleted,
           .identity = {.stem = stem,
                        .historyIndex = 0,
                        .relativePath =
                            std::filesystem::path("replay") / (stem + ".brd")},
           .metadata = {.relativePath =
                            std::filesystem::path("replay") / (stem + ".brd"),
                        .sha256 = std::string(64, 'f'),
                        .compressedSize = 10,
                        .codecVersion = 3}}};
}

replay::ModernReplayReservationReconciliationEntry reservation(
    int id, replay::ModernReplayReservationCommitState state) {
  const std::string stem(64, static_cast<char>('a' + id));
  return {
      .reservation =
          {.attemptId = "123e4567-e89b-42d3-a456-42661417400" +
                        std::to_string(id),
           .identity = {.stem = stem,
                        .historyIndex = 0,
                        .relativePath = std::filesystem::path("replay") /
                                        (stem + ".brd")},
           .createdAtUnixMillis = 1'700'000'000'000LL + id},
      .commitState = state,
  };
}

void testOnlyTombstonedOwnershipIsRemoved() {
  const auto active = entry(1, false);
  const auto deleted = entry(2, true);
  const auto failed = entry(3, true);
  bool staleCleanupCalled = false;
  std::vector<std::filesystem::path> removed;
  replay::ReplayFileReconciler reconciler({
      .listReferences = [&] {
        return ModernReplayFileInventoryOutcome{
            .status = ModernReplayFileInventoryStatus::Loaded,
            .entries = {active, deleted, failed}};
      },
      .removeReferencedEntry =
          [&](const replay::ReplayFileMetadata &metadata,
              std::string &diagnostic) {
            removed.push_back(metadata.relativePath);
            if (metadata.relativePath == failed.reference.metadata.relativePath) {
              diagnostic = "injected cleanup failure";
              return false;
            }
            return true;
          },
      .removeStaleTemporaryFiles = [&](auto) { staleCleanupCalled = true; },
      .listReservations = [] {
        return replay::ModernReplayReservationReconciliationOutcome{
            .status = ModernReplayFileInventoryStatus::Loaded};
      },
  });
  const auto report = reconciler.reconcile(
      std::chrono::system_clock::now() - std::chrono::hours(24));
  const std::vector<std::filesystem::path> expectedRemoved{
      deleted.reference.metadata.relativePath,
      failed.reference.metadata.relativePath};
  assert(staleCleanupCalled && report.referencesScanned == 3 &&
         report.tombstonesFound == 2 && report.filesRemoved == 1 &&
         report.failures.size() == 1 &&
         removed == expectedRemoved);
}

void testInventoryFailureIsNonThrowingAndConservative() {
  replay::ReplayFileReconciler reconciler({
      .listReferences = [] {
        return ModernReplayFileInventoryOutcome{
            .status = ModernReplayFileInventoryStatus::StorageFailure,
            .diagnostic = "database unavailable"};
      },
      .removeReferencedEntry = [](const auto &, auto &) {
        assert(false && "no path is removed without inventory authority");
        return false;
      },
      .removeStaleTemporaryFiles = [](auto) {},
      .listReservations = [] {
        return replay::ModernReplayReservationReconciliationOutcome{
            .status = ModernReplayFileInventoryStatus::Loaded};
      },
  });
  const auto report = reconciler.reconcile(std::chrono::system_clock::now());
  assert(report.referencesScanned == 0 && report.filesRemoved == 0 &&
         report.failures == std::vector<std::string>{"database unavailable"});
}

void testStaleReservationsRemoveOnlyUncommittedReplayPaths() {
  using replay::ModernReplayReservationCommitState;
  const auto unassociated =
      reservation(4, ModernReplayReservationCommitState::Unassociated);
  const auto resultOnly =
      reservation(5, ModernReplayReservationCommitState::ResultOnly);
  const auto attached =
      reservation(6, ModernReplayReservationCommitState::ReplayAttached);
  const auto failed =
      reservation(7, ModernReplayReservationCommitState::Unassociated);
  std::vector<std::filesystem::path> removed;
  std::vector<std::string> released;
  replay::ReplayFileReconciler reconciler({
      .listReferences = [] {
        return ModernReplayFileInventoryOutcome{
            .status = ModernReplayFileInventoryStatus::Loaded};
      },
      .removeReferencedEntry = [](const auto &, auto &) { return true; },
      .removeStaleTemporaryFiles = [](auto) {},
      .listReservations = [&] {
        return replay::ModernReplayReservationReconciliationOutcome{
            .status = ModernReplayFileInventoryStatus::Loaded,
            .entries = {unassociated, resultOnly, attached, failed}};
      },
      .removeReservedEntry =
          [&](const replay::ReplayPathIdentity &identity,
              std::string &diagnostic) {
            removed.push_back(identity.relativePath);
            if (identity == failed.reservation.identity) {
              diagnostic = "injected orphan cleanup failure";
              return false;
            }
            return true;
          },
      .releaseReservation =
          [&](const ModernReplayPathReservation &value) {
            released.push_back(value.attemptId);
            return ModernReplayReservationReleaseOutcome{
                .status = ModernReplayReservationReleaseStatus::Released};
          },
  });

  const auto report = reconciler.reconcile(std::chrono::system_clock::now());
  const std::vector<std::filesystem::path> expectedRemoved{
      unassociated.reservation.identity.relativePath,
      resultOnly.reservation.identity.relativePath,
      failed.reservation.identity.relativePath};
  const std::vector<std::string> expectedReleased{
      unassociated.reservation.attemptId, resultOnly.reservation.attemptId,
      attached.reservation.attemptId};
  assert(report.reservationsScanned == 4 &&
         report.unassociatedReservationsFound == 3 &&
         report.attachedReservationsFound == 1 &&
         report.unassociatedFilesRemoved == 2 &&
         report.reservationsReleased == 3 &&
         report.failures ==
             std::vector<std::string>{"injected orphan cleanup failure"} &&
         removed == expectedRemoved && released == expectedReleased);
}

} // namespace

int main() {
  testOnlyTombstonedOwnershipIsRemoved();
  testInventoryFailureIsNonThrowingAndConservative();
  testStaleReservationsRemoveOnlyUncommittedReplayPaths();
  return 0;
}
