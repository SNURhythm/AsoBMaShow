#pragma once

#if !defined(_WIN32)
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <linux/fs.h>
#include <sys/syscall.h>
#endif

namespace skin {

// The caller must serialize destination mutations. Android holds the same JVM
// monitor as its DocumentsProvider around this whole fallback. No intermediate
// destination is created, preserving the publication journal's missing-or-full
// tree recovery states on filesystems without renameat2 flags.
inline int skinRenameDirectoryUnderMutationLock(int sourceParent,
                                                const char *sourceName,
                                                int destinationParent,
                                                const char *destinationName) noexcept {
  const auto safeLeaf = [](const char *name) {
    return name && *name && std::strcmp(name, ".") != 0 &&
           std::strcmp(name, "..") != 0 && std::strchr(name, '/') == nullptr;
  };
  if (!safeLeaf(sourceName) || !safeLeaf(destinationName)) {
    errno = EINVAL;
    return -1;
  }
  struct stat source{};
  if (::fstatat(sourceParent, sourceName, &source, AT_SYMLINK_NOFOLLOW) != 0) {
    return -1;
  }
  if (!S_ISDIR(source.st_mode)) {
    errno = ENOTDIR;
    return -1;
  }
  struct stat destination{};
  if (::fstatat(destinationParent, destinationName, &destination,
                AT_SYMLINK_NOFOLLOW) == 0) {
    errno = EEXIST;
    return -1;
  }
  if (errno != ENOENT) {
    return -1;
  }
  return ::renameat(sourceParent, sourceName, destinationParent, destinationName);
}

#if defined(__ANDROID__)
int renameAndroidSkinDirectoryWithMutationLock(int sourceParent,
                                               const char *sourceName,
                                               int destinationParent,
                                               const char *destinationName) noexcept;
#endif

inline int skinRenameDirectoryNoReplace(int sourceParent, const char *sourceName,
                                         int destinationParent,
                                         const char *destinationName) noexcept {
#if defined(__APPLE__)
  return ::renameatx_np(sourceParent, sourceName, destinationParent,
                        destinationName, RENAME_EXCL);
#elif defined(__linux__)
  const int result = static_cast<int>(::syscall(
      SYS_renameat2, sourceParent, sourceName, destinationParent,
      destinationName, RENAME_NOREPLACE));
#if defined(__ANDROID__)
  if (result != 0 && (errno == EINVAL || errno == EOPNOTSUPP || errno == ENOSYS)) {
    return renameAndroidSkinDirectoryWithMutationLock(sourceParent, sourceName,
                                                        destinationParent, destinationName);
  }
#endif
  return result;
#else
  errno = ENOTSUP;
  return -1;
#endif
}

} // namespace skin
#endif
