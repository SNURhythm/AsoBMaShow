#pragma once
#include "ChartResourceInventory.h"
#include <span>
#include <stdexcept>

namespace storage_management {
struct StorageArchiveError : std::runtime_error {
  StorageReason code;
  StorageArchiveError(StorageReason code, const std::string &message) : std::runtime_error(message), code(code) {}
};
// A pinned, exclusively locked file. All offsets are checked before OS calls.
class StorageArchiveIO {
public:
  StorageArchiveIO(const std::filesystem::path &, const std::vector<std::filesystem::path> &accessRoots, bool write = true);
  ~StorageArchiveIO();
  StorageArchiveIO(const StorageArchiveIO &) = delete;
  StorageArchiveIO &operator=(const StorageArchiveIO &) = delete;
  void Read(std::uint64_t offset, std::span<char>) const;
  std::string Read(std::uint64_t offset, std::size_t length) const;
  void Write(std::uint64_t offset, std::string_view);
  void Truncate(std::uint64_t);
  void Sync();
  FileIdentity Identity() const;
  void CheckPath() const;
  const std::filesystem::path path;
private:
  void CaptureWrite();
  std::vector<std::filesystem::path> accessRoots_;
  FileIdentity original_;
#ifdef _WIN32
  void *handle_ = reinterpret_cast<void *>(-1);
#else
  int fd_ = -1;
#endif
};
}
