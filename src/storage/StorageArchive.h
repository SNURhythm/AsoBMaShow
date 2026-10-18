#pragma once
#include "StorageArchiveIO.h"
#include <memory>

namespace storage_management {
namespace archive_detail { struct Database; struct Statement; }
enum class StorageArchiveFormat { Zip, Tar };
struct StorageArchiveEntry {
  std::uint64_t id = 0, offset = 0, span = 0, dataOffset = 0, packed = 0, bytes = 0, crc = 0;
  unsigned method = 0, flags = 0;
  bool directory = false;
  std::string name, rawName, metadata;
};
struct StorageArchiveSummary {
  std::filesystem::path path;
  FileIdentity identity;
  std::vector<std::filesystem::path> innerRoots;
  std::uint64_t eligibleCount = 0, eligibleBytes = 0;
};
// Metadata spills to SQLite; entry traversal holds only one row at a time.
class StorageArchive {
public:
  StorageArchive(const std::filesystem::path &path, const std::filesystem::path &manifest,
      const std::vector<std::filesystem::path> &accessRoots, Checkpoint = {}, bool resume = false);
  ~StorageArchive();
  StorageArchive(const StorageArchive &) = delete;
  StorageArchive &operator=(const StorageArchive &) = delete;
  std::optional<StorageArchiveEntry> Next(std::optional<std::uint64_t> afterOffset = {}) const;
  std::optional<AssetCandidate> Candidate(const StorageArchiveEntry &, const StoragePolicy &, StorageDiagnostic &) const;
  void Extract(const StorageArchiveEntry &, const std::filesystem::path &output, Checkpoint) const;
  // Generates a replacement record header and central metadata, keeping the raw
  // filename encoding. The caller streams the already verified encoded payload.
  std::string Replacement(StorageArchiveEntry &, std::string_view extension,
      std::uint64_t bytes, std::uint32_t crc) const;
  std::string Central(const StorageArchiveEntry &, std::uint64_t offset) const;
  StorageArchiveFormat Format() const { return format_; }
  const FileIdentity identity;
  StorageArchiveIO io;
  std::unique_ptr<archive_detail::Database> db;
  std::string comment;
private:
  void ParseZip(Checkpoint);
  void ParseTar(Checkpoint);
  void Add(const StorageArchiveEntry &, archive_detail::Statement &);
  StorageArchiveFormat format_ = StorageArchiveFormat::Zip;
};
bool FindStorageArchive(const std::filesystem::path &virtualPath, std::filesystem::path &archive,
    std::filesystem::path &inner, const std::vector<std::filesystem::path> &accessRoots);
}
