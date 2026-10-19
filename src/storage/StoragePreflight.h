#pragma once
#include "ChartResourceInventory.h"
#include "StorageScanScheduler.h"
#include "StorageArchive.h"
#include "StoragePathComparison.h"

namespace storage_management {
struct StorageDirectorySummary {
  std::filesystem::path directory, accessRoot;
  StorageDirectoryStamp stamp;
  std::string fingerprint;
  std::string chartFingerprint; // Charts and child-directory membership, excluding media.
  std::vector<std::filesystem::path> childDirectories;
  StorageScanTotals totals;
  std::uint64_t eligibleCount = 0, eligibleBytes = 0, skippedCount = 0;
  std::map<StorageReason, std::uint64_t> reasons;
  std::vector<FileResult> samples; // Drained into the preflight's global bounded sample list.
  StorageDiagnostic reason;
};
struct StoragePreflight {
  InventoryRequest request;
  std::vector<GroupStorageMeasurement> groups;
  std::map<std::filesystem::path, StorageDirectorySummary, StoragePathLess> directories;
  std::vector<StorageArchiveSummary> archives;
  std::vector<FileResult> skips;
  std::optional<std::uint64_t> totalBytes;
  std::uint64_t eligibleCount = 0, eligibleBytes = 0, skippedCount = 0;
  // A completed review may contain unavailable groups; those remain skipped.
  // False means traversal was interrupted, not that every folder must be usable.
  bool complete = true, cancelled = false;
  std::vector<std::filesystem::path> EligibleRoots() const;
};
StoragePreflight BuildStoragePreflight(const InventoryRequest &, Checkpoint = {},
    StorageScanMetrics * = nullptr, StorageScanDependencies = {});
void AppendStorageArchivePreflight(StoragePreflight &, Checkpoint, StorageScanMetrics * = nullptr);
std::unique_ptr<StorageDirectoryInventory> LoadStorageDirectory(const StorageDirectorySummary &,
    const InventoryRequest &, Checkpoint, std::string &, StorageDirectoryLimits = {});
std::optional<AssetCandidate> StorageCandidateForEntry(const StorageDirectoryEntry &,
    const StorageDirectoryInventory &, const StoragePolicy &, const std::vector<std::string> &groups,
    StorageDiagnostic &);
}
