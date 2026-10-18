#pragma once

#include "StoragePolicy.h"

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace storage_management {

// A net size change has only one nonzero side. Keep growth while combining
// results so a larger lossless output offsets savings from other files.
inline void AccumulateStorageBytes(std::uint64_t &reclaimed, std::uint64_t &added,
                                   std::uint64_t reduction, std::uint64_t growth) {
  if (reduction >= growth) {
    const auto delta = reduction - growth;
    if (delta >= added) { reclaimed += delta - added; added = 0; }
    else added -= delta;
  } else {
    const auto delta = growth - reduction;
    if (delta >= reclaimed) { added += delta - reclaimed; reclaimed = 0; }
    else reclaimed -= delta;
  }
}

struct StorageExecutionLimits {
  std::size_t audioWorkers = 1;
  // Bound independent videos separately from each codec's internal threads.
  static constexpr std::size_t videoWorkers = 2;
  static constexpr int videoThreads = 2;
};
StorageExecutionLimits DefaultStorageExecutionLimits();

// May wait on the worker for foreground admission; false requests cancellation.
using Checkpoint = std::function<bool()>;
enum class StoragePhase { Inventory, Encode, Verify, Commit, Complete, ArchivePrepare, Prepare };
struct StorageProgress {
  StoragePhase phase = StoragePhase::Inventory;
  std::filesystem::path currentPath;
  std::uint64_t filesDone = 0;
  std::uint64_t filesTotal = 0;
  std::uint64_t reclaimedBytes = 0;
  std::uint64_t addedBytes = 0;
  std::uint64_t archiveBytesDone = 0, archiveBytesTotal = 0;
};
using ProgressCallback = std::function<void(const StorageProgress &)>;

enum class FileOutcome { Converted, Deleted, Skipped, Failed, Cancelled };
enum class StorageReason {
  None, PolicyKeep, UnsupportedSource, UnsupportedFormat, CodecUnavailable,
  UnsafePath, IncompleteInventory, Collision, StaleInput, NotSmaller,
  AlreadyOptimized, IoError, ValidationFailed, InsufficientSpace,
  CleanupFailed, RecoveryRequired, Cancelled
};
struct StorageDiagnostic {
  StorageReason code = StorageReason::None;
  std::string detail;
};
struct FileIdentity {
  std::uint64_t bytes = 0;
  std::filesystem::file_time_type modified;
  std::uint64_t device = 0;
  std::uint64_t fileId = 0;
  bool operator==(const FileIdentity &) const = default;
};
struct StorageFileChange {
  std::filesystem::path path;
  // Absent means the transaction removed this directory entry.
  std::optional<FileIdentity> after;
};
struct FileResult {
  std::filesystem::path source;
  std::filesystem::path destination;
  FileOutcome outcome = FileOutcome::Skipped;
  StorageDiagnostic reason;
  std::optional<std::uint64_t> bytesBefore;
  std::optional<std::uint64_t> bytesAfter;
  std::uint64_t reclaimedBytes = 0;
  std::uint64_t addedBytes = 0;
  // Exact committed changes, including a known retained recovery backup.
  std::vector<StorageFileChange> filesystemChanges;
};
struct GroupStorageDelta {
  std::string groupId;
  std::uint64_t bytesBefore = 0, bytesAfter = 0;
  bool needsRefresh = false;
  std::uint64_t audioBefore = 0, audioAfter = 0, videoBefore = 0, videoAfter = 0;
  std::uint64_t imageBefore = 0, imageAfter = 0;
  std::optional<std::uint64_t> baselineBytes;
  std::string sourceIdentity, nextSourceIdentity;
  std::uint64_t baselineGeneration = 0;
};
struct StorageReport {
  // A bounded detail list; counters include files omitted from this list.
  std::vector<FileResult> files;
  std::uint64_t converted = 0;
  std::uint64_t deleted = 0;
  std::uint64_t skipped = 0;
  std::uint64_t failed = 0;
  std::uint64_t reclaimedBytes = 0;
  std::uint64_t addedBytes = 0;
  bool cancelled = false;
  bool recoveryRequired = false;
  bool cleanupPending = false;
  std::vector<std::filesystem::path> changedRoots;
  std::vector<GroupStorageDelta> groupDeltas;
};
struct EncodeResult {
  FileOutcome outcome = FileOutcome::Skipped;
  std::filesystem::path outputPath;
  StorageDiagnostic reason;
};

struct StorageCodecSupport {
  bool available = false;
  std::string reason;
};
struct StorageCodecCapabilities {
  StorageCodecSupport flac, vorbis, x264, aac, mp4;
};
StorageCodecCapabilities ProbeStorageCodecs();

} // namespace storage_management
