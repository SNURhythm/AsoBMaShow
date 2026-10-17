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
    std::map<std::string, StorageGroupSource> &previous, const std::vector<StorageGroupSource> &, bool invalidateAll,
    const std::vector<std::filesystem::path> &changedRoots = {});
// Applies only deltas tied to the displayed generation and preflight baseline.
// Uncertain rows become unavailable; returns only rows inside changed subtrees
// for automatic measurement. Strict ancestors require explicit Refresh.
std::set<std::string> ApplyStorageReportMeasurements(StorageMeasurementRows &,
    const std::vector<FolderSelection> &, const StorageReport &);
}
