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
  });
  const auto report = reconciler.reconcile(std::chrono::system_clock::now());
  assert(report.referencesScanned == 0 && report.filesRemoved == 0 &&
         report.failures == std::vector<std::string>{"database unavailable"});
}

} // namespace

int main() {
  testOnlyTombstonedOwnershipIsRemoved();
  testInventoryFailureIsNonThrowingAndConservative();
  return 0;
}
