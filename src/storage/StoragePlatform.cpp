#include "StoragePlatform.h"
#include "../AtomicFile.h"
#include "../FileChecksum.h"
#include "../Uuid.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>

#ifdef _WIN32
#include <windows.h>
#include <aclapi.h>
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
StorageFileCreation createExclusive(const fs::path &path, std::string &error) {
  StorageFileCreation result;
#ifdef _WIN32
  const auto handle = CreateFileW(path.c_str(), GENERIC_WRITE | FILE_READ_ATTRIBUTES, 0, nullptr,
                                  CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) { error = "Exclusive creation failed"; return result; }
  result.created = true;
  BY_HANDLE_FILE_INFORMATION info{};
  if (GetFileInformationByHandle(handle, &info)) {
    result.identity.device = info.dwVolumeSerialNumber;
    result.identity.fileId = (std::uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
    result.identity.bytes = (std::uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    const std::uint64_t ticks = (std::uint64_t(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime;
    const auto sinceUnix = std::chrono::duration<std::int64_t, std::ratio<1, 10000000>>(
        static_cast<std::int64_t>(ticks) - 116444736000000000LL);
    result.identity.modified = std::chrono::time_point_cast<fs::file_time_type::duration>(
        std::chrono::clock_cast<fs::file_time_type::clock>(std::chrono::sys_time(sinceUnix)));
    result.ready = info.nNumberOfLinks == 1 && result.identity.bytes == 0 && result.identity.fileId != 0 &&
        !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) && FlushFileBuffers(handle);
  }
  if (!result.ready) error = "Cannot identify or flush exclusively created installation file";
  CloseHandle(handle);
#else
  const int fd = open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) { error = std::strerror(errno); return result; }
  result.created = true;
  struct stat info{};
  if (fstat(fd, &info) == 0) {
    result.identity.device = static_cast<std::uint64_t>(info.st_dev);
    result.identity.fileId = static_cast<std::uint64_t>(info.st_ino);
    result.identity.bytes = static_cast<std::uint64_t>(info.st_size);
#ifdef __APPLE__
    const auto modified = info.st_mtimespec;
#else
    const auto modified = info.st_mtim;
#endif
    const auto sinceUnix = std::chrono::seconds(modified.tv_sec) + std::chrono::nanoseconds(modified.tv_nsec);
    result.identity.modified = std::chrono::time_point_cast<fs::file_time_type::duration>(
        fs::file_time_type::clock::from_sys(std::chrono::sys_time(sinceUnix)));
    result.ready = S_ISREG(info.st_mode) && info.st_nlink == 1 && info.st_size == 0 && result.identity.fileId != 0 && fsync(fd) == 0;
  }
  if (!result.ready) error = "Cannot identify or flush exclusively created installation file";
  close(fd);
#endif
  return result;
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

bool preservePermissions(const fs::path &source, const FileIdentity &sourceIdentity,
                         const fs::path &installed, const FileIdentity &installedIdentity,
                         std::string &error) {
#ifdef _WIN32
  struct Handle {
    HANDLE value;
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
  } input{CreateFileW(source.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
                       nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)},
    output{CreateFileW(installed.c_str(), GENERIC_WRITE | FILE_READ_ATTRIBUTES | READ_CONTROL | WRITE_DAC, 0,
                        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
  const auto matches = [](HANDLE handle, const FileIdentity &identity) {
    BY_HANDLE_FILE_INFORMATION info{};
    return handle != INVALID_HANDLE_VALUE && GetFileInformationByHandle(handle, &info) &&
        info.nNumberOfLinks == 1 && !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
        info.dwVolumeSerialNumber == identity.device &&
        ((std::uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow) == identity.fileId &&
        ((std::uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow) == identity.bytes;
  };
  if (!matches(input.value, sourceIdentity) || !matches(output.value, installedIdentity)) {
    error = "File identity changed or security access denied before preserving permissions"; return false;
  }
  struct Security {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PSID owner = nullptr, group = nullptr;
    PACL dacl = nullptr;
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    ~Security() { if (descriptor) LocalFree(descriptor); }
    bool read(HANDLE handle) {
      DWORD revision = 0;
      return GetSecurityInfo(handle, SE_FILE_OBJECT,
          OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
          &owner, &group, &dacl, nullptr, &descriptor) == ERROR_SUCCESS &&
          GetSecurityDescriptorControl(descriptor, &control, &revision) && (control & SE_DACL_PRESENT) &&
          owner && group && IsValidSid(owner) && IsValidSid(group) && (!dacl || IsValidAcl(dacl));
    }
  } original, before, after;
  if (!original.read(input.value) || !before.read(output.value)) {
    error = "Cannot read source or installation access permissions"; return false;
  }
  // Owner/group affect ACL interpretation (including owner-rights ACEs). Do
  // not silently change those identities or request privilege escalation.
  if (!EqualSid(original.owner, before.owner) || !EqualSid(original.group, before.group)) {
    error = "Cannot preserve source security ownership"; return false;
  }
  const auto protection = (original.control & SE_DACL_PROTECTED) ?
      PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION;
  if (SetSecurityInfo(output.value, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | protection,
                      nullptr, nullptr, original.dacl, nullptr) != ERROR_SUCCESS || !after.read(output.value)) {
    error = "Cannot preserve source access permissions"; return false;
  }
  const auto sameAcl = [](PACL left, PACL right) {
    if (!left || !right) return left == right;
    if (left->AceCount != right->AceCount) return false;
    for (DWORD i = 0; i < left->AceCount; ++i) {
      void *a = nullptr, *b = nullptr;
      if (!GetAce(left, i, &a) || !GetAce(right, i, &b)) return false;
      const auto bytes = static_cast<ACE_HEADER *>(a)->AceSize;
      if (bytes != static_cast<ACE_HEADER *>(b)->AceSize || std::memcmp(a, b, bytes) != 0) return false;
    }
    return true;
  };
  if (!EqualSid(original.owner, after.owner) || !EqualSid(original.group, after.group) ||
      ((original.control ^ after.control) & SE_DACL_PROTECTED) || !sameAcl(original.dacl, after.dacl) ||
      !FlushFileBuffers(output.value)) {
    error = "Cannot verify or flush preserved source access permissions"; return false;
  }
  return true;
#else
  struct Descriptor {
    int value;
    ~Descriptor() { if (value >= 0) close(value); }
  } input{open(source.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)},
    output{open(installed.c_str(), O_WRONLY | O_CLOEXEC | O_NOFOLLOW)};
  const auto matches = [](int fd, const FileIdentity &identity, struct stat &info) {
    return fd >= 0 && fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_nlink == 1 &&
           static_cast<std::uint64_t>(info.st_dev) == identity.device &&
           static_cast<std::uint64_t>(info.st_ino) == identity.fileId &&
           static_cast<std::uint64_t>(info.st_size) == identity.bytes;
  };
  struct stat sourceInfo{}, installedInfo{};
  if (!matches(input.value, sourceIdentity, sourceInfo) ||
      !matches(output.value, installedIdentity, installedInfo)) {
    error = "File identity changed before preserving permissions"; return false;
  }
  // Carry access permissions, never set-user/group-ID bits, onto the new
  // inode. Keep the installation private until the full copy is verified.
  if (fchmod(output.value, sourceInfo.st_mode & 0777) != 0 || fsync(output.value) != 0) {
    error = "Cannot preserve source permissions: " + std::string(std::strerror(errno));
    return false;
  }
  return true;
#endif
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
  auto syncRoot = privateRoot.lexically_normal();
  // SDL preference paths end in a separator. Remove its empty component so
  // ancestry checks and the parent-by-parent durability barrier agree.
  while (syncRoot != syncRoot.root_path() && syncRoot.filename().empty())
    syncRoot = syncRoot.parent_path();
  std::error_code error;
  while (!syncRoot.empty() && !fs::exists(syncRoot, error) && !error) {
    const auto parent = syncRoot.parent_path();
    if (parent == syncRoot) break;
    syncRoot = parent;
  }
  return {privateRoot / "storage-management/journal", privateRoot / "storage-management/temporary",
          std::move(allowedRoots), std::move(accessRoots), privateRoot / "storage-management/scan", syncRoot};
}

bool CleanupAbandonedStorageScans(const StoragePlatformPaths &paths, std::string &error, Checkpoint check) {
  error.clear();
  try {
    const auto active = [&] {
      if (check && !check()) { error = "Scan cleanup cancelled"; return false; }
      return true;
    };
    if (!active()) return false;
    if (paths.scanRoot.empty()) return true;
    std::error_code ec;
    const auto status = fs::symlink_status(paths.scanRoot, ec);
    if (ec == std::errc::no_such_file_or_directory || (!ec && !fs::exists(status))) return true;
    if (ec) { error = ec.message(); return false; }
    if (!IsSafeLocalPath(paths.scanRoot, error, paths.pathAccessRoots)) return false;
    for (const auto &directory : fs::directory_iterator(paths.scanRoot)) {
      if (!active()) return false;
      auto name = directory.path().filename().string();
      std::string database = "review.sqlite";
      if (name.starts_with("storage-directory-")) { name.erase(0, 18); database = "directory.sqlite"; }
      else if (name.starts_with("storage-scan-")) { name.erase(0, 13); database = "frontier.sqlite"; }
      if (!uuid::isCanonicalLowerV4(name) || !fs::is_directory(directory.symlink_status())) continue;
      std::vector<std::pair<fs::path, FileIdentity>> files;
      bool owned = true;
      for (const auto &entry : fs::directory_iterator(directory.path())) {
        if (!active()) return false;
        const auto filename = entry.path().filename().string();
        if (filename != database && filename != database + "-journal" &&
            filename != database + "-wal" && filename != database + "-shm") { owned = false; break; }
        std::string ignored;
        const auto identity = ReadFileIdentity(entry.path(), ignored, paths.pathAccessRoots);
        if (!identity) { owned = false; break; }
        files.emplace_back(entry.path(), *identity);
      }
      if (!owned) continue;
      for (const auto &[path, identity] : files) {
        if (!active()) return false;
        if (ReadFileIdentity(path, error, paths.pathAccessRoots) != identity) {
          if (error.empty()) error = "Scan database changed during cleanup";
          return false;
        }
        fs::remove(path);
      }
      // Only remove the now-empty directory; never recursively delete contents.
      fs::remove(directory.path());
    }
    return true;
  } catch (const std::exception &failure) { error = failure.what(); return false; }
}

std::optional<std::vector<fs::path>> StorageReservationRoots(
    const StoragePlatformPaths &paths, std::vector<fs::path> selected, std::string &error) {
  error.clear();
  std::error_code ec;
  const auto journal = fs::symlink_status(paths.journalRoot, ec);
  if (ec == std::errc::no_such_file_or_directory || (!ec && !fs::exists(journal))) return selected;
  if (ec || !fs::is_directory(journal)) {
    error = ec ? ec.message() : "Storage journal directory is unsafe";
    return {};
  }
  const bool empty = fs::is_empty(paths.journalRoot, ec);
  if (ec) { error = ec.message(); return {}; }
  if (empty) return selected;
  // Apply recovers the global journal before processing its selection. Reserve
  // every existing authorized root so an unrelated provider writer cannot race
  // backup cleanup or archive recovery. Missing roots remain recovery's concern.
  for (const auto &root : paths.allowedRoots) {
    const auto status = fs::status(root, ec);
    if (ec == std::errc::no_such_file_or_directory || (!ec && !fs::exists(status))) continue;
    if (ec) { error = root.string() + ": " + ec.message(); return {}; }
    if (fs::is_directory(status) || fs::is_regular_file(status)) selected.push_back(root);
  }
  return selected;
}

FileTransactionDependencies DefaultFileTransactionDependencies() {
  return {.identity = [](const fs::path &path, std::string &error) { return ReadFileIdentity(path, error); },
          .createExclusive = createExclusive,
          .copyAndSync = copyAndSync,
          .preservePermissions = preservePermissions,
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
