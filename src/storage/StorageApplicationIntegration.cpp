#include "StorageApplicationIntegration.h"
#include "StorageTask.h"
#include "../context.h"
#include "../scene/SceneManager.h"
#include "../scene/Scene.h"
#include "../targets.h"
#include <SDL3/SDL.h>
#if TARGET_OS_ANDROID
#include "../AndroidNatives.h"
#elif TARGET_OS_IOS || TARGET_OS_SIMULATOR
#include "../iOSNatives.hpp"
#endif

namespace storage_management {
namespace {
struct Access {
  StoragePlatformPaths paths;
  std::vector<std::shared_ptr<void>> scopes;
  std::vector<std::filesystem::path> unavailableRoots;
};
std::optional<Access> accessFolders(ApplicationContext &context, std::string &error) {
  auto session = context.chartRepository.OpenSession();
  if (!session) { error = "Chart library is unavailable."; return {}; }
  Access access;
  std::vector<std::filesystem::path> roots;
  for (const auto &entry : session->SelectAllEntries()) {
    auto path = chart_library_platform::resolveFolderEntryPath(entry);
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
    if (!entry.iosBookmark.empty()) {
      std::string resolved;
      void *handle = StartIOSSecurityScopedResource(fspath_to_utf8(path), entry.iosBookmark, resolved, error);
      if (handle) access.scopes.emplace_back(handle, [](void *value) { StopIOSSecurityScopedResource(value); });
      else access.unavailableRoots.push_back(path);
      error.clear();
      if (!resolved.empty()) path = std::filesystem::path(resolved);
    }
#endif
    // Archives and SAF entries remain visible in the browser, but do not grant
    // local transaction authority. Missing roots may contain recovery records;
    // retain their absolute names so recovery can fail closed on actual access.
    if (path.is_absolute()) roots.push_back(path.lexically_normal());
  }
  auto root = StoragePrivateRoot(context);
  if (root.empty()) { error = "Private storage is unavailable."; return {}; }
  access.paths = MakeStoragePlatformPaths(root, std::move(roots));
  return access;
}
std::vector<std::filesystem::path> selectedRoots(const OptimizationRequest &request) {
  std::vector<std::filesystem::path> roots;
  for (const auto &group : request.inventory.groups) {
    auto path = (group.chartPathFallback ? group.path.parent_path() : group.path).lexically_normal();
    if (std::any_of(roots.begin(), roots.end(), [&](const auto &root) { return IsWithinFolder(path, root); })) continue;
    std::erase_if(roots, [&](const auto &root) { return IsWithinFolder(root, path); });
    roots.push_back(std::move(path));
  }
  return roots;
}
}

std::filesystem::path StoragePrivateRoot(ApplicationContext &) {
#if TARGET_OS_ANDROID
  return GetAndroidInternalFilesDir();
#elif TARGET_OS_IOS || TARGET_OS_SIMULATOR
  return GetIOSApplicationSupportPath();
#else
  char *value = SDL_GetPrefPath("SNURhythm", "AsoBMaShow");
  std::filesystem::path path = value ? value : "";
  SDL_free(value);
  return path;
#endif
}

bool InitializeStorageServices(ApplicationContext &context) {
  std::string error;
  auto startup = accessFolders(context, error);
  if (!startup) { SDL_Log("Storage recovery: %s", error.c_str()); return false; }
#if TARGET_OS_ANDROID
  // Reserve only existing roots. Recovery itself rejects inaccessible targets.
  std::vector<std::filesystem::path> existing;
  for (const auto &root : startup->paths.allowedRoots) {
    std::error_code ec;
    if (std::filesystem::is_directory(root, ec)) existing.push_back(root);
  }
  // Registered roots can overlap; reserve their union once.
  OptimizationRequest scopeRequest;
  for (const auto &root : existing) scopeRequest.inventory.groups.push_back({.path = root});
  std::shared_ptr<void> reservation;
  std::error_code journalError;
  if (std::filesystem::exists(startup->paths.journalRoot, journalError) &&
      !std::filesystem::is_empty(startup->paths.journalRoot, journalError)) {
    reservation = ReserveAndroidStorageSubtrees(selectedRoots(scopeRequest), error);
    if (!reservation) { SDL_Log("Storage recovery: %s", error.c_str()); return false; }
  }
  if (journalError) return false;
#endif
  auto recovery = StorageFileTransaction(startup->paths).Recover();
  if (recovery.recoveryRequired) return false;
  context.chartLibraryFolderActions = std::make_unique<chart_library_platform::FolderActionService>(
      context.chartRepository, *context.chartLibraryTasks);
  auto environment = std::make_shared<Access>();
  context.storageTask = std::make_shared<StorageTask>(StorageTaskDependencies{
    .admit = [&context, environment](const OptimizationRequest &request, std::string &error)
        -> std::optional<StorageMaintenanceGuard> {
      if (context.appInBackground || context.profileGameplayActive || context.profileArchiveOperationActive ||
          context.replayVideoExportActive || context.backgroundTasksPausedForForegroundScene) {
        error = "Finish gameplay, export, or the profile operation before managing storage.";
        return {};
      }
      if (context.chartLibraryFolderActions && context.chartLibraryFolderActions->active()) {
        error = "A chart folder picker is active."; return {};
      }
      if (auto reason = context.profileSwitchBlockers.firstReason()) { error = *reason; return {}; }
      const auto token = context.chartLibraryTasks->tryReserveMaintenance();
      if (!token) { error = "A chart library scan or import is active."; return {}; }
      context.storageMaintenanceActive = true;
      StorageMaintenanceGuard guard(
          [&context, token](const StorageReport &report) {
            ++context.storageMeasurementRevision;
            if (report.recoveryRequired) return;
            for (const auto &root : report.changedRoots) {
              if (!context.chartLibraryTasks->enqueueMaintenanceRefresh(*token, {
                  .kind = chart_library_tasks::TaskKind::RefreshPath,
                  .title = "Refresh optimized chart folder",
                  .refreshPath = root})) throw std::runtime_error("Could not refresh optimized folder");
            }
            context.chartLibraryListReloadRequested = true;
          },
          [&context, token, environment] {
            environment->scopes.clear();
            std::string ignored;
            context.musicPlayer.SetStorageMaintenance(false, ignored);
            context.chartLibraryTasks->releaseMaintenance(*token);
            context.storageMaintenanceActive = false;
          });
      auto access = accessFolders(context, error);
      if (!access) return {};
      const auto roots = selectedRoots(request);
      if (roots.empty()) { error = "Select a local chart folder."; return {}; }
      for (const auto &root : roots) {
        if (!IsSafeLocalPath(root, error) ||
            std::any_of(access->unavailableRoots.begin(), access->unavailableRoots.end(),
                [&](const auto &unavailable) { return IsWithinFolder(root, unavailable); }) ||
            std::none_of(access->paths.allowedRoots.begin(), access->paths.allowedRoots.end(),
                [&](const auto &allowed) { return IsWithinFolder(root, allowed); })) {
          error = "The selected folder is not a registered local chart folder."; return {};
        }
      }
#if TARGET_OS_ANDROID
      auto reservation = ReserveAndroidStorageSubtrees(roots, error);
      if (!reservation) return {};
      access->scopes.push_back(std::move(reservation));
#endif
      *environment = std::move(*access);
      if (!context.musicPlayer.SetStorageMaintenance(true, error)) return {};
      if (context.sceneManager && !context.sceneManager->prepareForStorageMaintenance(error)) return {};
      auto stopped = context.jukebox.discardLoadedMedia();
      if (!stopped.success) { error = stopped.diagnostic; return {}; }
      return guard;
    },
    .run = [environment](const OptimizationRequest &request, Checkpoint check, ProgressCallback progress) {
      return StorageOptimizer(DefaultOptimizerDependencies(environment->paths)).Run(request, std::move(check), std::move(progress));
    },
    .wake = [] { SharedStorageConversionPermit()->Notify(); },
  });
  return true;
}
void PollStorageTask(ApplicationContext &context) {
  if (context.storageTask) context.storageTask->TakeCompletion();
}
void RequestCancelStorageTask(ApplicationContext &context) {
  if (context.storageTask) context.storageTask->RequestCancel();
}
void CancelAndWaitStorageTask(ApplicationContext &context) {
  if (context.storageTask) context.storageTask->CancelAndWait();
}
void ShutdownStorageServices(ApplicationContext &context) {
  if (context.storageTask) context.storageTask->Shutdown();
  context.storageTask.reset();
}
}
