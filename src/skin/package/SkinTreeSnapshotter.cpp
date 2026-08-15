#include "SkinTreeSnapshotter.h"

#include "../../FileChecksum.h"
#include "SkinPathPolicy.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <map>
#include <set>
#include <span>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <linux/fs.h>
#include <sys/syscall.h>
#endif
#endif

#include "SkinIOSFileOpenCompatibility.h"

namespace skin {
namespace {

namespace fs = std::filesystem;

struct FileMetadata {
  std::uint64_t device = 0;
  std::uint64_t inode = 0;
  std::uint64_t size = 0;
  std::uint64_t links = 0;
  std::int64_t modifiedSeconds = 0;
  std::int64_t modifiedNanoseconds = 0;
  std::uint32_t permissions = 0;
  bool directory = false;

  auto operator<=>(const FileMetadata &) const = default;
};

struct InventoryEntry {
  std::string normalizedPath;
  std::string collisionKey;
  fs::path sourcePath;
  FileMetadata metadata;

  bool operator==(const InventoryEntry &other) const {
    return normalizedPath == other.normalizedPath &&
           collisionKey == other.collisionKey && metadata == other.metadata;
  }
};

struct Inventory {
  fs::path rootPath;
  FileMetadata rootMetadata;
  bool externallyPinnedRoot = false;
  std::vector<InventoryEntry> entries;
  std::uint64_t fileCount = 0;
  std::uint64_t totalBytes = 0;
};

SkinDiagnostic diagnostic(std::string code, std::string message,
                          std::string virtualPath = {}) {
  return {.code = std::move(code),
          .message = std::move(message),
          .virtualPath = std::move(virtualPath),
          .severity = DiagnosticSeverity::Error};
}

std::string utf8Path(const fs::path &path) {
  const auto value = path.generic_u8string();
  return std::string(reinterpret_cast<const char *>(value.data()),
                     value.size());
}

bool report(const SkinProgressCallback &callback, SkinProgress progress,
            std::vector<SkinDiagnostic> &diagnostics) {
  if (!callback) {
    return true;
  }
  try {
    callback(progress);
    return true;
  } catch (...) {
    diagnostics.push_back(
        diagnostic("skin_snapshot_progress_callback_failed",
                   "skin snapshot progress callback raised an exception"));
    return false;
  }
}

bool injectedFailure(
    const std::shared_ptr<const SkinSnapshotFailureInjector> &failures,
    SkinSnapshotIoOperation operation, const fs::path &path) {
  return failures && failures->shouldFail(operation, path);
}

#if defined(_WIN32)
class UniqueHandle {
public:
  UniqueHandle() = default;
  explicit UniqueHandle(HANDLE value) noexcept : value_(value) {}
  UniqueHandle(const UniqueHandle &) = delete;
  UniqueHandle &operator=(const UniqueHandle &) = delete;
  UniqueHandle(UniqueHandle &&other) noexcept
      : value_(std::exchange(other.value_, INVALID_HANDLE_VALUE)) {}
  UniqueHandle &operator=(UniqueHandle &&other) noexcept {
    if (this != &other) {
      reset(std::exchange(other.value_, INVALID_HANDLE_VALUE));
    }
    return *this;
  }
  ~UniqueHandle() { reset(); }

  [[nodiscard]] HANDLE get() const noexcept { return value_; }
  [[nodiscard]] explicit operator bool() const noexcept {
    return value_ != INVALID_HANDLE_VALUE && value_ != nullptr;
  }

private:
  void reset(HANDLE value = INVALID_HANDLE_VALUE) noexcept {
    if (*this) {
      CloseHandle(value_);
    }
    value_ = value;
  }

  HANDLE value_ = INVALID_HANDLE_VALUE;
};

bool metadataFromHandle(HANDLE handle, FileMetadata &metadata,
                        std::error_code &error) {
  FILE_ATTRIBUTE_TAG_INFO tagInfo{};
  BY_HANDLE_FILE_INFORMATION information{};
  if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tagInfo,
                                    sizeof(tagInfo)) ||
      !GetFileInformationByHandle(handle, &information)) {
    error = std::error_code(static_cast<int>(GetLastError()),
                            std::system_category());
    return false;
  }
  if ((tagInfo.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
      GetFileType(handle) != FILE_TYPE_DISK) {
    error = std::make_error_code(std::errc::invalid_argument);
    return false;
  }

  metadata.device = information.dwVolumeSerialNumber;
  metadata.inode =
      (static_cast<std::uint64_t>(information.nFileIndexHigh) << 32U) |
      static_cast<std::uint64_t>(information.nFileIndexLow);
  metadata.links = information.nNumberOfLinks;
  metadata.directory = (tagInfo.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  metadata.size =
      metadata.directory
          ? 0
          : (static_cast<std::uint64_t>(information.nFileSizeHigh) << 32U) |
                static_cast<std::uint64_t>(information.nFileSizeLow);
  metadata.modifiedSeconds = information.ftLastWriteTime.dwHighDateTime;
  metadata.modifiedNanoseconds = information.ftLastWriteTime.dwLowDateTime;
  metadata.permissions =
      (tagInfo.FileAttributes & FILE_ATTRIBUTE_READONLY) != 0 ? 0 : 0222;
  error.clear();
  return true;
}

struct WindowsHandleWalk {
  fs::path currentPath;
  std::vector<UniqueHandle> handles;

  [[nodiscard]] HANDLE leaf() const noexcept {
    return handles.empty() ? INVALID_HANDLE_VALUE : handles.back().get();
  }
};

bool appendWindowsPathComponent(WindowsHandleWalk &walk,
                                const fs::path &component, DWORD desiredAccess,
                                std::optional<bool> expectedDirectory,
                                DWORD disposition, std::error_code &error,
                                bool shareDelete = false) {
  if (component.empty() || component == fs::path(L".") ||
      component == fs::path(L"..")) {
    error = std::make_error_code(std::errc::invalid_argument);
    return false;
  }
  // CreateFileW has no documented root-handle-relative form. Keep every
  // already-opened component alive without delete sharing so its name
  // cannot be replaced by a reparse point before the child handle is opened.
  walk.currentPath /= component;
  DWORD flags = FILE_FLAG_OPEN_REPARSE_POINT;
  if (!expectedDirectory || *expectedDirectory) {
    flags |= FILE_FLAG_BACKUP_SEMANTICS;
  }
  if (expectedDirectory && !*expectedDirectory &&
      disposition == OPEN_EXISTING) {
    flags |= FILE_FLAG_SEQUENTIAL_SCAN;
  }
  if (disposition != OPEN_EXISTING) {
    flags |= FILE_ATTRIBUTE_NORMAL;
  }
  const DWORD shareMode = FILE_SHARE_READ | FILE_SHARE_WRITE |
                          (shareDelete ? FILE_SHARE_DELETE : 0);
  UniqueHandle handle(CreateFileW(walk.currentPath.c_str(),
                                  desiredAccess | FILE_READ_ATTRIBUTES,
                                  shareMode, nullptr, disposition, flags,
                                  nullptr));
  if (!handle) {
    error = std::error_code(static_cast<int>(GetLastError()),
                            std::system_category());
    return false;
  }
  FileMetadata metadata;
  if (!metadataFromHandle(handle.get(), metadata, error) ||
      (expectedDirectory && metadata.directory != *expectedDirectory)) {
    if (!error) {
      error = std::make_error_code(std::errc::invalid_argument);
    }
    return false;
  }
  walk.handles.push_back(std::move(handle));
  return true;
}

std::optional<WindowsHandleWalk>
openWindowsPathNoFollow(const fs::path &path, DWORD desiredAccess,
                        std::optional<bool> expectedDirectory,
                        DWORD disposition, std::error_code &error,
                        const fs::path *deleteSharedRoot = nullptr) {
  fs::path absolute = fs::absolute(path, error);
  if (error) {
    return std::nullopt;
  }
  absolute = absolute.lexically_normal();
  const fs::path root = absolute.root_path();
  if (root.empty()) {
    error = std::make_error_code(std::errc::invalid_argument);
    return std::nullopt;
  }

  WindowsHandleWalk walk{.currentPath = root};
  UniqueHandle rootHandle(CreateFileW(
      root.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (!rootHandle) {
    error = std::error_code(static_cast<int>(GetLastError()),
                            std::system_category());
    return std::nullopt;
  }
  FileMetadata rootMetadata;
  if (!metadataFromHandle(rootHandle.get(), rootMetadata, error)) {
    return std::nullopt;
  }
  if (!rootMetadata.directory) {
    error = std::make_error_code(std::errc::invalid_argument);
    return std::nullopt;
  }
  walk.handles.push_back(std::move(rootHandle));

  const fs::path relative = absolute.lexically_relative(root);
  const fs::path normalizedDeleteSharedRoot =
      deleteSharedRoot == nullptr ? fs::path{}
                                  : deleteSharedRoot->lexically_normal();
  auto iterator = relative.begin();
  while (iterator != relative.end()) {
    const fs::path component = *iterator;
    ++iterator;
    const bool final = iterator == relative.end();
    const bool shareDeleteForPinnedRoot =
        deleteSharedRoot != nullptr &&
        (walk.currentPath / component).lexically_normal() ==
            normalizedDeleteSharedRoot;
    if (!appendWindowsPathComponent(
            walk, component,
            final ? desiredAccess : FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
            final ? expectedDirectory : std::optional<bool>(true),
            final ? disposition : OPEN_EXISTING, error,
            shareDeleteForPinnedRoot)) {
      return std::nullopt;
    }
  }
  if (relative.empty()) {
    if (expectedDirectory && !*expectedDirectory) {
      error = std::make_error_code(std::errc::invalid_argument);
      return std::nullopt;
    }
  } else if (!expectedDirectory) {
    FileMetadata metadata;
    if (!metadataFromHandle(walk.leaf(), metadata, error)) {
      return std::nullopt;
    }
  }
  error.clear();
  return walk;
}

bool readMetadata(const fs::path &path, FileMetadata &metadata,
                  std::error_code &error,
                  const fs::path *deleteSharedRoot = nullptr) {
  auto walk = openWindowsPathNoFollow(path, FILE_READ_ATTRIBUTES, std::nullopt,
                                      OPEN_EXISTING, error, deleteSharedRoot);
  return walk && metadataFromHandle(walk->leaf(), metadata, error);
}
#else
bool readMetadata(const fs::path &path, FileMetadata &metadata,
                  std::error_code &error, const fs::path * = nullptr) {
  struct stat status{};
  if (::lstat(path.c_str(), &status) != 0) {
    error = std::error_code(errno, std::generic_category());
    return false;
  }
  if (!S_ISDIR(status.st_mode) && !S_ISREG(status.st_mode)) {
    error = std::make_error_code(std::errc::invalid_argument);
    return false;
  }
  metadata.device = static_cast<std::uint64_t>(status.st_dev);
  metadata.inode = static_cast<std::uint64_t>(status.st_ino);
  metadata.size =
      S_ISREG(status.st_mode) ? static_cast<std::uint64_t>(status.st_size) : 0;
  metadata.links = static_cast<std::uint64_t>(status.st_nlink);
  metadata.permissions = static_cast<std::uint32_t>(status.st_mode & 07777);
  metadata.directory = S_ISDIR(status.st_mode);
#if defined(__APPLE__)
  metadata.modifiedSeconds = status.st_mtimespec.tv_sec;
  metadata.modifiedNanoseconds = status.st_mtimespec.tv_nsec;
#else
  metadata.modifiedSeconds = status.st_mtim.tv_sec;
  metadata.modifiedNanoseconds = status.st_mtim.tv_nsec;
#endif
  return true;
}
#endif

bool inspectTree(const fs::path &directory, const fs::path &root,
                 const SkinPackageId &package, const SkinAliasDetector &aliases,
                 Inventory &inventory, std::map<std::string, bool> &identities,
                 std::stop_token stop,
                 std::vector<SkinDiagnostic> &diagnostics,
                 bool externallyPinnedRoot) {
#if defined(_WIN32)
  std::error_code directoryHandleError;
  auto directoryHandle = openWindowsPathNoFollow(
      directory, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, true,
      OPEN_EXISTING, directoryHandleError,
      externallyPinnedRoot ? &root : nullptr);
  if (!directoryHandle) {
    diagnostics.push_back(diagnostic(
        "skin_snapshot_directory_read_failed",
        "unable to open the skin source directory without following links"));
    return false;
  }
#endif
  std::error_code iteratorError;
  fs::directory_iterator iterator(directory, iteratorError);
  if (iteratorError) {
    diagnostics.push_back(
        diagnostic("skin_snapshot_directory_read_failed",
                   "unable to enumerate the skin source directory"));
    return false;
  }
  for (const fs::directory_entry &entry : iterator) {
    if (stop.stop_requested()) {
      return false;
    }
    const fs::path path = entry.path();
    const fs::path relative = path.lexically_relative(root);
    const std::string authoredRelative = utf8Path(relative);
    const auto normalized = normalizeEntryPath(package, authoredRelative);
    if (!normalized.entry) {
      diagnostics.push_back(diagnostic("skin_snapshot_path_invalid",
                                       normalized.error, authoredRelative));
      return false;
    }

    const SkinRejectedLinkKind rejected = aliases.inspectNoFollow(path);
    if (rejected != SkinRejectedLinkKind::None) {
      diagnostics.push_back(diagnostic(
          "skin_snapshot_link_rejected",
          "skin source contains a link, alias, reparse point, or special node",
          normalized.entry->packageRelativePath));
      return false;
    }

    std::error_code metadataError;
    FileMetadata metadata;
    if (!readMetadata(path, metadata, metadataError,
                      externallyPinnedRoot ? &root : nullptr)) {
      diagnostics.push_back(
          diagnostic("skin_snapshot_nonregular_rejected",
                     "skin source contains an unreadable or non-regular node",
                     normalized.entry->packageRelativePath));
      return false;
    }
    if (!metadata.directory && metadata.links != 1) {
      diagnostics.push_back(
          diagnostic("skin_snapshot_hard_link_rejected",
                     "skin source contains a multiply-linked regular file",
                     normalized.entry->packageRelativePath));
      return false;
    }
    const auto [identity, inserted] =
        identities.emplace(normalized.entry->collisionKey, metadata.directory);
    if (!inserted) {
      diagnostics.push_back(diagnostic(
          "skin_snapshot_path_collision",
          identity->second == metadata.directory
              ? "skin source contains duplicate normalized paths"
              : "skin source contains a file/directory path collision",
          normalized.entry->packageRelativePath));
      return false;
    }
    inventory.entries.push_back(
        {.normalizedPath = normalized.entry->packageRelativePath,
         .collisionKey = normalized.entry->collisionKey,
         .sourcePath = path,
         .metadata = metadata});
    if (metadata.directory) {
      if (!inspectTree(path, root, package, aliases, inventory, identities,
                       stop, diagnostics, externallyPinnedRoot)) {
        return false;
      }
      continue;
    }
    if (metadata.size > SkinPackagePolicy::maxRegularFileBytes ||
        inventory.totalBytes >
            SkinPackagePolicy::maxExpandedBytes - metadata.size) {
      diagnostics.push_back(diagnostic(
          "skin_snapshot_size_limit",
          "skin source exceeds the regular-file or expanded-size limit",
          normalized.entry->packageRelativePath));
      return false;
    }
    ++inventory.fileCount;
    inventory.totalBytes += metadata.size;
    if (inventory.fileCount > SkinPackagePolicy::maxFiles) {
      diagnostics.push_back(diagnostic("skin_snapshot_file_count_limit",
                                       "skin source exceeds the file limit"));
      return false;
    }
  }
  return true;
}

std::optional<Inventory>
inventoryTree(const fs::path &root, const SkinPackageId &package,
              const SkinAliasDetector &aliases, std::stop_token stop,
              std::vector<SkinDiagnostic> &diagnostics,
              bool externallyPinnedRoot) {
  if (aliases.inspectNoFollow(root) != SkinRejectedLinkKind::None) {
    diagnostics.push_back(diagnostic(
        "skin_snapshot_root_rejected",
        "skin source root is a link, alias, reparse point, or special node"));
    return std::nullopt;
  }
  FileMetadata rootMetadata;
  std::error_code rootError;
  if (!readMetadata(root, rootMetadata, rootError,
                    externallyPinnedRoot ? &root : nullptr) ||
      !rootMetadata.directory) {
    diagnostics.push_back(diagnostic("skin_snapshot_root_invalid",
                                     "skin source root is not a directory"));
    return std::nullopt;
  }
  Inventory inventory;
  inventory.rootPath = root;
  inventory.rootMetadata = rootMetadata;
  inventory.externallyPinnedRoot = externallyPinnedRoot;
  std::map<std::string, bool> identities;
  if (!inspectTree(root, root, package, aliases, inventory, identities, stop,
                   diagnostics, externallyPinnedRoot)) {
    return std::nullopt;
  }
  std::sort(inventory.entries.begin(), inventory.entries.end(),
            [](const InventoryEntry &left, const InventoryEntry &right) {
              return left.normalizedPath < right.normalizedPath;
            });
  return inventory;
}

template <typename Integer>
void hashBigEndian(file_checksum::Sha256 &hash, Integer value) {
  std::array<std::byte, sizeof(Integer)> bytes{};
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    bytes[bytes.size() - index - 1] =
        static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
  }
  hash.update(bytes);
}

void hashText(file_checksum::Sha256 &hash, std::string_view text) {
  hash.update(std::as_bytes(std::span(text.data(), text.size())));
}

bool metadataMatchesOpenFile(
#if defined(_WIN32)
    HANDLE handle,
#else
    int descriptor,
#endif
    const FileMetadata &expected) {
#if defined(_WIN32)
  FileMetadata actual;
  std::error_code error;
  return metadataFromHandle(handle, actual, error) && actual == expected;
#else
  struct stat status{};
  if (::fstat(descriptor, &status) != 0) {
    return false;
  }
  FileMetadata actual;
  actual.device = static_cast<std::uint64_t>(status.st_dev);
  actual.inode = static_cast<std::uint64_t>(status.st_ino);
  actual.size =
      S_ISREG(status.st_mode) ? static_cast<std::uint64_t>(status.st_size) : 0;
  actual.links = static_cast<std::uint64_t>(status.st_nlink);
  actual.permissions = static_cast<std::uint32_t>(status.st_mode & 07777);
  actual.directory = S_ISDIR(status.st_mode);
#if defined(__APPLE__)
  actual.modifiedSeconds = status.st_mtimespec.tv_sec;
  actual.modifiedNanoseconds = status.st_mtimespec.tv_nsec;
#else
  actual.modifiedSeconds = status.st_mtim.tv_sec;
  actual.modifiedNanoseconds = status.st_mtim.tv_nsec;
#endif
  return actual == expected;
#endif
}

#if defined(_WIN32)
std::optional<WindowsHandleWalk>
openInventoryFileNoFollow(const Inventory &inventory,
                          const InventoryEntry &entry) {
  std::error_code error;
  auto walk = openWindowsPathNoFollow(
      inventory.rootPath, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, true,
      OPEN_EXISTING, error,
      inventory.externallyPinnedRoot ? &inventory.rootPath : nullptr);
  if (!walk || !metadataMatchesOpenFile(walk->leaf(), inventory.rootMetadata)) {
    return std::nullopt;
  }

  const fs::path relative =
      entry.sourcePath.lexically_relative(inventory.rootPath);
  if (relative.empty()) {
    return std::nullopt;
  }
  auto iterator = relative.begin();
  while (iterator != relative.end()) {
    const fs::path component = *iterator;
    ++iterator;
    const bool final = iterator == relative.end();
    if (!appendWindowsPathComponent(*walk, component,
                                    final ? GENERIC_READ | FILE_READ_ATTRIBUTES
                                          : FILE_LIST_DIRECTORY |
                                                FILE_READ_ATTRIBUTES,
                                    !final, OPEN_EXISTING, error)) {
      return std::nullopt;
    }
  }
  return walk;
}

bool directoryFlushUnsupported(DWORD error) {
  return error == ERROR_ACCESS_DENIED || error == ERROR_INVALID_FUNCTION ||
         error == ERROR_INVALID_HANDLE || error == ERROR_NOT_SUPPORTED;
}

bool fsyncDirectory(const fs::path &directory) {
  std::error_code error;
  auto walk = openWindowsPathNoFollow(
      directory, GENERIC_READ | GENERIC_WRITE | FILE_READ_ATTRIBUTES, true,
      OPEN_EXISTING, error);
  if (!walk && error.value() == ERROR_ACCESS_DENIED) {
    error.clear();
    walk = openWindowsPathNoFollow(directory, FILE_READ_ATTRIBUTES, true,
                                   OPEN_EXISTING, error);
  }
  if (!walk) {
    return false;
  }
  if (FlushFileBuffers(walk->leaf())) {
    return true;
  }
  return directoryFlushUnsupported(GetLastError());
}

bool fsyncDirectoryTree(const fs::path &root) {
  std::vector<fs::path> directories{root};
  std::error_code error;
  for (fs::recursive_directory_iterator iterator(root, error), end;
       !error && iterator != end; ++iterator) {
    if (iterator->is_directory(error)) {
      directories.push_back(iterator->path());
    }
  }
  if (error) {
    return false;
  }
  for (auto iterator = directories.rbegin(); iterator != directories.rend();
       ++iterator) {
    if (!fsyncDirectory(*iterator)) {
      return false;
    }
  }
  return true;
}

bool publishDirectoryAtomically(const fs::path &from, const fs::path &to,
                                std::error_code &error) {
  if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) {
    error.clear();
    return true;
  }
  error =
      std::error_code(static_cast<int>(GetLastError()), std::system_category());
  return false;
}

bool setWindowsReadOnlyNoFollow(const fs::path &path, bool readOnly,
                                std::error_code &error) {
  auto walk = openWindowsPathNoFollow(
      path, FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES, std::nullopt,
      OPEN_EXISTING, error);
  if (!walk) {
    return false;
  }
  FILE_BASIC_INFO information{};
  if (!GetFileInformationByHandleEx(walk->leaf(), FileBasicInfo, &information,
                                    sizeof(information))) {
    error = std::error_code(static_cast<int>(GetLastError()),
                            std::system_category());
    return false;
  }
  if (readOnly) {
    information.FileAttributes |= FILE_ATTRIBUTE_READONLY;
  } else {
    information.FileAttributes &= ~FILE_ATTRIBUTE_READONLY;
  }
  if (!SetFileInformationByHandle(walk->leaf(), FileBasicInfo, &information,
                                  sizeof(information))) {
    error = std::error_code(static_cast<int>(GetLastError()),
                            std::system_category());
    return false;
  }
  error.clear();
  return true;
}

bool makeTreeImmutableNoFollow(const fs::path &root, std::error_code &error) {
  std::vector<fs::path> paths;
  for (fs::recursive_directory_iterator iterator(root, error), end;
       !error && iterator != end; ++iterator) {
    paths.push_back(iterator->path());
  }
  if (error) {
    return false;
  }
  for (const fs::path &path : paths) {
    if (!setWindowsReadOnlyNoFollow(path, true, error)) {
      return false;
    }
  }
  return setWindowsReadOnlyNoFollow(root, true, error) &&
         fsyncDirectoryTree(root);
}

bool makeTreeWritableForCleanup(const fs::path &root) {
  std::error_code error;
  if (!setWindowsReadOnlyNoFollow(root, false, error)) {
    return false;
  }
  for (fs::recursive_directory_iterator iterator(root, error), end;
       !error && iterator != end; ++iterator) {
    if (!setWindowsReadOnlyNoFollow(iterator->path(), false, error)) {
      return false;
    }
  }
  return !error;
}

bool setRootPermissionsNoFollow(const fs::path &root, fs::perms permissions,
                                std::error_code &error) {
  const bool writable =
      (permissions & fs::perms::owner_write) != fs::perms::none;
  return setWindowsReadOnlyNoFollow(root, !writable, error);
}

bool renameNoReplace(const fs::path &source, const fs::path &destination,
                     std::error_code &error) {
  return publishDirectoryAtomically(source, destination, error);
}

bool inspectDestinationNoFollow(const fs::path &path, bool &exists,
                                bool &directory, std::error_code &error) {
  auto walk = openWindowsPathNoFollow(path, FILE_READ_ATTRIBUTES, std::nullopt,
                                      OPEN_EXISTING, error);
  if (!walk && (error.value() == ERROR_FILE_NOT_FOUND ||
                error.value() == ERROR_PATH_NOT_FOUND)) {
    exists = false;
    directory = false;
    error.clear();
    return true;
  }
  FileMetadata metadata;
  if (!walk || !metadataFromHandle(walk->leaf(), metadata, error)) {
    return false;
  }
  exists = true;
  directory = metadata.directory;
  return true;
}
#else
bool fsyncDirectory(const fs::path &directory) {
  const int descriptor =
      ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
  if (descriptor < 0) {
    return false;
  }
  const bool ok = ::fsync(descriptor) == 0;
  ::close(descriptor);
  return ok;
}

bool fsyncDirectoryTree(const fs::path &root) {
  std::vector<fs::path> directories{root};
  std::error_code error;
  for (fs::recursive_directory_iterator iterator(root, error), end;
       !error && iterator != end; ++iterator) {
    if (iterator->is_directory(error)) {
      directories.push_back(iterator->path());
    }
  }
  if (error) {
    return false;
  }
  for (auto iterator = directories.rbegin(); iterator != directories.rend();
       ++iterator) {
    if (!fsyncDirectory(*iterator)) {
      return false;
    }
  }
  return true;
}

int openInventoryFileNoFollow(const Inventory &inventory,
                              std::string_view relativePath) {
  int parent =
      ::open(inventory.rootPath.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
  if (parent < 0 || !metadataMatchesOpenFile(parent, inventory.rootMetadata)) {
    if (parent >= 0) {
      ::close(parent);
    }
    return -1;
  }

  std::size_t componentStart = 0;
  while (true) {
    const std::size_t separator = relativePath.find('/', componentStart);
    const std::string component(
        relativePath.substr(componentStart, separator == std::string_view::npos
                                                ? std::string_view::npos
                                                : separator - componentStart));
    if (separator == std::string_view::npos) {
      const int file = ::openat(parent, component.c_str(),
                                O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
      ::close(parent);
      return file;
    }
    const int next = ::openat(parent, component.c_str(),
                              O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    ::close(parent);
    if (next < 0) {
      return -1;
    }
    parent = next;
    componentStart = separator + 1;
  }
}
bool freezeDirectoryDescriptor(int descriptor, std::error_code &error) {
  const int duplicate = ::dup(descriptor);
  if (duplicate < 0) {
    error = std::error_code(errno, std::generic_category());
    return false;
  }
  DIR *stream = ::fdopendir(duplicate);
  if (stream == nullptr) {
    ::close(duplicate);
    error = std::error_code(errno, std::generic_category());
    return false;
  }
  errno = 0;
  while (const dirent *entry = ::readdir(stream)) {
    const std::string_view name(entry->d_name);
    if (name == "." || name == "..") {
      continue;
    }
    struct stat status{};
    if (::fstatat(descriptor, entry->d_name, &status, AT_SYMLINK_NOFOLLOW) !=
        0) {
      error = std::error_code(errno, std::generic_category());
      ::closedir(stream);
      return false;
    }
    if (S_ISDIR(status.st_mode)) {
      const int child = ::openat(descriptor, entry->d_name,
                                 O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
      if (child < 0 || !freezeDirectoryDescriptor(child, error)) {
        if (child >= 0) {
          ::close(child);
        }
        if (!error) {
          error = std::error_code(errno, std::generic_category());
        }
        ::closedir(stream);
        return false;
      }
      ::close(child);
      continue;
    }
    if (!S_ISREG(status.st_mode) || status.st_nlink != 1) {
      error = std::make_error_code(std::errc::operation_not_permitted);
      ::closedir(stream);
      return false;
    }
    const int file =
        ::openat(descriptor, entry->d_name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    struct stat openedStatus{};
    if (file < 0 || ::fstat(file, &openedStatus) != 0 ||
        !S_ISREG(openedStatus.st_mode) || openedStatus.st_nlink != 1 ||
        openedStatus.st_dev != status.st_dev ||
        openedStatus.st_ino != status.st_ino || ::fchmod(file, 0400) != 0 ||
        ::fsync(file) != 0) {
      if (file >= 0) {
        ::close(file);
      }
      error =
          std::error_code(errno == 0 ? EIO : errno, std::generic_category());
      ::closedir(stream);
      return false;
    }
    ::close(file);
  }
  if (errno != 0) {
    error = std::error_code(errno, std::generic_category());
    ::closedir(stream);
    return false;
  }
  ::closedir(stream);
  if (::fchmod(descriptor, 0500) != 0 || ::fsync(descriptor) != 0) {
    error = std::error_code(errno, std::generic_category());
    return false;
  }
  return true;
}

bool makeTreeImmutableNoFollow(const fs::path &root, std::error_code &error) {
  const int descriptor =
      ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
  if (descriptor < 0) {
    error = std::error_code(errno, std::generic_category());
    return false;
  }
  const bool frozen = freezeDirectoryDescriptor(descriptor, error);
  ::close(descriptor);
  return frozen;
}

bool makeDirectoryWritableDescriptor(int descriptor) {
  if (::fchmod(descriptor, 0700) != 0) {
    return false;
  }
  const int duplicate = ::dup(descriptor);
  DIR *stream = duplicate >= 0 ? ::fdopendir(duplicate) : nullptr;
  if (stream == nullptr) {
    if (duplicate >= 0) {
      ::close(duplicate);
    }
    return false;
  }
  bool writable = true;
  while (const dirent *entry = ::readdir(stream)) {
    const std::string_view name(entry->d_name);
    if (name == "." || name == "..") {
      continue;
    }
    struct stat status{};
    if (::fstatat(descriptor, entry->d_name, &status, AT_SYMLINK_NOFOLLOW) !=
        0) {
      writable = false;
      break;
    }
    if (S_ISDIR(status.st_mode)) {
      const int child = ::openat(descriptor, entry->d_name,
                                 O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
      if (child < 0 || ::fchmod(child, 0700) != 0) {
        if (child >= 0) {
          ::close(child);
        }
        writable = false;
        break;
      }
      if (!makeDirectoryWritableDescriptor(child)) {
        ::close(child);
        writable = false;
        break;
      }
      ::close(child);
    } else if (S_ISREG(status.st_mode)) {
      const int file = ::openat(descriptor, entry->d_name,
                                O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
      if (file < 0 || ::fchmod(file, 0600) != 0) {
        if (file >= 0) {
          ::close(file);
        }
        writable = false;
        break;
      }
      ::close(file);
    }
  }
  ::closedir(stream);
  return writable;
}

bool makeTreeWritableForCleanup(const fs::path &root) {
  const int descriptor =
      ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
  if (descriptor < 0) {
    return false;
  }
  const bool writable = makeDirectoryWritableDescriptor(descriptor);
  ::close(descriptor);
  return writable;
}

bool setRootPermissionsNoFollow(const fs::path &root, mode_t permissions,
                                std::error_code &error) {
  const int descriptor =
      ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
  if (descriptor < 0 || ::fchmod(descriptor, permissions) != 0) {
    if (descriptor >= 0) {
      ::close(descriptor);
    }
    error = std::error_code(errno, std::generic_category());
    return false;
  }
  ::close(descriptor);
  return true;
}

bool renameNoReplace(const fs::path &source, const fs::path &destination,
                     std::error_code &error) {
#if defined(__APPLE__)
  if (::renameatx_np(AT_FDCWD, source.c_str(), AT_FDCWD, destination.c_str(),
                     RENAME_EXCL) == 0) {
    return true;
  }
#elif defined(__linux__)
  if (::syscall(SYS_renameat2, AT_FDCWD, source.c_str(), AT_FDCWD,
                destination.c_str(), RENAME_NOREPLACE) == 0) {
    return true;
  }
#else
  fs::rename(source, destination, error);
  return !error;
#endif
  error = std::error_code(errno, std::generic_category());
  return false;
}

bool inspectDestinationNoFollow(const fs::path &path, bool &exists,
                                bool &directory, std::error_code &error) {
  struct stat status{};
  if (::lstat(path.c_str(), &status) == 0) {
    exists = true;
    directory = S_ISDIR(status.st_mode);
    error.clear();
    return true;
  }
  if (errno == ENOENT) {
    exists = false;
    directory = false;
    error.clear();
    return true;
  }
  error = std::error_code(errno, std::generic_category());
  return false;
}
#endif

std::optional<std::string> digestAndMaybeCopy(
    const Inventory &inventory, const std::optional<fs::path> &destination,
    SkinProgressPhase phase, std::stop_token stop,
    const SkinProgressCallback &callback,
    std::vector<SkinDiagnostic> &diagnostics,
    const std::shared_ptr<const SkinSnapshotFailureInjector> &failures = {},
    bool requireStable = true) {
  file_checksum::Sha256 hash;
  hashText(hash, "ASOBMSKIN-TREE-V1");
  const std::array<std::byte, 1> terminator{std::byte{0}};
  hash.update(terminator);
  hashBigEndian(hash, inventory.fileCount);

  std::uint64_t completedBytes = 0;
  std::uint64_t completedFiles = 0;
  std::array<char, 64 * 1024> buffer{};
  for (const InventoryEntry &entry : inventory.entries) {
    if (entry.metadata.directory) {
      if (destination) {
        std::error_code createError;
        fs::create_directories(
            *destination / pathFromUtf8(entry.normalizedPath), createError);
        if (createError) {
          diagnostics.push_back(
              diagnostic("skin_snapshot_staging_create_failed",
                         "unable to create a normalized staging directory",
                         entry.normalizedPath));
          return std::nullopt;
        }
      }
      continue;
    }
    if (stop.stop_requested()) {
      return std::nullopt;
    }
    hashBigEndian(hash,
                  static_cast<std::uint32_t>(entry.normalizedPath.size()));
    hashText(hash, entry.normalizedPath);
    hashBigEndian(hash, entry.metadata.size);

#if defined(_WIN32)
    auto input = openInventoryFileNoFollow(inventory, entry);
    if (!input || (requireStable &&
                   !metadataMatchesOpenFile(input->leaf(), entry.metadata))) {
      diagnostics.push_back(diagnostic("skin_snapshot_source_changed",
                                       "skin source changed before copying",
                                       entry.normalizedPath));
      return std::nullopt;
    }
    std::optional<WindowsHandleWalk> output;
    if (destination) {
      const fs::path target = *destination / pathFromUtf8(entry.normalizedPath);
      std::error_code createError;
      fs::create_directories(target.parent_path(), createError);
      if (!createError) {
        output = openWindowsPathNoFollow(target,
                                         GENERIC_WRITE | FILE_READ_ATTRIBUTES,
                                         false, CREATE_NEW, createError);
      }
      if (!output) {
        diagnostics.push_back(diagnostic("skin_snapshot_copy_failed",
                                         "unable to create a safe staging file",
                                         entry.normalizedPath));
        return std::nullopt;
      }
    }
    std::uint64_t readTotal = 0;
    while (true) {
      if (stop.stop_requested()) {
        return std::nullopt;
      }
      DWORD read = 0;
      if (!ReadFile(input->leaf(), buffer.data(),
                    static_cast<DWORD>(buffer.size()), &read, nullptr)) {
        diagnostics.push_back(diagnostic("skin_snapshot_copy_failed",
                                         "unable to read skin source file",
                                         entry.normalizedPath));
        return std::nullopt;
      }
      if (read == 0) {
        break;
      }
      const auto readSize = static_cast<std::size_t>(read);
      hash.update(std::as_bytes(std::span(buffer.data(), readSize)));
      if (output) {
        if (stop.stop_requested()) {
          return std::nullopt;
        }
        DWORD written = 0;
        if (!WriteFile(output->leaf(), buffer.data(), read, &written,
                       nullptr) ||
            written != read) {
          diagnostics.push_back(
              diagnostic("skin_snapshot_copy_failed",
                         "unable to write complete staging file chunk",
                         entry.normalizedPath));
          return std::nullopt;
        }
      }
      readTotal += read;
    }
    const bool stable =
        !requireStable ||
        (readTotal == entry.metadata.size &&
         metadataMatchesOpenFile(input->leaf(), entry.metadata));
    if (output &&
        (injectedFailure(failures, SkinSnapshotIoOperation::CopiedFileFsync,
                         entry.sourcePath) ||
         !FlushFileBuffers(output->leaf()))) {
      diagnostics.push_back(diagnostic("skin_snapshot_fsync_failed",
                                       "unable to synchronize staging file",
                                       entry.normalizedPath));
      return std::nullopt;
    }
    if (!stable) {
      diagnostics.push_back(diagnostic("skin_snapshot_source_changed",
                                       "skin source changed while copying",
                                       entry.normalizedPath));
      return std::nullopt;
    }
#else
    const int input =
        openInventoryFileNoFollow(inventory, entry.normalizedPath);
    if (input < 0 ||
        (requireStable && !metadataMatchesOpenFile(input, entry.metadata))) {
      if (input >= 0) {
        ::close(input);
      }
      diagnostics.push_back(diagnostic("skin_snapshot_source_changed",
                                       "skin source changed before copying",
                                       entry.normalizedPath));
      return std::nullopt;
    }
    int output = -1;
    if (destination) {
      const fs::path target = *destination / pathFromUtf8(entry.normalizedPath);
      std::error_code createError;
      fs::create_directories(target.parent_path(), createError);
      if (createError) {
        ::close(input);
        diagnostics.push_back(diagnostic("skin_snapshot_copy_failed",
                                         "unable to create staging parent",
                                         entry.normalizedPath));
        return std::nullopt;
      }
      if (injectedFailure(failures, SkinSnapshotIoOperation::CopiedFileCreate,
                          target)) {
        errno = EIO;
      } else {
        output = ::open(target.c_str(),
                        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
      }
      if (output < 0) {
        const std::error_code outputError(errno, std::generic_category());
        ::close(input);
        diagnostics.push_back(diagnostic("skin_snapshot_copy_failed",
                                         "unable to create staging file: " +
                                             outputError.message(),
                                         entry.normalizedPath));
        return std::nullopt;
      }
    }
    std::uint64_t readTotal = 0;
    while (true) {
      if (stop.stop_requested()) {
        if (output >= 0) {
          ::close(output);
        }
        ::close(input);
        return std::nullopt;
      }
      const ssize_t count = ::read(input, buffer.data(), buffer.size());
      if (count < 0) {
        if (errno == EINTR) {
          continue;
        }
        if (output >= 0) {
          ::close(output);
        }
        ::close(input);
        diagnostics.push_back(diagnostic("skin_snapshot_copy_failed",
                                         "unable to read skin source file",
                                         entry.normalizedPath));
        return std::nullopt;
      }
      if (count == 0) {
        break;
      }
      const auto countSize = static_cast<std::size_t>(count);
      hash.update(std::as_bytes(std::span(buffer.data(), countSize)));
      if (output >= 0) {
        std::size_t written = 0;
        while (written < countSize) {
          const ssize_t amount =
              ::write(output, buffer.data() + written, countSize - written);
          if (amount < 0 && errno == EINTR) {
            continue;
          }
          if (amount <= 0) {
            ::close(output);
            ::close(input);
            diagnostics.push_back(diagnostic("skin_snapshot_copy_failed",
                                             "unable to write staging file",
                                             entry.normalizedPath));
            return std::nullopt;
          }
          written += static_cast<std::size_t>(amount);
        }
      }
      readTotal += static_cast<std::uint64_t>(count);
    }
    const bool stable =
        !requireStable || (readTotal == entry.metadata.size &&
                           metadataMatchesOpenFile(input, entry.metadata));
    if (output >= 0) {
      if (injectedFailure(failures, SkinSnapshotIoOperation::CopiedFileFsync,
                          entry.sourcePath) ||
          ::fsync(output) != 0) {
        ::close(output);
        ::close(input);
        diagnostics.push_back(diagnostic("skin_snapshot_fsync_failed",
                                         "unable to synchronize staging file",
                                         entry.normalizedPath));
        return std::nullopt;
      }
      ::close(output);
    }
    ::close(input);
    if (!stable) {
      diagnostics.push_back(diagnostic("skin_snapshot_source_changed",
                                       "skin source changed while copying",
                                       entry.normalizedPath));
      return std::nullopt;
    }
#endif
    completedBytes += entry.metadata.size;
    ++completedFiles;
    if (!report(callback,
                {.phase = phase,
                 .completedBytes = completedBytes,
                 .totalBytes = inventory.totalBytes,
                 .completedFiles = completedFiles},
                diagnostics)) {
      return std::nullopt;
    }
  }
  if (destination && !fsyncDirectoryTree(*destination)) {
    diagnostics.push_back(diagnostic("skin_snapshot_fsync_failed",
                                     "unable to synchronize staging root"));
    return std::nullopt;
  }
  return hash.finalHex();
}

bool inventoryIsImmutable(const Inventory &inventory) {
  constexpr std::uint32_t writePermissions = 0222;
  if ((inventory.rootMetadata.permissions & writePermissions) != 0) {
    return false;
  }
  return std::ranges::all_of(
      inventory.entries, [](const InventoryEntry &entry) {
        return (entry.metadata.permissions & writePermissions) == 0;
      });
}

bool verifyPrivateRevisionRoot(const fs::path &root,
                               const SkinRevision &expected,
                               bool requireImmutable, std::string &error) {
  auto detector = createPlatformSkinAliasDetector();
  std::vector<SkinDiagnostic> diagnostics;
  const auto inventory =
      inventoryTree(root, expected.package, *detector, {}, diagnostics, false);
  if (!inventory || inventory->fileCount != expected.fileCount ||
      inventory->totalBytes != expected.totalBytes ||
      (requireImmutable && !inventoryIsImmutable(*inventory))) {
    error = "private skin revision tree failed no-follow verification";
    return false;
  }
  const auto digest =
      digestAndMaybeCopy(*inventory, std::nullopt,
                         SkinProgressPhase::Validating, {}, {}, diagnostics);
  if (!digest || *digest != expected.lowercaseSha256) {
    error = "private skin revision digest verification failed";
    return false;
  }
  return true;
}

std::string uniqueStagingName() {
  static std::atomic_uint64_t serial{0};
  return "snapshot-" + std::to_string(++serial);
}

} // namespace

struct SkinRevisionPin {
  SkinRevision revision;
  fs::path root;
};

SkinRevisionWeakPin::SkinRevisionWeakPin(
    std::weak_ptr<const SkinRevisionPin> pin)
    : pin_(std::move(pin)) {}

bool SkinRevisionWeakPin::hasLiveLease() const noexcept {
  return !pin_.expired();
}

struct PreparedSkinRevision::State {
  State(SkinRevision revisionValue, fs::path stagingValue,
        fs::path publishedValue,
        std::shared_ptr<const SkinSnapshotFailureInjector> failuresValue,
        bool liveSourceValue = false)
      : revision(std::move(revisionValue)),
        stagingRoot(std::move(stagingValue)),
        publishedRoot(std::move(publishedValue)),
        failures(std::move(failuresValue)), liveSource(liveSourceValue) {}

  SkinRevision revision;
  fs::path stagingRoot;
  fs::path publishedRoot;
  std::shared_ptr<const SkinSnapshotFailureInjector> failures;
  bool liveSource = false;

  ~State() {
    if (liveSource || stagingRoot.empty()) {
      return;
    }
    std::error_code ignored;
    makeTreeWritableForCleanup(stagingRoot);
    fs::remove_all(stagingRoot, ignored);
  }
};

SkinRevisionReadView::SkinRevisionReadView(const SkinRevision *revision,
                                           const fs::path *root)
    : revision_(revision), root_(root) {}

const SkinRevision &SkinRevisionReadView::revision() const noexcept {
  assert(revision_ != nullptr);
  return *revision_;
}

const fs::path &SkinRevisionReadView::root() const noexcept {
  assert(root_ != nullptr);
  return *root_;
}

SkinRevisionLease::SkinRevisionLease(std::shared_ptr<const SkinRevisionPin> pin)
    : pin_(std::move(pin)) {}

const SkinRevision &SkinRevisionLease::revision() const noexcept {
  assert(pin_ != nullptr);
  return pin_->revision;
}

const fs::path &SkinRevisionLease::root() const noexcept {
  assert(pin_ != nullptr);
  return pin_->root;
}

SkinRevisionReadView SkinRevisionLease::readView() const noexcept {
  return SkinRevisionReadView(&pin_->revision, &pin_->root);
}

SkinRevisionLease SkinRevisionLease::clone() const {
  assert(pin_ != nullptr);
  return SkinRevisionLease(pin_);
}

SkinRevisionWeakPin SkinRevisionLease::weakPin() const noexcept {
  return SkinRevisionWeakPin(pin_);
}

std::optional<SkinRevisionLease>
SkinRevisionLease::fromLiveSource(SkinRevision revision, fs::path root) {
  if (root.empty() || !root.is_absolute()) {
    return std::nullopt;
  }
  auto pin = std::make_shared<SkinRevisionPin>(SkinRevisionPin{
      .revision = std::move(revision), .root = std::move(root)});
  return SkinRevisionLease(std::move(pin));
}

PreparedSkinRevision::PreparedSkinRevision(
    SkinRevision revision, fs::path stagingRoot, fs::path publishedRoot,
    std::shared_ptr<const SkinSnapshotFailureInjector> failures)
    : state_(std::make_unique<State>(
          std::move(revision), std::move(stagingRoot), std::move(publishedRoot),
          std::move(failures))) {}

PreparedSkinRevision::PreparedSkinRevision(PreparedSkinRevision &&) noexcept =
    default;
PreparedSkinRevision &
PreparedSkinRevision::operator=(PreparedSkinRevision &&) noexcept = default;
PreparedSkinRevision::~PreparedSkinRevision() = default;

const SkinRevision &PreparedSkinRevision::revision() const noexcept {
  assert(state_ != nullptr);
  return state_->revision;
}

const fs::path &PreparedSkinRevision::stagingRoot() const noexcept {
  assert(state_ != nullptr);
  return state_->stagingRoot;
}

void PreparedSkinRevision::relocateLiveSourceTo(fs::path destination) noexcept {
  if (!state_ || !state_->liveSource || destination.empty()) {
    return;
  }
  state_->stagingRoot = std::move(destination);
  state_->publishedRoot = state_->stagingRoot;
}

SkinRevisionReadView PreparedSkinRevision::readView() const noexcept {
  return SkinRevisionReadView(&state_->revision, &state_->stagingRoot);
}

std::optional<SkinRevisionLease>
PreparedSkinRevision::publish(std::string &error) && {
  error.clear();
  if (!state_ || state_->stagingRoot.empty()) {
    error = "prepared skin revision is no longer publishable";
    return std::nullopt;
  }
  if (state_->liveSource) {
    auto lease = SkinRevisionLease::fromLiveSource(state_->revision,
                                                   state_->stagingRoot);
    if (!lease) {
      error = "live skin package source is unavailable";
    }
    return lease;
  }
  if (!verifyPrivateRevisionRoot(state_->stagingRoot, state_->revision, true,
                                 error)) {
    return std::nullopt;
  }
  std::error_code filesystemError;
  if (!makeTreeImmutableNoFollow(state_->stagingRoot, filesystemError)) {
    error =
        "unable to freeze prepared skin revision: " + filesystemError.message();
    return std::nullopt;
  }
  if (!verifyPrivateRevisionRoot(state_->stagingRoot, state_->revision, true,
                                 error)) {
    return std::nullopt;
  }

  const fs::path stagingParent = state_->stagingRoot.parent_path();
  const fs::path publishedParent = state_->publishedRoot.parent_path();
  fs::create_directories(publishedParent, filesystemError);
  if (filesystemError) {
    error =
        "unable to create private revision root: " + filesystemError.message();
    return std::nullopt;
  }

  bool destinationExists = false;
  bool destinationIsDirectory = false;
  filesystemError.clear();
  if (!inspectDestinationNoFollow(state_->publishedRoot, destinationExists,
                                  destinationIsDirectory, filesystemError)) {
    error = "unable to inspect private revision destination: " +
            filesystemError.message();
    return std::nullopt;
  }
  if (destinationExists) {
    if (!destinationIsDirectory ||
        !verifyPrivateRevisionRoot(state_->publishedRoot, state_->revision,
                                   true, error)) {
      if (error.empty()) {
        error = "private skin revision destination has an unsafe type";
      }
      return std::nullopt;
    }
    makeTreeWritableForCleanup(state_->stagingRoot);
    fs::remove_all(state_->stagingRoot, filesystemError);
    if (filesystemError) {
      error = "unable to remove duplicate prepared revision: " +
              filesystemError.message();
      return std::nullopt;
    }
    if (!fsyncDirectory(stagingParent) || !fsyncDirectory(publishedParent)) {
      error = "unable to synchronize duplicate revision publication";
      return std::nullopt;
    }
    state_->stagingRoot.clear();
  } else {
    filesystemError.clear();
    if (injectedFailure(state_->failures,
                        SkinSnapshotIoOperation::PublicationRename,
                        state_->publishedRoot)) {
      error = "injected private skin revision publication failure";
      return std::nullopt;
    }
    if (!setRootPermissionsNoFollow(state_->stagingRoot,
#if defined(_WIN32)
                                    fs::perms::owner_all,
#else
                                    0700,
#endif
                                    filesystemError)) {
      error = "unable to prepare immutable revision root for publication: " +
              filesystemError.message();
      return std::nullopt;
    }
    if (!renameNoReplace(state_->stagingRoot, state_->publishedRoot,
                         filesystemError)) {
      std::error_code ignored;
      makeTreeImmutableNoFollow(state_->stagingRoot, ignored);
      error = "unable to publish private skin revision: " +
              filesystemError.message();
      return std::nullopt;
    }
    state_->stagingRoot = state_->publishedRoot;
    filesystemError.clear();
    if (!makeTreeImmutableNoFollow(state_->publishedRoot, filesystemError)) {
      error = "unable to refreeze published skin revision: " +
              filesystemError.message();
      return std::nullopt;
    }
    if (!verifyPrivateRevisionRoot(state_->publishedRoot, state_->revision,
                                   true, error)) {
      return std::nullopt;
    }
    if (!fsyncDirectory(stagingParent) ||
        injectedFailure(state_->failures,
                        SkinSnapshotIoOperation::PublishedParentFsync,
                        publishedParent) ||
        !fsyncDirectory(publishedParent)) {
      error = "unable to synchronize private revision publication";
      return std::nullopt;
    }
    state_->stagingRoot.clear();
  }

  auto pin = std::make_shared<SkinRevisionPin>(SkinRevisionPin{
      .revision = state_->revision, .root = state_->publishedRoot});
  return SkinRevisionLease(std::move(pin));
}

SkinTreeSnapshotter::SkinTreeSnapshotter(
    SkinStorageRoots roots, const SkinAliasDetector &aliases,
    std::shared_ptr<const SkinSnapshotFailureInjector> failures)
    : roots_(std::move(roots)), aliases_(aliases),
      failures_(std::move(failures)) {}

SnapshotTreeResult SkinTreeSnapshotter::snapshot(
    const fs::path &sourceRoot, const SkinPackageId &package,
    std::stop_token stop, SkinProgressCallback callback,
    SkinSnapshotSourceRootPin sourceRootPin) {
  SnapshotTreeResult result;
  const bool externallyPinnedRoot =
      sourceRootPin == SkinSnapshotSourceRootPin::RetainedByCaller;
  if (roots_.liveSources) {
    const auto normalizedPackage = normalizePackageId(package.directoryName);
    if (!normalizedPackage.package ||
        normalizedPackage.package->collisionKey != package.collisionKey) {
      result.diagnostics.push_back(diagnostic(
          "skin_snapshot_package_invalid", "skin package identity is invalid"));
      return result;
    }
    if (sourceRoot.empty() || !sourceRoot.is_absolute() ||
        stop.stop_requested()) {
      result.cancelled = stop.stop_requested();
      if (!result.cancelled) {
        result.diagnostics.push_back(diagnostic(
            "skin_snapshot_source_invalid", "skin source root is unavailable"));
      }
      return result;
    }
    auto inventory = inventoryTree(sourceRoot, *normalizedPackage.package,
                                   aliases_, stop, result.diagnostics,
                                   externallyPinnedRoot);
    if (!inventory) {
      result.cancelled = stop.stop_requested();
      return result;
    }
    if (inventory->fileCount == 0) {
      result.diagnostics.push_back(diagnostic(
          "skin_snapshot_empty_tree", "skin source contains no regular files"));
      return result;
    }
    if (!report(callback,
                {.phase = SkinProgressPhase::Inspecting,
                 .completedBytes = inventory->totalBytes,
                 .totalBytes = inventory->totalBytes,
                 .completedFiles = inventory->fileCount},
                result.diagnostics)) {
      return result;
    }
    const auto digest = digestAndMaybeCopy(
        *inventory, std::nullopt, SkinProgressPhase::Validating, stop,
        std::move(callback), result.diagnostics, failures_, false);
    if (!digest) {
      result.cancelled = stop.stop_requested();
      return result;
    }
    SkinRevision revision{.package = *normalizedPackage.package,
                          .lowercaseSha256 = *digest,
                          .fileCount = inventory->fileCount,
                          .totalBytes = inventory->totalBytes};
    result.prepared = PreparedSkinRevision(std::move(revision), sourceRoot,
                                           sourceRoot, failures_);
    result.prepared->state_->liveSource = true;
    return result;
  }
  if (roots_.privateRevisions.empty() ||
      !roots_.privateRevisions.is_absolute()) {
    result.diagnostics.push_back(
        diagnostic("skin_snapshot_private_root_invalid",
                   "private skin revision storage is unavailable"));
    return result;
  }
  const auto normalizedPackage = normalizePackageId(package.directoryName);
  if (!normalizedPackage.package ||
      normalizedPackage.package->collisionKey != package.collisionKey) {
    result.diagnostics.push_back(diagnostic(
        "skin_snapshot_package_invalid", "skin package identity is invalid"));
    return result;
  }
  if (stop.stop_requested()) {
    result.cancelled = true;
    return result;
  }

  const SkinPackageId &canonicalPackage = *normalizedPackage.package;
  auto first = inventoryTree(sourceRoot, canonicalPackage, aliases_, stop,
                             result.diagnostics, externallyPinnedRoot);
  if (!first) {
    result.cancelled = stop.stop_requested();
    return result;
  }
  if (first->fileCount == 0) {
    result.diagnostics.push_back(diagnostic(
        "skin_snapshot_empty_tree", "skin source contains no regular files"));
    return result;
  }
  if (!report(callback,
              {.phase = SkinProgressPhase::Inspecting,
               .completedBytes = first->totalBytes,
               .totalBytes = first->totalBytes,
               .completedFiles = first->fileCount},
              result.diagnostics)) {
    return result;
  }
  if (stop.stop_requested()) {
    result.cancelled = true;
    return result;
  }

  std::error_code stagingError;
  const fs::path stagingParent = roots_.privateRevisions / ".staging";
  fs::create_directories(stagingParent, stagingError);
  const fs::path stagingRoot = stagingParent / uniqueStagingName();
  if (!stagingError) {
    fs::create_directory(stagingRoot, stagingError);
  }
  if (stagingError) {
    result.diagnostics.push_back(
        diagnostic("skin_snapshot_staging_create_failed",
                   "unable to create private skin revision staging"));
    return result;
  }
  struct StagingCleanup {
    fs::path path;
    ~StagingCleanup() {
      if (path.empty()) {
        return;
      }
      std::error_code ignored;
      makeTreeWritableForCleanup(path);
      fs::remove_all(path, ignored);
    }
  } cleanup{stagingRoot};

  const auto copiedDigest =
      digestAndMaybeCopy(*first, stagingRoot, SkinProgressPhase::Copying, stop,
                         callback, result.diagnostics, failures_);
  if (!copiedDigest) {
    result.cancelled = stop.stop_requested();
    return result;
  }
  if (!report(callback,
              {.phase = SkinProgressPhase::Validating,
               .completedBytes = 0,
               .totalBytes = first->totalBytes,
               .completedFiles = 0},
              result.diagnostics)) {
    return result;
  }
  if (stop.stop_requested()) {
    result.cancelled = true;
    return result;
  }
  auto second = inventoryTree(sourceRoot, canonicalPackage, aliases_, stop,
                              result.diagnostics, externallyPinnedRoot);
  if (!second) {
    result.cancelled = stop.stop_requested();
    return result;
  }
  if (first->rootMetadata != second->rootMetadata ||
      first->entries != second->entries ||
      first->fileCount != second->fileCount ||
      first->totalBytes != second->totalBytes) {
    result.diagnostics.push_back(
        diagnostic("skin_snapshot_source_changed",
                   "skin source changed between snapshot inventory passes"));
    return result;
  }
  const auto verifiedDigest =
      digestAndMaybeCopy(*second, std::nullopt, SkinProgressPhase::Validating,
                         stop, callback, result.diagnostics);
  if (!verifiedDigest) {
    result.cancelled = stop.stop_requested();
    return result;
  }
  if (*copiedDigest != *verifiedDigest) {
    result.diagnostics.push_back(diagnostic(
        "skin_snapshot_source_changed",
        "skin source content changed while the snapshot was copied"));
    return result;
  }

  if (!report(callback,
              {.phase = SkinProgressPhase::Publishing,
               .completedBytes = second->totalBytes,
               .totalBytes = second->totalBytes,
               .completedFiles = second->fileCount},
              result.diagnostics)) {
    return result;
  }
  if (stop.stop_requested()) {
    result.cancelled = true;
    return result;
  }
  auto finalSource = inventoryTree(sourceRoot, canonicalPackage, aliases_, stop,
                                   result.diagnostics, externallyPinnedRoot);
  if (!finalSource) {
    result.cancelled = stop.stop_requested();
    return result;
  }
  if (first->rootMetadata != finalSource->rootMetadata ||
      first->entries != finalSource->entries ||
      first->fileCount != finalSource->fileCount ||
      first->totalBytes != finalSource->totalBytes) {
    result.diagnostics.push_back(diagnostic(
        "skin_snapshot_source_changed",
        "skin source changed before the snapshot could be prepared"));
    return result;
  }
  const auto finalSourceDigest = digestAndMaybeCopy(
      *finalSource, std::nullopt, SkinProgressPhase::Validating, stop, {},
      result.diagnostics);
  if (!finalSourceDigest || *finalSourceDigest != *verifiedDigest) {
    result.cancelled = stop.stop_requested();
    if (!result.cancelled) {
      result.diagnostics.push_back(diagnostic(
          "skin_snapshot_source_changed",
          "skin source content changed before preparation completed"));
    }
    return result;
  }
  SkinRevision revision{.package = canonicalPackage,
                        .lowercaseSha256 = *verifiedDigest,
                        .fileCount = finalSource->fileCount,
                        .totalBytes = finalSource->totalBytes};
  std::string privateVerificationError;
  if (!verifyPrivateRevisionRoot(stagingRoot, revision, false,
                                 privateVerificationError)) {
    result.diagnostics.push_back(diagnostic(
        "skin_snapshot_staging_integrity_failed", privateVerificationError));
    return result;
  }
  std::error_code freezeError;
  if (!makeTreeImmutableNoFollow(stagingRoot, freezeError)) {
    result.diagnostics.push_back(
        diagnostic("skin_snapshot_staging_freeze_failed",
                   "unable to freeze the prepared skin revision"));
    return result;
  }
  if (!verifyPrivateRevisionRoot(stagingRoot, revision, true,
                                 privateVerificationError)) {
    result.diagnostics.push_back(diagnostic(
        "skin_snapshot_staging_integrity_failed", privateVerificationError));
    return result;
  }
  if (injectedFailure(failures_, SkinSnapshotIoOperation::PreparedParentFsync,
                      stagingParent) ||
      !fsyncDirectory(stagingParent)) {
    result.diagnostics.push_back(
        diagnostic("skin_snapshot_fsync_failed",
                   "unable to synchronize prepared skin revision staging"));
    return result;
  }
  const fs::path publishedRoot =
      roots_.privateRevisions / revision.lowercaseSha256;
  result.prepared = PreparedSkinRevision(std::move(revision), stagingRoot,
                                         publishedRoot, failures_);
  cleanup.path.clear();
  return result;
}

} // namespace skin
