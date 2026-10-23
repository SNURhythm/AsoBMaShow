#pragma once

#include <array>
#include <filesystem>
#include <span>
#include <system_error>

#if !defined(_WIN32)
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace android_file {

inline std::array<std::filesystem::path, 2> publicationRoots(
    int androidSdk, const std::filesystem::path &external,
    const std::filesystem::path &internal) {
  // Before Android 11, storage-authorized apps can mutate another app's
  // external directory without taking our monitor. Unknown SDKs fail closed.
  return {androidSdk >= 30 ? external : std::filesystem::path{}, internal};
}

// Android's emulated external storage can reject both hard links and
// RENAME_NOREPLACE. Only app-owned containers can use this fallback: every
// native publisher and DocumentsProvider mutation must hold the same monitor.
// Pin the parent directories and publish with one rename, so journal recovery
// still sees a missing or complete file with its original inode identity.
inline bool renameUnderMutationLock(const std::filesystem::path &from,
                                    const std::filesystem::path &to,
                                    std::span<const std::filesystem::path> roots,
                                    std::error_code &error) {
  namespace fs = std::filesystem;
  error.clear();
  const auto safeLeaf = [](const fs::path &path) {
    const auto leaf = path.filename();
    return path.is_absolute() && !leaf.empty() && leaf != "." && leaf != "..";
  };
  if (!safeLeaf(from) || !safeLeaf(to)) {
    error = std::make_error_code(std::errc::invalid_argument);
    return false;
  }
  const auto sourceParent = fs::canonical(from.parent_path(), error);
  if (error) return false;
  const auto destinationParent = fs::canonical(to.parent_path(), error);
  if (error) return false;
  const auto owned = [&](const fs::path &parent) {
    for (const auto &root : roots) {
      if (!root.is_absolute()) continue;
      std::error_code ec;
      const auto canonicalRoot = fs::canonical(root, ec);
      if (ec) continue;
      const auto relative = parent.lexically_relative(canonicalRoot);
      if (!relative.empty() && *relative.begin() != "..") return true;
    }
    return false;
  };
  if (!owned(sourceParent) || !owned(destinationParent)) {
    error = std::make_error_code(std::errc::operation_not_supported);
    return false;
  }
  struct Directory {
    int fd;
    ~Directory() { if (fd >= 0) ::close(fd); }
  } source{::open(sourceParent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)},
    destination{::open(destinationParent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
  if (source.fd < 0 || destination.fd < 0) {
    error = std::error_code(errno, std::generic_category());
    return false;
  }
  struct stat info{};
  if (::fstatat(source.fd, from.filename().c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
    error = std::error_code(errno, std::generic_category());
    return false;
  }
  if (!S_ISREG(info.st_mode) || info.st_nlink != 1) {
    error = std::make_error_code(std::errc::operation_not_supported);
    return false;
  }
  if (::fstatat(destination.fd, to.filename().c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0) {
    error = std::make_error_code(std::errc::file_exists);
    return false;
  }
  if (errno != ENOENT ||
      ::renameat(source.fd, from.filename().c_str(), destination.fd, to.filename().c_str()) != 0) {
    error = std::error_code(errno, std::generic_category());
    return false;
  }
  return true;
}

#if defined(__ANDROID__)
// Acquires the JVM DocumentsProvider monitor and supplies only OS-owned roots.
// Callers must sync the affected directories after success.
bool renameNoReplace(const std::filesystem::path &from,
                     const std::filesystem::path &to, std::error_code &error);
#endif

} // namespace android_file
#endif
