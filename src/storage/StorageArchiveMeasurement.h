#pragma once
#include "StorageMeasurement.h"

namespace storage_management {
// Cache candidate archive boundaries while grouping virtual paths, so thousands
// of folders in one archive do not repeat its filesystem metadata probes.
bool SplitStorageArchivePath(const std::filesystem::path &, std::filesystem::path &archive,
    std::filesystem::path &inner, std::map<std::filesystem::path, bool> &knownBoundaries);
std::vector<GroupStorageMeasurement> MeasureArchiveGroups(const std::filesystem::path &,
    std::span<const FolderSelection>, Checkpoint = {}, StorageScanMetrics * = nullptr);
}
