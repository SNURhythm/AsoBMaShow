#include "ChartLibraryPlatform.h"

#include "ChartLibraryTaskService.h"
#include "../path.h"
#include "../targets.h"

#include <SDL2/SDL.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <thread>

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

struct FolderActionService::Impl {
  ChartRepository *repository = nullptr;
  chart_library_tasks::ChartLibraryTaskService *tasks = nullptr;
  std::jthread pickerThread;
  std::atomic_bool pickerActive = false;
#if TARGET_OS_ANDROID
  std::mutex pendingMutex;
  std::deque<std::pair<std::uint64_t, bool>> pendingImports;
  std::uint64_t nextPollMillis = 0;
#endif

  void enqueueFolder(const std::filesystem::path &folder,
                     const std::string &bookmark) {
    if (folder.empty() || repository == nullptr || tasks == nullptr) return;
    if (auto session = repository->OpenSession()) {
      if (!session->InsertEntry(folder, bookmark)) {
        SDL_Log("Failed to add chart folder entry %s",
                fspath_to_utf8(folder).c_str());
      }
    } else {
      SDL_Log("Failed to open chart database while adding folder %s",
              fspath_to_utf8(folder).c_str());
    }
    std::string name = folder.filename().empty()
                           ? fspath_to_utf8(folder)
                           : fspath_to_utf8(folder.filename());
    if (name.empty()) name = "Folder";
    tasks->enqueue({.kind = chart_library_tasks::TaskKind::RefreshLibrary,
                    .title = "Add Folder: " + name,
                    .folderToAdd = folder,
                    .iosBookmark = bookmark});
  }

#if TARGET_OS_ANDROID
  void enqueueImport(std::uint64_t id, const std::filesystem::path &path,
                     bool folder) {
    if (tasks == nullptr) return;
    if (path.empty()) {
      tasks->failReserved(id, "Import failed: selected path is empty.");
      return;
    }
    tasks->enqueueReserved(
        id, {.kind = chart_library_tasks::TaskKind::AndroidImport,
             .title = folder ? "Import Folder" : "Import Archive",
             .androidImportPath = path,
             .androidImportFolder = folder});
  }

  void requestImport(bool folder) {
    if (pickerActive.exchange(true)) return;
    if (pickerThread.joinable()) pickerThread.join();
    pickerThread = std::jthread(
        [this, folder](const std::stop_token &stopToken) {
          struct Reset {
            std::atomic_bool &active;
            ~Reset() { active.store(false); }
          } reset{pickerActive};
          std::filesystem::path path;
          std::string error;
          const bool picked = folder
                                  ? PickAndroidFolderForImport(path, error)
                                  : PickAndroidArchiveForImport(path, error);
          if (!picked) {
            if (!error.empty()) {
              SDL_Log("Failed to pick Android %s: %s",
                      folder ? "folder" : "archive", error.c_str());
            }
            return;
          }
          if (stopToken.stop_requested() || tasks == nullptr) return;
          const std::uint64_t id = tasks->reserve(
              folder ? "Import Folder" : "Import Archive",
              folder ? "Copying selected folder"
                     : "Copying selected archive");
          if (path.empty()) {
            std::lock_guard lock(pendingMutex);
            pendingImports.emplace_back(id, folder);
          } else {
            enqueueImport(id, path, folder);
          }
        });
  }
#endif
};

FolderActionService::FolderActionService(
    ChartRepository &repository,
    chart_library_tasks::ChartLibraryTaskService &tasks)
    : impl_(std::make_unique<Impl>()) {
  impl_->repository = &repository;
  impl_->tasks = &tasks;
}

FolderActionService::~FolderActionService() {
  if (impl_ && impl_->pickerThread.joinable()) {
    impl_->pickerThread.request_stop();
    impl_->pickerThread.join();
  }
}

void FolderActionService::requestAddFolder() {
  if (!impl_) return;
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  if (impl_->pickerActive.exchange(true)) return;
  if (impl_->pickerThread.joinable()) impl_->pickerThread.join();
  impl_->pickerThread = std::jthread(
      [state = impl_.get()](const std::stop_token &stopToken) {
        struct Reset {
          std::atomic_bool &active;
          ~Reset() { active.store(false); }
        } reset{state->pickerActive};
        std::string folder;
        std::string bookmark;
        std::string error;
        if (!PickIOSFolder(folder, bookmark, error)) {
          if (!error.empty()) {
            SDL_Log("Failed to pick iOS library folder: %s", error.c_str());
          }
          return;
        }
        if (!stopToken.stop_requested()) {
          state->enqueueFolder(std::filesystem::path(folder), bookmark);
        }
      });
#elif TARGET_OS_ANDROID
  if (AndroidBuildHasManageExternalStorage()) {
    if (impl_->pickerActive.exchange(true)) return;
    if (impl_->pickerThread.joinable()) impl_->pickerThread.join();
    impl_->pickerThread = std::jthread(
        [state = impl_.get()](const std::stop_token &stopToken) {
          struct Reset {
            std::atomic_bool &active;
            ~Reset() { active.store(false); }
          } reset{state->pickerActive};
          std::filesystem::path folder;
          std::string treeUri;
          std::string error;
          if (!PickAndroidChartFolder(folder, treeUri, error)) {
            if (!error.empty()) {
              SDL_Log("Failed to pick Android library folder: %s",
                      error.c_str());
            }
            return;
          }
          if (!stopToken.stop_requested()) {
            state->enqueueFolder(folder, treeUri);
          }
        });
  } else {
    impl_->requestImport(true);
  }
#endif
}

void FolderActionService::requestImportArchive() {
#if TARGET_OS_ANDROID
  if (impl_) impl_->requestImport(false);
#endif
}

void FolderActionService::poll() {
#if TARGET_OS_ANDROID
  if (!impl_) return;
  const std::uint64_t now = SDL_GetTicks64();
  if (now < impl_->nextPollMillis) return;
  impl_->nextPollMillis = now + 1000;
  std::string error;
  const auto path = ConsumePendingAndroidArchiveImport(error);
  if (!path && error.empty()) return;
  std::uint64_t id = 0;
  bool folder = false;
  {
    std::lock_guard lock(impl_->pendingMutex);
    if (!impl_->pendingImports.empty()) {
      id = impl_->pendingImports.front().first;
      folder = impl_->pendingImports.front().second;
      impl_->pendingImports.pop_front();
    }
  }
  if (id == 0 && impl_->tasks != nullptr) {
    id = impl_->tasks->reserve("Import Archive", "Copying shared archive");
  }
  if (path) {
    impl_->enqueueImport(id, *path, folder);
  } else if (impl_->tasks != nullptr) {
    impl_->tasks->failReserved(
        id, error.empty() ? "Import failed" : std::move(error));
  }
#endif
}

bool FolderActionService::active() const noexcept {
  if (!impl_) return false;
  if (impl_->pickerActive.load(std::memory_order_acquire)) return true;
#if TARGET_OS_ANDROID
  std::lock_guard lock(impl_->pendingMutex);
  return !impl_->pendingImports.empty();
#else
  return false;
#endif
}

} // namespace chart_library_platform
