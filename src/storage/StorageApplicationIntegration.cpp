#include "StorageApplicationIntegration.h"
#include "StorageTask.h"
#include "StoragePathIndex.h"
#include "StorageArchiveTransaction.h"
#include "../context.h"
#include "../scene/SceneManager.h"
#include "../scene/Scene.h"
#include "../view/ImageView.h"
#include "../RAII.h"
#include "../targets.h"
#include <SDL3/SDL.h>
#if TARGET_OS_ANDROID
#include "../AndroidNatives.h"
#elif TARGET_OS_IOS || TARGET_OS_SIMULATOR
#include "../iOSNatives.hpp"
#include "../repositories/ChartStorageIdentity.h"
#endif

namespace storage_management {
namespace {
struct Access {
  StoragePlatformPaths paths;
  std::vector<std::shared_ptr<void>> scopes;
  std::vector<std::filesystem::path> availableRoots;
};
std::optional<Access> accessFolders(ApplicationContext &context, std::string &error) {
  auto session = context.chartRepository.OpenSession();
  if (!session) { error = "Chart library is unavailable."; return {}; }
  Access access;
  std::vector<std::filesystem::path> roots;
  for (const auto &entry : session->SelectAllEntries()) {
    auto path = chart_library_platform::resolveFolderEntryPath(entry);
    bool available = true;
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
    if (!entry.iosBookmark.empty()) {
      std::string resolved;
      void *handle = StartIOSSecurityScopedResource(fspath_to_utf8(path), entry.iosBookmark, resolved, error);
      if (handle) access.scopes.emplace_back(handle, [](void *value) { StopIOSSecurityScopedResource(value); });
      // The native API can return a wrapper even when starting access failed.
      // Retain it for balanced cleanup, but do not treat it as a new grant.
      available = handle && error.empty();
      error.clear();
      if (!resolved.empty()) path = std::filesystem::path(resolved);
    }
#endif
    // Local archive files inherit their registered folder's authority; SAF
    // references do not grant local filesystem authority. Missing roots may contain recovery records;
    // retain their absolute names so recovery can fail closed on actual access.
    if (path.is_absolute()) {
      path = path.lexically_normal();
      roots.push_back(path);
      if (available) access.availableRoots.push_back(path);
    }
  }
  auto root = StoragePrivateRoot(context);
  if (root.empty()) { error = "Private storage is unavailable."; return {}; }
  access.paths = MakeStoragePlatformPaths(root, access.availableRoots, TARGET_OS_ANDROID || TARGET_OS_IOS || TARGET_OS_SIMULATOR);
  // Retain all registrations for recovery authority, but only active grants may
  // bypass inaccessible system ancestors. Recovery still fails closed on IO.
  access.paths.allowedRoots = std::move(roots);
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  // Documents and Library survive app updates even when iOS relocates their
  // enclosing container. Only the OS-derived Documents root can retarget paths;
  // recovery still checks current registration authority and recorded identities.
  access.paths.resolveRecoveryPath = [documents = std::filesystem::path(GetIOSDocumentsPath())](const auto &path) {
    return chart_storage_identity::RebaseLegacyIOSDocumentsPath(path, documents).value_or(path);
  };
#endif
  return access;
}
std::vector<std::filesystem::path> selectedRoots(const OptimizationRequest &request) {
  std::vector<std::filesystem::path> roots;
  for (const auto &group : request.inventory.groups)
    roots.push_back((group.chartPathFallback ? group.path.parent_path() : group.path).lexically_normal());
  return StoragePathIndex(std::move(roots)).MinimalRoots();
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

bool InitializeStorageServices(ApplicationContext &context, StorageRecoveryProgressCallback notify) {
  StorageRecoveryProgress progress;
  NotifyStorageRecoveryProgress(notify, progress);
  std::string error;
  auto startup = accessFolders(context, error);
  if (!startup) { SDL_Log("Storage recovery: %s", error.c_str()); return false; }
#if TARGET_OS_ANDROID
  const auto recoveryRoots = StorageReservationRoots(startup->paths, {}, error);
  if (!recoveryRoots) { SDL_Log("Storage recovery: %s", error.c_str()); return false; }
  std::shared_ptr<void> reservation;
  if (!recoveryRoots->empty()) {
    reservation = ReserveAndroidStorageSubtrees(StoragePathIndex(*recoveryRoots).MinimalRoots(), error);
    if (!reservation) { SDL_Log("Storage recovery: %s", error.c_str()); return false; }
  }
#endif
  auto recovery = RecoverStorageArchives(startup->paths, {}, {}, [&](const auto &value) {
    progress = value;
    NotifyStorageRecoveryProgress(notify, progress);
  });
  if (!recovery.recoveryRequired && !recovery.cancelled) {
    progress.phase = StorageRecoveryPhase::Files;
    progress.currentPath.clear(); progress.archiveBytesDone = 0;
    NotifyStorageRecoveryProgress(notify, progress);
    recovery = StorageFileTransaction(startup->paths).Recover({}, {
      .before = [&](const auto &folder) {
        progress.currentPath = folder;
        NotifyStorageRecoveryProgress(notify, progress);
      },
      .after = [&](const auto &, const auto &) {
        ++progress.completedOperations;
        NotifyStorageRecoveryProgress(notify, progress);
      },
    });
  }
  if (recovery.recoveryRequired || recovery.cancelled) {
    for (const auto &file : recovery.files)
      SDL_Log("Storage recovery: %s: %s", file.source.string().c_str(), file.reason.detail.c_str());
    return false;
  }
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
      // Guard construction and its captured roots can allocate. Reserve cleanup
      // first without allocating, and expose maintenance only once it is owned.
      auto reservationRollback = makeScopeExit([&context, token] {
        context.chartLibraryTasks->releaseMaintenance(*token);
      });
      StorageMaintenanceGuard guard(
          [&context, token, roots = selectedRoots(request)](const StorageReport &report) {
            ++context.storageMeasurementRevision;
            if (report.recoveryRequired) return;
            if (!report.changedRoots.empty()) {
              // Include selected ancestors whose charts may reference a changed
              // nested media folder, and perform one scoped scanner operation.
              const StoragePathIndex selected(roots);
              std::vector<std::filesystem::path> refreshRoots;
              for (const auto &changed : report.changedRoots) {
                const auto ancestors = selected.Ancestors(changed);
                refreshRoots.push_back(ancestors.empty() ? changed : roots[ancestors.front()]);
              }
              chart_library_tasks::TaskRequest refresh;
              refresh.kind = chart_library_tasks::TaskKind::RefreshPath;
              refresh.title = "Refresh optimized chart folders";
              refresh.refreshPaths = StoragePathIndex(std::move(refreshRoots)).MinimalRoots();
              refresh.storageMeasurementsCurrent = true;
              if (!context.chartLibraryTasks->enqueueMaintenanceRefresh(*token, std::move(refresh)))
                throw std::runtime_error("Could not refresh optimized folders");
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
      reservationRollback.dismiss();
      context.storageMaintenanceActive = true;
      auto access = accessFolders(context, error);
      if (!access) return {};
      const auto roots = request.preflight ? request.preflight->EligibleRoots() : selectedRoots(request);
      if (roots.empty()) { error = "Select a local chart folder."; return {}; }
      // Grants form a union: a failed overlapping bookmark does not revoke an
      // active parent or child grant. Anchor validation at the most specific
      // active boundary and keep siblings outside that grant unauthorized.
      const StoragePathIndex allowed(access->availableRoots),
          boundaries(access->paths.pathAccessRoots);
      for (const auto &root : roots) {
        const auto matches = boundaries.Ancestors(root);
        const auto localBoundaries = matches.empty() ? std::vector<std::filesystem::path>{} :
            std::vector<std::filesystem::path>{access->paths.pathAccessRoots[matches.back()]};
        const auto allowedMatches = allowed.Ancestors(root);
        if (!IsSafeLocalPath(root, error, localBoundaries) ||
            !std::ranges::any_of(allowedMatches, [&](const auto index) {
              return IsWithinLocalFolder(root, access->availableRoots[index]);
            })) {
          error = "The selected folder is not a registered local chart folder."; return {};
        }
      }
#if TARGET_OS_ANDROID
      const auto reservationRoots = StorageReservationRoots(access->paths, roots, error);
      if (!reservationRoots) return {};
      auto reservation = ReserveAndroidStorageSubtrees(StoragePathIndex(*reservationRoots).MinimalRoots(), error);
      if (!reservation) return {};
      access->scopes.push_back(std::move(reservation));
#endif
      *environment = std::move(*access);
      if (context.sceneManager && !context.sceneManager->canPrepareForStorageMaintenance(error)) return {};
      // Entering Storage pauses the retained scenes and joins their preload
      // workers. The maintenance gate prevents new loads. After confirming the
      // shared backend is stopped, unload native music before releasing shared
      // PCM or scene owners. Either rejection retains the loaded media.
      auto stopped = context.jukebox.discardLoadedMedia([&] {
        const bool unloaded = context.musicPlayer.SetStorageMaintenance(true, error);
        return audio::playback::BackendOperationResult{.success = unloaded, .diagnostic = error};
      });
      if (!stopped.success) { error = stopped.diagnostic; return {}; }
      if (context.sceneManager && !context.sceneManager->prepareForStorageMaintenance(error)) return {};
      // The storage list can keep requesting artwork while archives are moved.
      // Retain this drain through recovery-required reports as part of the guard.
      environment->scopes.push_back(pauseImageReadsForStorageMaintenance());
      return guard;
    },
    .run = [environment](OptimizationRequest request, Checkpoint check, ProgressCallback progress) {
      auto report = StorageOptimizer(DefaultOptimizerDependencies(environment->paths)).Run(std::move(request), std::move(check), std::move(progress));
      for (const auto &file : report.files)
        if (file.outcome == FileOutcome::Failed)
          SDL_Log("Storage optimization failed for %s: %s", fspath_to_utf8(file.source).c_str(), file.reason.detail.c_str());
      return report;
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
