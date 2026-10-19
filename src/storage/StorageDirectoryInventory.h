#pragma once

#include "StorageTypes.h"
#include <memory>
#include <array>

namespace storage_management {
enum class StorageVisitMode { SizeOnly, Preflight };
enum class StorageEntryKind { Regular, Directory, Symlink, Other };
// Chart-only change time detects in-place writes even when mtime is restored.
// Keep it separate from FileIdentity: transaction renames legitimately change it.
using StorageChartChangeStamp = std::array<std::uint64_t, 2>;
std::optional<StorageChartChangeStamp> ReadStorageChartChangeStamp(const std::filesystem::path &, std::string &);
struct StorageDirectoryEntry {
  std::filesystem::path name;
  StorageEntryKind kind = StorageEntryKind::Other;
  std::uint64_t bytes = 0;
  std::filesystem::perms permissions = std::filesystem::perms::unknown;
  std::optional<FileIdentity> identity;
  std::optional<StorageChartChangeStamp> chartChange;
};
struct StorageDirectoryStamp {
  std::uint64_t device = 0, fileId = 0;
  std::filesystem::file_time_type modified;
  std::filesystem::perms permissions = std::filesystem::perms::unknown;
  bool operator==(const StorageDirectoryStamp &) const = default;
};
bool ReadStorageDirectoryStamp(const std::filesystem::path &, const std::filesystem::path &accessRoot,
    StorageDirectoryStamp &, std::string &);
std::string StorageDirectorySourceIdentity(const StorageDirectoryStamp &);
// Each worker owns its metrics, then the coordinator sums completed batches.
struct StorageScanMetrics {
  std::uint64_t entries = 0, metadataReads = 0, rootIndexSteps = 0;
  std::uint64_t archiveIndexVisits = 0, archiveEntries = 0;
  std::uint64_t peakArchiveAggregationRecords = 0; // Maximum for one archive, excluding the streaming index.
};
using EntryVisitor = std::function<bool(const StorageDirectoryEntry &)>;
bool VisitStorageDirectory(const std::filesystem::path &, const std::filesystem::path &accessRoot,
    StorageVisitMode, Checkpoint, EntryVisitor, StorageDirectoryStamp &, StorageScanMetrics *, std::string &);
}

namespace storage_management {
struct StorageDirectoryLimits {
  std::size_t records = 4096, bytes = 8 * 1024 * 1024;
  std::filesystem::path scratchRoot;
};
// One directory's bounded, ordered metadata store. No media contents are read.
class StorageDirectoryInventory {
public:
  explicit StorageDirectoryInventory(std::filesystem::path, StorageDirectoryLimits = {});
  ~StorageDirectoryInventory();
  StorageDirectoryInventory(const StorageDirectoryInventory &) = delete;
  StorageDirectoryInventory &operator=(const StorageDirectoryInventory &) = delete;
  bool Add(const StorageDirectoryEntry &, std::string &);
  bool Set(const StorageDirectoryEntry &, std::string &);
  bool Remove(const std::filesystem::path &name, std::string &);
  std::optional<StorageDirectoryEntry> Find(const std::filesystem::path &name, std::string &) const;
  std::optional<StorageDirectoryEntry> Next(const std::filesystem::path &after, std::string &) const;
  bool Visit(Checkpoint, EntryVisitor, std::string &) const;
  std::uint64_t StemCount(const std::filesystem::path &, bool audio, std::string &) const;
  std::string Fingerprint(Checkpoint, std::string &) const;
  std::size_t Size() const;
  bool Spilled() const;
  const std::filesystem::path &Directory() const;
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
