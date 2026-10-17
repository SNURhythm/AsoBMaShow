#include "StorageDirectoryInventory.h"
#include "ChartResourceInventory.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <memory>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace storage_management {
namespace fs = std::filesystem;
namespace {
bool keepGoing(const Checkpoint &check, std::string &error) {
  if (!check || check()) return true;
  error = "Cancelled";
  return false;
}
bool readable(fs::perms permissions) {
  return permissions == fs::perms::unknown ||
      ((permissions & (fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read)) != fs::perms::none &&
       (permissions & (fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec)) != fs::perms::none);
}
#ifdef _WIN32
struct Handle {
  HANDLE value = INVALID_HANDLE_VALUE;
  ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
fs::file_time_type modified(const FILETIME &value) {
  const std::uint64_t ticks = (std::uint64_t(value.dwHighDateTime) << 32) | value.dwLowDateTime;
  const auto sinceUnix = std::chrono::duration<std::int64_t, std::ratio<1, 10000000>>(
      static_cast<std::int64_t>(ticks) - 116444736000000000LL);
  return std::chrono::time_point_cast<fs::file_time_type::duration>(
      fs::file_time_type::clock::from_sys(std::chrono::sys_time(sinceUnix)));
}
bool inspect(const fs::path &path, BY_HANDLE_FILE_INFORMATION &info, std::string &error) {
  Handle handle{CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
  if (handle.value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(handle.value, &info)) {
    error = "Cannot inspect directory entry"; return false;
  }
  return true;
}
StorageDirectoryStamp directoryStamp(const BY_HANDLE_FILE_INFORMATION &info) {
  return {info.dwVolumeSerialNumber,
      (std::uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow,
      modified(info.ftLastWriteTime), fs::perms::unknown};
}
#else
fs::file_time_type modified(const struct stat &info) {
#ifdef __APPLE__
  const auto value = info.st_mtimespec;
#else
  const auto value = info.st_mtim;
#endif
  const auto sinceUnix = std::chrono::seconds(value.tv_sec) + std::chrono::nanoseconds(value.tv_nsec);
  return std::chrono::time_point_cast<fs::file_time_type::duration>(
      fs::file_time_type::clock::from_sys(std::chrono::sys_time(sinceUnix)));
}
StorageDirectoryStamp directoryStamp(const struct stat &info) {
  return {static_cast<std::uint64_t>(info.st_dev), static_cast<std::uint64_t>(info.st_ino),
      modified(info), static_cast<fs::perms>(info.st_mode & 07777)};
}
struct DirectoryCloser { void operator()(DIR *directory) const { closedir(directory); } };
#endif
}

bool VisitStorageDirectory(const fs::path &path, const fs::path &accessRoot,
    StorageVisitMode mode, Checkpoint check, EntryVisitor visit, StorageDirectoryStamp &stamp,
    StorageScanMetrics *metrics, std::string &error) {
  if (!keepGoing(check, error)) return false;
  const std::vector<fs::path> boundaries = accessRoot.empty() ? std::vector<fs::path>{} : std::vector<fs::path>{accessRoot};
  if (!IsSafeLocalPath(path, error, boundaries)) return false;
#ifdef _WIN32
  BY_HANDLE_FILE_INFORMATION initial{};
  if (!inspect(path, initial, error) || !(initial.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
      (initial.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
    error = "Unsafe or inaccessible directory"; return false;
  }
  stamp = directoryStamp(initial);
  std::error_code ec;
  fs::directory_iterator iterator(path, ec), end;
  if (ec) { error = ec.message(); return false; }
  while (iterator != end) {
    if (!keepGoing(check, error)) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    if (!inspect(iterator->path(), info, error)) return false;
    StorageDirectoryEntry entry{.name = iterator->path().filename()};
    if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) entry.kind = StorageEntryKind::Symlink;
    else if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) entry.kind = StorageEntryKind::Directory;
    else {
      entry.kind = StorageEntryKind::Regular;
      entry.bytes = (std::uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
      if (mode == StorageVisitMode::Preflight && info.nNumberOfLinks == 1)
        entry.identity = FileIdentity{entry.bytes, modified(info.ftLastWriteTime), info.dwVolumeSerialNumber,
            (std::uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow};
    }
    if (metrics) { ++metrics->entries; ++metrics->metadataReads; }
    if (visit && !visit(entry)) { error = "Visitor stopped"; return false; }
    iterator.increment(ec);
    if (ec) { error = ec.message(); return false; }
  }
  BY_HANDLE_FILE_INFORMATION current{};
  if (!inspect(path, current, error) || directoryStamp(current) != stamp) {
    error = "Directory changed during traversal"; return false;
  }
#else
  const int descriptor = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) { error = std::strerror(errno); return false; }
  DIR *rawDirectory = fdopendir(descriptor);
  if (!rawDirectory) { error = std::strerror(errno); close(descriptor); return false; }
  std::unique_ptr<DIR, DirectoryCloser> directory(rawDirectory);
  struct stat initial{};
  if (fstat(descriptor, &initial) != 0 || !S_ISDIR(initial.st_mode)) {
    error = "Cannot inspect directory"; return false;
  }
  stamp = directoryStamp(initial);
  if (!readable(stamp.permissions)) { error = "Directory is not readable"; return false; }
  for (;;) {
    if (!keepGoing(check, error)) return false;
    errno = 0;
    const auto *item = readdir(directory.get());
    if (!item) {
      if (errno) { error = std::strerror(errno); return false; }
      break;
    }
    if (std::strcmp(item->d_name, ".") == 0 || std::strcmp(item->d_name, "..") == 0) continue;
    struct stat info{};
    if (fstatat(descriptor, item->d_name, &info, AT_SYMLINK_NOFOLLOW) != 0) {
      error = std::strerror(errno); return false;
    }
    StorageDirectoryEntry entry{.name = fs::path(item->d_name),
        .permissions = static_cast<fs::perms>(info.st_mode & 07777)};
    if (S_ISLNK(info.st_mode)) entry.kind = StorageEntryKind::Symlink;
    else if (S_ISDIR(info.st_mode)) entry.kind = StorageEntryKind::Directory;
    else if (S_ISREG(info.st_mode)) {
      if (info.st_size < 0) { error = "Invalid file size"; return false; }
      entry.kind = StorageEntryKind::Regular;
      entry.bytes = static_cast<std::uint64_t>(info.st_size);
      if (mode == StorageVisitMode::Preflight && info.st_nlink == 1)
        entry.identity = FileIdentity{entry.bytes, modified(info),
            static_cast<std::uint64_t>(info.st_dev), static_cast<std::uint64_t>(info.st_ino)};
    }
    if (metrics) { ++metrics->entries; ++metrics->metadataReads; }
    if (visit && !visit(entry)) { error = "Visitor stopped"; return false; }
  }
  struct stat current{}, named{};
  if (fstat(descriptor, &current) != 0 || lstat(path.c_str(), &named) != 0 ||
      !S_ISDIR(named.st_mode) || directoryStamp(current) != stamp || directoryStamp(named) != stamp) {
    error = "Directory changed during traversal"; return false;
  }
#endif
  return keepGoing(check, error);
}
}
