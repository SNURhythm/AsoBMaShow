#include "ChartLibraryPlatform.h"

#include "../path.h"
#include "../targets.h"

#include <SDL2/SDL.h>

#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
#include "../iOSNatives.hpp"

#include <mutex>
#include <unordered_map>
#elif TARGET_OS_ANDROID
#include "../AndroidNatives.h"
#endif

namespace chart_library_platform {

#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
namespace {
std::mutex folderAccessMutex;
std::vector<void *> folderAccessHandles;
std::unordered_map<path_t, path_t> resolvedFolderPaths;

void clearFolderAccessLocked() {
  for (void *handle : folderAccessHandles) {
    StopIOSSecurityScopedResource(handle);
  }
  folderAccessHandles.clear();
  resolvedFolderPaths.clear();
}
} // namespace
#endif

void clearFolderAccess() {
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  std::lock_guard<std::mutex> lock(folderAccessMutex);
  clearFolderAccessLocked();
#endif
}

void refreshFolderAccess(const std::vector<ChartEntry> &entries) {
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  std::lock_guard<std::mutex> lock(folderAccessMutex);
  clearFolderAccessLocked();

  for (const auto &entry : entries) {
    if (entry.iosBookmark.empty()) {
      continue;
    }
    std::string resolvedPath;
    std::string errorMessage;
    void *handle = StartIOSSecurityScopedResource(path_t_to_utf8(entry.path),
                                                  entry.iosBookmark,
                                                  resolvedPath, errorMessage);
    if (!errorMessage.empty()) {
      SDL_Log("Failed to open folder access for %s: %s",
              path_t_to_utf8(entry.path).c_str(), errorMessage.c_str());
    }
    if (!resolvedPath.empty()) {
      resolvedFolderPaths[entry.path] = utf8_to_path_t(resolvedPath);
    }
    if (handle != nullptr) {
      folderAccessHandles.push_back(handle);
    }
  }
#else
  (void)entries;
#endif
}

std::filesystem::path resolveFolderEntryPath(const ChartEntry &entry) {
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  std::lock_guard<std::mutex> lock(folderAccessMutex);
  const auto resolved = resolvedFolderPaths.find(entry.path);
  if (resolved != resolvedFolderPaths.end()) {
    return std::filesystem::path(resolved->second);
  }
  return std::filesystem::path(entry.path);
#elif TARGET_OS_ANDROID
  std::filesystem::path root(entry.path);
  RegisterAndroidChartFolder(root, entry.iosBookmark);
  return root;
#else
  return std::filesystem::path(entry.path);
#endif
}

} // namespace chart_library_platform
