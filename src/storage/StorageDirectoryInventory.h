#pragma once

#include "StorageTypes.h"

namespace storage_management {
enum class StorageVisitMode { SizeOnly, Preflight };
enum class StorageEntryKind { Regular, Directory, Symlink, Other };
struct StorageDirectoryEntry {
  std::filesystem::path name;
  StorageEntryKind kind = StorageEntryKind::Other;
  std::uint64_t bytes = 0;
  std::filesystem::perms permissions = std::filesystem::perms::unknown;
  std::optional<FileIdentity> identity;
};
struct StorageDirectoryStamp {
  std::uint64_t device = 0, fileId = 0;
  std::filesystem::file_time_type modified;
  std::filesystem::perms permissions = std::filesystem::perms::unknown;
  bool operator==(const StorageDirectoryStamp &) const = default;
};
// Each worker owns its metrics, then the coordinator sums completed batches.
struct StorageScanMetrics {
  std::uint64_t entries = 0, metadataReads = 0, rootIndexSteps = 0;
  std::uint64_t archiveIndexVisits = 0, archiveEntries = 0;
};
using EntryVisitor = std::function<bool(const StorageDirectoryEntry &)>;
bool VisitStorageDirectory(const std::filesystem::path &, const std::filesystem::path &accessRoot,
    StorageVisitMode, Checkpoint, EntryVisitor, StorageDirectoryStamp &, StorageScanMetrics *, std::string &);
}
