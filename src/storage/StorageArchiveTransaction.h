#pragma once
#include "StorageArchive.h"
#include "StoragePlatform.h"
#include "StorageRecoveryProgress.h"

namespace storage_management {
class StorageWorkBudget;
struct StorageArchiveMetrics {
  std::uint64_t archivesCompacted = 0, bytesWritten = 0, peakScratchBytes = 0;
  std::uint64_t preparationBytesRead = 0, sourceBytesRead = 0, durableBatches = 0;
  std::uint64_t preparationReadCalls = 0, preparationBackwardReads = 0, sourceWriteRanges = 0;
  std::uint64_t parallelHashBatches = 0, peakHashWorkers = 0;
  // Optional wall-clock diagnostics. Batch redo includes its durability barriers;
  // journal includes batch SQLite commits; sync measures the batch archive fsync.
  std::uint64_t totalNanoseconds = 0, preparationNanoseconds = 0;
  std::uint64_t readNanoseconds = 0, hashNanoseconds = 0, redoNanoseconds = 0;
  std::uint64_t journalNanoseconds = 0, writeNanoseconds = 0, syncNanoseconds = 0;
};
struct StorageArchiveDependencies {
  // Encoders/checkpoints may run concurrently on private staged files. Archive
  // progress, validation, fault points and all source writes stay on the owner.
  std::function<EncodeResult(const AssetCandidate &, const StoragePolicy &,
      const std::filesystem::path &, Checkpoint, ProgressCallback)> encode;
  std::function<StorageDiagnostic(const AssetCandidate &, Checkpoint)> validateExisting;
  std::function<std::optional<std::uint64_t>(const std::filesystem::path &)> availableSpace;
  std::function<void(std::string_view)> faultPoint;
  // Includes every pending extracted input/output and the prepared replacement.
  // Journal metadata and a bounded pair of redo buffers are additional.
  std::uint64_t scratchLimit = 1024ULL * 1024 * 1024;
  StorageArchiveMetrics *metrics = nullptr;
  // Displayed rows for this container, including unchanged inner siblings.
  // Successful updates advance their identities using committed entry deltas.
  std::span<const GroupStorageMeasurement> measurementBaselines;
  StorageWorkBudget *workBudget = nullptr;
  StorageExecutionLimits executionLimits = DefaultStorageExecutionLimits();
};
StorageReport UpdateStorageArchive(const StorageArchiveSummary &, const StoragePolicy &,
    const StoragePlatformPaths &, const StorageArchiveDependencies &, Checkpoint = {}, ProgressCallback = {});
StorageReport RecoverStorageArchives(const StoragePlatformPaths &, Checkpoint = {},
    const StorageArchiveDependencies & = {}, StorageRecoveryProgressCallback = {});
}
