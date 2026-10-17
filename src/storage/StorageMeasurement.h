#pragma once
#include "ChartResourceInventory.h"
#include "StorageScanScheduler.h"
#include <map>

namespace storage_management {
struct StorageDirectoryMeasurement {
  StorageDirectoryStamp stamp;
  StorageScanTotals totals;
  StorageDiagnostic reason;
};
struct StorageMeasurements {
  std::vector<GroupStorageMeasurement> groups;
  std::map<std::filesystem::path, StorageDirectoryMeasurement> directories;
  std::optional<std::uint64_t> totalBytes;
  bool complete = true, cancelled = false;
};
using MeasurementCallback = std::function<void(const std::vector<GroupStorageMeasurement> &)>;
StorageMeasurements MeasureStorageGroups(const InventoryRequest &, Checkpoint = {},
    MeasurementCallback = {}, StorageScanMetrics * = nullptr, StorageScanDependencies = {});
}
