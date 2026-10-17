#pragma once
#include "StorageArchive.h"
#include "StoragePlatform.h"

namespace storage_management {
struct StorageArchiveMetrics {
  std::uint64_t archivesCompacted = 0, bytesWritten = 0, peakScratchBytes = 0;
};
struct StorageArchiveDependencies {
  std::function<EncodeResult(const AssetCandidate &, const StoragePolicy &,
      const std::filesystem::path &, Checkpoint, ProgressCallback)> encode;
  std::function<std::optional<std::uint64_t>(const std::filesystem::path &)> availableSpace;
  std::function<void(std::string_view)> faultPoint;
  // Includes the one extracted input, encoded output and prepared replacement.
  // Journal metadata and a bounded pair of redo buffers are additional.
  std::uint64_t scratchLimit = 1024ULL * 1024 * 1024;
  StorageArchiveMetrics *metrics = nullptr;
};
StorageReport UpdateStorageArchive(const StorageArchiveSummary &, const StoragePolicy &,
    const StoragePlatformPaths &, const StorageArchiveDependencies &, Checkpoint = {}, ProgressCallback = {});
StorageReport RecoverStorageArchives(const StoragePlatformPaths &, Checkpoint = {},
    const StorageArchiveDependencies & = {});
}
