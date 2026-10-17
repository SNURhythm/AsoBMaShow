#pragma once

#include "StorageDirectoryInventory.h"
#include <span>
#include <stop_token>

namespace storage_management {
enum class StorageScanJobKind { Directory, Archive };
struct StorageScanJob {
  StorageScanJobKind kind = StorageScanJobKind::Directory;
  std::filesystem::path path, accessRoot;
  std::uint64_t id = 0, parentId = 0, context = 0;
};
struct StorageScanTotals {
  std::uint64_t bytes = 0, audio = 0, video = 0, image = 0, files = 0;
};
struct StorageScanBatch {
  StorageScanJob job;
  std::vector<StorageScanJob> children;
  StorageScanTotals totals;
  StorageDirectoryStamp stamp;
  StorageScanMetrics metrics;
  StorageDiagnostic reason;
  bool directoryComplete = false, subtreeComplete = false;
};
struct StorageScanScheduleMetrics {
  std::size_t peakSubmitted = 0, peakBufferedBatches = 0, peakFrontierRecords = 0;
  std::uint64_t spilledJobs = 0, jobsFinished = 0;
};
struct StorageScanLimits {
  std::size_t workers = 1, archiveBuilders = 1, maxSubmitted = 2;
  std::size_t frontierRecords = 4096, frontierBytes = 8 * 1024 * 1024;
  std::filesystem::path scratchRoot;
  std::stop_token stopToken;
  StorageScanScheduleMetrics *metrics = nullptr;
};
StorageScanLimits DefaultStorageScanLimits();
struct StorageScanDependencies {
  std::function<decltype(VisitStorageDirectory)> enumerate = VisitStorageDirectory;
  StorageScanLimits limits = DefaultStorageScanLimits();
};
using ScanBatchSink = std::function<bool(StorageScanBatch)>;
using ScanJobWork = std::function<void(const StorageScanJob &, Checkpoint, ScanBatchSink)>;
// Work/checkpoint run concurrently; result delivery runs only on the coordinator.
// StopToken wakes queue waiters; Checkpoint also observes foreground admission.
bool RunStorageScan(std::span<const StorageScanJob>, StorageScanLimits,
                    ScanJobWork, ScanBatchSink, Checkpoint = {});
}
