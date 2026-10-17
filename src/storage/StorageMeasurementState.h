#pragma once
#include "ChartResourceInventory.h"
namespace storage_management {
using StorageMeasurementRows = std::map<std::string, GroupStorageMeasurement>;
struct StorageGroupSource {
  FolderSelection selection;
  std::string revision;
};
// Returns only new/changed rows; stable sources retain their completed sizes.
std::set<std::string> ReconcileStorageMeasurements(StorageMeasurementRows &,
    std::map<std::string, StorageGroupSource> &previous, const std::vector<StorageGroupSource> &, bool invalidateAll);
// Applies only deltas tied to the displayed generation and preflight baseline.
// Uncertain affected rows become pending; unrelated rows are untouched.
std::set<std::string> ApplyStorageReportMeasurements(StorageMeasurementRows &,
    const std::vector<FolderSelection> &, const StorageReport &);
}
