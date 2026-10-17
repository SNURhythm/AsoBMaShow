#include "StoragePlatform.h"
#include "../AtomicFile.h"
#include "../FileChecksum.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#endif

namespace storage_management {
namespace fs = std::filesystem;
namespace {
bool createExclusive(const fs::path &path, std::string &error) {
#ifdef _WIN32
  const auto handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                  CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) { error = "Exclusive creation failed"; return false; }
  const bool ok = FlushFileBuffers(handle);
  CloseHandle(handle);
  return ok;
#else
  const int fd = open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) { error = std::strerror(errno); return false; }
  const bool ok = fsync(fd) == 0;
  if (!ok) error = std::strerror(errno);
  close(fd);
  return ok;
#endif
}

bool copyAndSync(const fs::path &from, const fs::path &to, const FileIdentity &expected,
                 Checkpoint check, std::string &error) {
  std::ifstream input(from, std::ios::binary);
  if (!input) { error = "Cannot open encoded media"; return false; }
#ifdef _WIN32
  struct OutputHandle {
    HANDLE value;
    ~OutputHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
  } output{CreateFileW(to.c_str(), GENERIC_WRITE | FILE_READ_ATTRIBUTES, 0, nullptr,
                       OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
  BY_HANDLE_FILE_INFORMATION info{};
  if (output.value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(output.value, &info) ||
      info.nNumberOfLinks != 1 || info.nFileSizeHigh != 0 || info.nFileSizeLow != 0 ||
      (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
      info.dwVolumeSerialNumber != expected.device ||
      ((std::uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow) != expected.fileId) {
    error = "Installation file identity changed"; return false;
  }
#else
  struct OutputDescriptor {
    int value;
    ~OutputDescriptor() { if (value >= 0) close(value); }
  } output{open(to.c_str(), O_WRONLY | O_CLOEXEC | O_NOFOLLOW)};
  struct stat info{};
  if (output.value < 0 || fstat(output.value, &info) != 0 || !S_ISREG(info.st_mode) ||
      info.st_nlink != 1 || info.st_size != 0 ||
      static_cast<std::uint64_t>(info.st_dev) != expected.device ||
      static_cast<std::uint64_t>(info.st_ino) != expected.fileId) {
    error = "Installation file identity changed"; return false;
  }
#endif
  std::array<char, 65536> buffer{};
  while (input) {
    if (check && !check()) { error = "Cancelled"; return false; }
    input.read(buffer.data(), buffer.size());
    std::size_t offset = 0;
    const auto bytes = static_cast<std::size_t>(input.gcount());
    while (offset < bytes) {
#ifdef _WIN32
      DWORD written = 0;
      if (!WriteFile(output.value, buffer.data() + offset, static_cast<DWORD>(bytes - offset), &written, nullptr) || written == 0) {
        error = "Media installation write failed"; return false;
      }
#else
      const auto written = write(output.value, buffer.data() + offset, bytes - offset);
      if (written < 0 && errno == EINTR) continue;
      if (written <= 0) { error = "Media installation write failed"; return false; }
#endif
      offset += static_cast<std::size_t>(written);
    }
  }
  if (input.bad()) { error = "Media copy read failed"; return false; }
#ifdef _WIN32
  const bool synced = FlushFileBuffers(output.value);
#else
  const bool synced = fsync(output.value) == 0;
#endif
  if (!synced) error = "Media installation flush failed";
  return synced;
}

bool renameNoReplace(const fs::path &from, const fs::path &to, std::string &error) {
#ifdef _WIN32
  if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) return true;
  error = "Exclusive rename failed: " + std::to_string(GetLastError());
#elif defined(__APPLE__)
  if (renamex_np(from.c_str(), to.c_str(), RENAME_EXCL) == 0) return true;
  error = std::strerror(errno);
#elif defined(__linux__) && defined(SYS_renameat2)
  // RENAME_NOREPLACE = 1. Fail closed on kernels/filesystems without support.
  if (syscall(SYS_renameat2, AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), 1) == 0) return true;
  error = std::strerror(errno);
#else
  (void)from;
  (void)to;
  error = "Atomic no-clobber rename is unavailable on this platform";
#endif
  return false;
}
} // namespace

StoragePlatformPaths MakeStoragePlatformPaths(const fs::path &privateRoot,
                                             std::vector<fs::path> allowedRoots, bool sandboxedAncestors) {
  std::vector<fs::path> accessRoots;
  if (sandboxedAncestors) {
    accessRoots = allowedRoots;
    accessRoots.push_back(privateRoot);
  }
  return {privateRoot / "storage-management/journal", privateRoot / "storage-management/temporary",
          std::move(allowedRoots), std::move(accessRoots)};
}

FileTransactionDependencies DefaultFileTransactionDependencies() {
  return {.identity = [](const fs::path &path, std::string &error) { return ReadFileIdentity(path, error); },
          .createExclusive = createExclusive,
          .copyAndSync = copyAndSync,
          .renameNoReplace = renameNoReplace,
          .remove = [](const fs::path &path, std::string &error) {
            std::error_code ec;
            fs::remove(path, ec);
            if (ec) { error = ec.message(); return false; }
            return true;
          },
          .syncDirectory = atomic_file::syncDirectory};
}

std::optional<std::string> HashStorageFile(const fs::path &path, Checkpoint check, std::string &error) {
  std::ifstream input(path, std::ios::binary);
  if (!input) { error = "Cannot open media for verification"; return {}; }
  file_checksum::Sha256 hash;
  std::array<std::byte, 65536> buffer{};
  while (input) {
    if (check && !check()) { error = "Cancelled"; return {}; }
    input.read(reinterpret_cast<char *>(buffer.data()), buffer.size());
    hash.update(std::span(buffer).first(static_cast<std::size_t>(input.gcount())));
  }
  if (input.bad()) { error = "Media verification read failed"; return {}; }
  return hash.finalHex();
}

} // namespace storage_management
