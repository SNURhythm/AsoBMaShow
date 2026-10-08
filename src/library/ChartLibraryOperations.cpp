#include "ChartLibraryOperations.h"
#include "ChartLibraryPlatform.h"
#include "ArchiveUnzipRecovery.h"

#include "../ArchiveFile.h"
#include "../archive/ArchiveSourceAccess.h"
#include "../RAII.h"
#include "../scene/ArchiveUnzipPresentation.h"
#include "../Utils.h"
#include "../path.h"
#include "../targets.h"

#include <SDL2/SDL.h>

#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <utility>
#include <vector>

namespace chart_library_tasks {

namespace {
constexpr const char *kDefaultDifficultyTableUrls[] = {
    "https://rattoto10.jounin.jp/table.html",
    "https://rattoto10.jounin.jp/table_insane.html",
    "https://miraiscarlet.github.io/bms/table/genocide_normal/normal_bms.html",
    "https://miraiscarlet.github.io/bms/table/genocide_insane/insane_bms.html",
    "https://stellabms.xyz/sl/table.html",
    "https://stellabms.xyz/st/table.html",
    "https://asumatoki.kr/table/aery/header.json",
    "https://asumatoki.kr/table/aery7/header.json",
};
} // namespace

ChartLibraryOperations::ChartLibraryOperations(
    ChartLibraryOperationsDependencies dependencies)
    : dependencies_(std::move(dependencies)) {
  if (!dependencies_.importDifficultyTableFromUrl) {
    dependencies_.importDifficultyTableFromUrl =
        [](ChartRepository::Session &session, const std::string &url,
           std::string *errorMessage,
           DifficultyTableImportProgressCallback progressCallback,
           const DifficultyTableImportCheckpoint &checkpoint,
           const DifficultyTableImportPauseProbe &pauseRequested) {
          DifficultyTableImporter importer;
          return importer.ImportFromUrl(session, url, errorMessage,
                                        std::move(progressCallback),
                                        checkpoint, pauseRequested);
        };
  }
  if (!dependencies_.importDifficultyTablesFromDirectory) {
    dependencies_.importDifficultyTablesFromDirectory =
        [](ChartRepository::Session &session,
           const std::filesystem::path &directory,
           const DifficultyTableImportCheckpoint &checkpoint) {
          DifficultyTableImporter importer;
          return importer.ImportFromDirectory(session, directory, checkpoint);
        };
  }
  if (!dependencies_.updateDifficultyTableFromSourceUrl) {
    dependencies_.updateDifficultyTableFromSourceUrl =
        [](ChartRepository::Session &session, int tableId,
           std::string *errorMessage,
           const DifficultyTableImportCheckpoint &checkpoint,
           const DifficultyTableImportPauseProbe &pauseRequested) {
          DifficultyTableImporter importer;
          return importer.UpdateFromSourceUrl(session, tableId, errorMessage,
                                              checkpoint, pauseRequested);
        };
  }
  if (!dependencies_.refreshFolderAccess) {
    dependencies_.refreshFolderAccess =
        [](const std::vector<ChartEntry> &entries) {
          chart_library_platform::refreshFolderAccess(entries);
        };
  }
}

TaskRunResult ChartLibraryOperations::run(
    const TaskRequest &request, const std::stop_token &stopToken,
    TaskProgressCallback progress, TaskPauseCallback waitForResume) {
  switch (request.kind) {
  case TaskKind::RefreshLibrary:
    return runRefresh(request, stopToken, progress, waitForResume);
  case TaskKind::RefreshPath:
    return runPathRefresh(request, stopToken, progress, waitForResume);
  case TaskKind::UpdateDifficultyTable:
    return runDifficultyTableUpdate(request, stopToken, progress,
                                    waitForResume);
  case TaskKind::IndexDownloadedPath:
    return runDownloadedIndex(request, stopToken, progress, waitForResume);
  case TaskKind::AndroidImport:
    return runAndroidImport(request, stopToken, progress, waitForResume);
  }
  throw TaskError(i18n::message("library.tasks.unknown_task"));
}

TaskRunResult ChartLibraryOperations::runDifficultyTableUpdate(
    const TaskRequest &request, const std::stop_token &stopToken,
    const TaskProgressCallback &progress,
    const TaskPauseCallback &waitForResume) {
  if (!waitForResume() || stopToken.stop_requested()) {
    return {.disposition = TaskRunDisposition::Paused, .detail = i18n::message("library.tasks.paused.label")};
  }
  auto session = dependencies_.repository.OpenSession();
  if (!session.has_value()) {
    throw TaskError(i18n::message("library.tasks.database_open_failed"));
  }
  session->EnsureSchema();
  progress({.current = 0,
            .total = 1,
            .stage = ChartScanProgressStage::Preparing},
           i18n::message("library.tasks.updating_table"));
  std::atomic_bool interrupted = false;
  const DifficultyTableImportCheckpoint checkpoint = [&] {
    // Non-blocking pause probe: abort to Paused when gameplay pauses instead
    // of blocking this thread in waitForResume until gameplay ends.
    const bool resumed =
        !stopToken.stop_requested() &&
        !(dependencies_.pauseRequested && dependencies_.pauseRequested());
    if (!resumed) interrupted.store(true, std::memory_order_release);
    return resumed;
  };
  const DifficultyTableImportPauseProbe pauseRequested = [&] {
    return stopToken.stop_requested() ||
           (dependencies_.pauseRequested && dependencies_.pauseRequested());
  };
  std::string errorMessage;
  if (!dependencies_.updateDifficultyTableFromSourceUrl(
          *session, request.tableId, &errorMessage, checkpoint,
          pauseRequested)) {
    if (interrupted.load(std::memory_order_acquire) ||
        stopToken.stop_requested()) {
      return {.disposition = TaskRunDisposition::Paused, .detail = i18n::message("library.tasks.paused.label")};
    }
    throw TaskError(errorMessage.empty()
                                 ? i18n::message("library.tasks.table_update_failed")
                                 : errorMessage);
  }
  if (interrupted.load(std::memory_order_acquire) ||
      stopToken.stop_requested()) {
    return {.disposition = TaskRunDisposition::Paused, .detail = i18n::message("library.tasks.paused.label")};
  }
  if (dependencies_.requestReload) dependencies_.requestReload(false);
  return {.detail = i18n::message("library.tasks.complete.label")};
}

TaskRunResult ChartLibraryOperations::runPathRefresh(
    const TaskRequest &request, const std::stop_token &stopToken,
    const TaskProgressCallback &progress,
    const TaskPauseCallback &waitForResume) {
  if (!waitForResume() || stopToken.stop_requested()) {
    return {.disposition = TaskRunDisposition::Paused, .detail = i18n::message("library.tasks.paused.label")};
  }

  auto session = dependencies_.repository.OpenSession();
  if (!session.has_value()) {
    throw TaskError(i18n::message("library.tasks.database_open_failed"));
  }
  session->EnsureSchema();

  std::atomic_bool checkpointPaused{false};
  auto checkpoint = [&] {
    // Non-blocking pause probe: abort to Paused when gameplay pauses instead
    // of blocking the single library thread in waitForResume (which only
    // clears when gameplay ends). The framework re-runs the task after
    // resume.
    const bool resumed =
        !stopToken.stop_requested() &&
        !(dependencies_.pauseRequested && dependencies_.pauseRequested());
    if (!resumed) checkpointPaused.store(true, std::memory_order_relaxed);
    return resumed;
  };
  auto publishScanProgress = [&](const ChartScanProgress &value) {
    progress(value, progressStageText(value.stage));
  };
  ChartLibraryScanner scanner;
  const auto result = scanner.ScanScopedWithResult(
      *session, {request.refreshPath}, &stopToken, publishScanProgress,
      checkpoint, dependencies_.pendingScanFlushRequest,
      dependencies_.completeScanFlush);
  SDL_Log("Chart folder refresh changed %d entries", result.changedCount);

  if (stopToken.stop_requested() ||
      checkpointPaused.load(std::memory_order_relaxed)) {
    return {.disposition = TaskRunDisposition::Paused, .detail = i18n::message("library.tasks.paused.label")};
  }
  if (!result.completed) {
    throw TaskError(i18n::message("library.tasks.folder_refresh_failed"));
  }
  if (dependencies_.requestReload) {
    dependencies_.requestReload(true);
  }
  return {.detail = i18n::message("library.tasks.complete.label")};
}

TaskRunResult ChartLibraryOperations::runRefresh(
    const TaskRequest &request, const std::stop_token &stopToken,
    const TaskProgressCallback &progress,
    const TaskPauseCallback &waitForResume) {
  if (!waitForResume() || stopToken.stop_requested()) {
    return {.disposition = TaskRunDisposition::Paused, .detail = i18n::message("library.tasks.paused.label")};
  }

  auto session = dependencies_.repository.OpenSession();
  if (!session.has_value()) {
    throw TaskError(i18n::message("library.tasks.database_open_failed"));
  }
  session->EnsureSchema();

  TaskRunResult pausedResult{
      .disposition = TaskRunDisposition::Paused,
      .detail = i18n::message("library.tasks.paused.label"),
      .folderRegistrationCompleted = request.folderRegistrationCompleted};
  std::vector<ChartEntry> entries;
  if (!request.folderToAdd.empty()) {
    if (!request.folderRegistrationCompleted) {
      progress({.current = 1,
                .total = 100,
                .stage = ChartScanProgressStage::Preparing},
               i18n::message("library.tasks.adding_folder"));
      if (!session->InsertEntry(request.folderToAdd, request.iosBookmark)) {
        throw TaskError(i18n::message("library.tasks.add_folder_failed"));
      }
      pausedResult.folderRegistrationCompleted = true;
      if (dependencies_.requestReload) {
        dependencies_.requestReload(true);
      }
    }
    entries.push_back({.path = fspath_to_path_t(request.folderToAdd),
                       .iosBookmark = request.iosBookmark});
  }

  const auto pendingUnzips = session->LoadUnzipRecovery();
  if (pendingUnzips && !pendingUnzips->empty()) {
    dependencies_.refreshFolderAccess(session->SelectEffectiveEntries());
  }
  std::atomic_bool recoveryPaused{false};
  const auto recovery = archive_unzip_recovery::recover(
      *session, stopToken,
      [&](const ChartScanProgress &value) {
        progress(value, i18n::message("library.tasks.recovering_unzip"));
      },
      [&] {
        const bool resumed = !stopToken.stop_requested() &&
                             !(dependencies_.pauseRequested && dependencies_.pauseRequested());
        if (!resumed) recoveryPaused.store(true, std::memory_order_relaxed);
        return resumed;
      });
  if (recovery.libraryChanged && dependencies_.requestReload) {
    dependencies_.requestReload(true);
  }
  if (stopToken.stop_requested() || recoveryPaused.load(std::memory_order_relaxed)) return pausedResult;
  const auto recoveryDetail = i18n::message("library.tasks.recovery_pending");

  progress({.current = 2,
            .total = 100,
            .stage = ChartScanProgressStage::Preparing},
           i18n::message("library.tasks.importing_tables"));
  if (!waitForResume() || stopToken.stop_requested()) {
    return pausedResult;
  }
  bool tableImportInterrupted = false;
  const auto seedCompleted = seedDefaultDifficultyTablesIfNeeded(
      *session, stopToken, progress, waitForResume);
  tableImportInterrupted = tableImportInterrupted || !seedCompleted;
  if (!waitForResume() || stopToken.stop_requested()) {
    return pausedResult;
  }
  const DifficultyTableImportCheckpoint tableImportCheckpoint = [&] {
    // Non-blocking: a gameplay pause must abort the import to Paused rather
    // than hold the single library thread in waitForResume (which only clears
    // when gameplay ends). The framework re-runs the task after resume.
    const bool resumed =
        !stopToken.stop_requested() &&
        !(dependencies_.pauseRequested && dependencies_.pauseRequested());
    tableImportInterrupted = tableImportInterrupted || !resumed;
    return resumed;
  };
  const int importedTables = dependencies_.importDifficultyTablesFromDirectory(
      *session, dependencies_.tablesDirectory, tableImportCheckpoint);
  if (tableImportInterrupted || stopToken.stop_requested()) {
    return pausedResult;
  }
  if (importedTables > 0 &&
      dependencies_.requestReload) {
    dependencies_.requestReload(true);
  }

  if (entries.empty()) {
    entries = session->SelectEffectiveEntries();
  }
  if (stopToken.stop_requested()) {
    return pausedResult;
  }
  if (entries.empty() && dependencies_.selectInitialFolder) {
    const auto selected = dependencies_.selectInitialFolder();
    if (selected && !selected->empty()) {
      if (!session->InsertEntry(*selected)) {
        throw TaskError(i18n::message("library.tasks.add_selected_folder_failed"));
      }
      entries = session->SelectEffectiveEntries();
    }
  }
  if (!waitForResume() || stopToken.stop_requested()) {
    return pausedResult;
  }

  // iOS refresh tears down all current security-scoped handles before opening
  // this list. Keep the scan roots scoped to a newly added folder, but always
  // reopen every effective entry.
  dependencies_.refreshFolderAccess(session->SelectEffectiveEntries());

  if (entries.empty()) {
    return {.disposition = recovery.completed ? TaskRunDisposition::Complete : TaskRunDisposition::Failed,
            .detail = recovery.completed ? i18n::message("library.tasks.complete.label") : recoveryDetail};
  }

  if (request.rebuildLibraryMetadata) {
    progress({.current = 8,
              .total = 100,
              .stage = ChartScanProgressStage::Preparing},
             i18n::message("library.tasks.clearing_caches"));
    archive_file::appendDebugLogLine(
        "Manual library rebuild requested; clearing chart metadata caches.");
    if (!session->ClearChartMeta()) {
      throw TaskError(i18n::message("library.tasks.clear_cache_failed"));
    }
    pausedResult.rebuildLibraryMetadataCleared = true;
  }

  std::vector<std::filesystem::path> roots;
  roots.reserve(entries.size());
  for (const auto &entry : entries) {
    if (stopToken.stop_requested()) {
      return pausedResult;
    }
    roots.push_back(chart_library_platform::resolveFolderEntryPath(entry));
  }

  std::atomic_bool checkpointPaused{false};
  auto checkpoint = [&] {
    // Non-blocking pause probe: abort to Paused when gameplay pauses instead
    // of blocking the single library thread in waitForResume (which only
    // clears when gameplay ends). The framework re-runs the task after
    // resume.
    const bool resumed =
        !stopToken.stop_requested() &&
        !(dependencies_.pauseRequested && dependencies_.pauseRequested());
    if (!resumed) checkpointPaused.store(true, std::memory_order_relaxed);
    return resumed;
  };
  auto publishScanProgress = [&](const ChartScanProgress &value) {
    progress(value, progressStageText(value.stage));
  };
  SDL_Log("Refreshing chart library");
  ChartLibraryScanner scanner;
  const auto result = scanner.ScanWithResult(
      *session, roots, &stopToken, publishScanProgress, checkpoint,
      dependencies_.pendingScanFlushRequest,
      dependencies_.completeScanFlush);
  SDL_Log("Chart library refresh changed %d entries", result.changedCount);

  const bool scanPaused = checkpointPaused.load(std::memory_order_relaxed);
  if (stopToken.stop_requested() || scanPaused) {
    return pausedResult;
  }
  if (!result.completed) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                 "Chart library refresh failed: completed=%d committed=%d "
                 "changed=%d stop=%d pause=%d",
                 static_cast<int>(result.completed),
                 static_cast<int>(result.committed), result.changedCount,
                 static_cast<int>(stopToken.stop_requested()),
                 static_cast<int>(scanPaused));
    archive_file::appendDebugLogLine(
        "Chart library refresh failed: completed=" +
        std::to_string(result.completed) + " committed=" +
        std::to_string(result.committed) + " changed=" +
        std::to_string(result.changedCount) + " stop=" +
        std::to_string(stopToken.stop_requested()) + " pause=" +
        std::to_string(scanPaused));
    throw TaskError(i18n::message("library.tasks.refresh_failed"));
  }
  if (dependencies_.requestReload) {
    dependencies_.requestReload(true);
  }
  return {.disposition = recovery.completed ? TaskRunDisposition::Complete : TaskRunDisposition::Failed,
          .detail = recovery.completed ? i18n::message("library.tasks.complete.label") : recoveryDetail,
          .rebuildLibraryMetadataCleared = pausedResult.rebuildLibraryMetadataCleared,
          .folderRegistrationCompleted = pausedResult.folderRegistrationCompleted};
}

bool ChartLibraryOperations::seedDefaultDifficultyTablesIfNeeded(
    ChartRepository::Session &session, const std::stop_token &stopToken,
    const TaskProgressCallback &progress,
    const TaskPauseCallback &waitForResume) {
  if ((dependencies_.defaultDifficultyTablesSeeded &&
       dependencies_.defaultDifficultyTablesSeeded()) ||
      stopToken.stop_requested()) {
    return true;
  }

  constexpr int totalTables =
      static_cast<int>(sizeof(kDefaultDifficultyTableUrls) /
                       sizeof(kDefaultDifficultyTableUrls[0]));
  int successfulTables = 0;
  bool allSucceeded = true;
  bool interrupted = false;
  const DifficultyTableImportCheckpoint checkpoint = [&] {
    // Non-blocking pause probe: abort to Paused when gameplay pauses instead
    // of blocking this thread in waitForResume until gameplay ends.
    if (stopToken.stop_requested() ||
        (dependencies_.pauseRequested && dependencies_.pauseRequested())) {
      interrupted = true;
      return false;
    }
    return true;
  };
  const DifficultyTableImportPauseProbe pauseRequested = [&] {
    return stopToken.stop_requested() ||
           (dependencies_.pauseRequested && dependencies_.pauseRequested());
  };
  for (int i = 0; i < totalTables; ++i) {
    if (!checkpoint()) {
      return false;
    }
    const char *url = kDefaultDifficultyTableUrls[i];
    progress({.current = i,
              .total = totalTables,
              .stage = ChartScanProgressStage::Preparing},
             i18n::message("library.tasks.adding_default_tables"));
    std::string errorMessage;
    const bool ok = dependencies_.importDifficultyTableFromUrl(
        session, url, &errorMessage,
        [&progress, i, totalTables,
         url](const DifficultyTableImportProgress &value) {
          const std::string detail = value.tableName.empty()
                                         ? std::string(url)
                                         : value.tableName;
          progress({.current = i + (value.current > 0 ? 1 : 0),
                    .total = totalTables,
                    .stage = ChartScanProgressStage::Preparing},
                   i18n::message("library.tasks.adding_default_table", {{"name", detail}}));
        },
        checkpoint, pauseRequested);
    if (interrupted || stopToken.stop_requested()) {
      return false;
    }
    if (ok) {
      ++successfulTables;
    } else {
      allSucceeded = false;
      SDL_Log("Failed to import default difficulty table %s: %s", url,
              errorMessage.empty() ? "unknown error" : errorMessage.c_str());
    }
  }

  if (interrupted || stopToken.stop_requested()) {
    return false;
  }
  if (allSucceeded && dependencies_.setDefaultDifficultyTablesSeeded) {
    dependencies_.setDefaultDifficultyTablesSeeded(true);
    if (dependencies_.saveSettings && !dependencies_.saveSettings()) {
      SDL_Log("Failed to save default difficulty table seed setting");
    }
  }
  if (successfulTables > 0 && dependencies_.requestReload) {
    dependencies_.requestReload(true);
  }
  return true;
}

TaskRunResult ChartLibraryOperations::runDownloadedIndex(
    const TaskRequest &request, const std::stop_token &stopToken,
    const TaskProgressCallback &progress,
    const TaskPauseCallback &waitForResume) {
  auto session = dependencies_.repository.OpenSession();
  if (!session.has_value()) {
    throw TaskError(i18n::message("library.tasks.database_open_failed"));
  }
  if (!session->EnsureSchema()) {
    throw TaskError(i18n::message("library.tasks.database_prepare_failed"));
  }
  if (!waitForResume() || stopToken.stop_requested()) {
    return {.disposition = TaskRunDisposition::Paused, .detail = i18n::message("library.tasks.paused.label")};
  }

  for (const auto &removedPath : request.downloadedRemovedPaths) {
    if (session->DeleteChartMetaInDirectory(removedPath) < 0) {
      if (dependencies_.requestReload) {
        dependencies_.requestReload(true);
      }
      throw TaskError(i18n::message("library.tasks.reconcile_failed"));
    }
  }

  const auto entries =
      main_menu_library::downloadedPathScanEntries(request.downloadedPath);
  if (entries.empty()) {
    throw TaskError(i18n::message("library.tasks.download_empty"));
  }
  std::vector<std::filesystem::path> roots;
  roots.reserve(entries.size());
  for (const auto &entry : entries) {
    roots.push_back(chart_library_platform::resolveFolderEntryPath(entry));
  }

  std::atomic_bool checkpointPaused{false};
  auto checkpoint = [&] {
    // Non-blocking pause probe: abort to Paused when gameplay pauses instead
    // of blocking the single library thread in waitForResume (which only
    // clears when gameplay ends). The framework re-runs the task after
    // resume.
    const bool resumed =
        !stopToken.stop_requested() &&
        !(dependencies_.pauseRequested && dependencies_.pauseRequested());
    if (!resumed) checkpointPaused.store(true, std::memory_order_relaxed);
    return resumed;
  };
  auto publishScanProgress = [&](const ChartScanProgress &value) {
    progress(value, progressStageText(value.stage));
  };
  ChartLibraryScanner scanner;
  const ChartScanResult scanResult = scanner.ScanAddedWithResult(
      *session, roots, &stopToken, publishScanProgress, checkpoint);
  if (stopToken.stop_requested() ||
      checkpointPaused.load(std::memory_order_relaxed)) {
    return {.disposition = TaskRunDisposition::Paused, .detail = i18n::message("library.tasks.paused.label")};
  }
  if (!scanResult.completed) {
    if (dependencies_.requestReload) {
      dependencies_.requestReload(true);
    }
    throw TaskError(i18n::message("library.tasks.download_index_failed"));
  }

  std::optional<std::filesystem::path> chartPath;
  if (scanResult.committed && request.downloadedTargetIdentity.valid()) {
    const auto matches = session->SelectChartMetaByHash(
        request.downloadedTargetIdentity.sha256,
        request.downloadedTargetIdentity.md5);
    chartPath = main_menu_library::downloadedChartPath(
        matches, request.downloadedPath, scanResult.upsertedChartPaths);
  }
  if (!main_menu_library::findBmsIndexTaskSucceeded(
          request.downloadedTargetIdentity, scanResult.committed, chartPath)) {
    if (dependencies_.requestReload) {
      dependencies_.requestReload(true);
    }
    throw TaskError(i18n::message("library.tasks.download_target_failed"));
  }

  TaskRunResult result{.detail = i18n::message("library.tasks.complete.label")};
  if (chartPath.has_value()) {
    result.downloadedIndex = DownloadedIndexCompletion{
        .chartPath = *chartPath,
        .targetIdentity = request.downloadedTargetIdentity,
        .selectionGeneration = request.downloadedSelectionGeneration,
    };
  }
  if (dependencies_.requestReload) {
    dependencies_.requestReload(true);
  }
  return result;
}

TaskRunResult ChartLibraryOperations::runAndroidImport(
    const TaskRequest &request, const std::stop_token &stopToken,
    const TaskProgressCallback &progress,
    const TaskPauseCallback &waitForResume) {
#if TARGET_OS_ANDROID
  const std::filesystem::path importPath = request.androidImportPath;
  if (importPath.empty()) {
    throw TaskError(i18n::message("library.tasks.import_empty"));
  }

  const bool referencedSource = archive_source::isReference(importPath);
  const bool keepArchive = !request.androidArchiveUri.empty();
  if (keepArchive && !archive_source::validReference(importPath))
    throw TaskError("Invalid archive reference.");
  ScopeExit discardStaging([&] {
    if (!request.androidImportFolder && !referencedSource) {
      std::error_code ignored;
      std::filesystem::remove(importPath, ignored);
    }
  });
  std::error_code importPathError;
  const bool importingFolder =
      request.androidImportFolder ||
      std::filesystem::is_directory(importPath, importPathError);
  const std::string importType = importingFolder ? "folder" : "archive";
  const std::filesystem::path outputRoot =
      ChartRepository::DefaultBmsFolderPath();
  progress({.stage = ChartScanProgressStage::Preparing}, i18n::message("library.tasks.preparing_import"));
  archive_file::appendDebugLogLine(
      "Android import task requested: " + fspath_to_utf8(importPath) +
      " outputRoot=" + fspath_to_utf8(outputRoot));

  auto postImportProgress = [&](double fraction, const i18n::Text &message) {
    progress({.current = static_cast<int>(
                  std::clamp(fraction, 0.0, 1.0) * 10000.0),
              .total = 10000,
              .stage = ChartScanProgressStage::Preparing},
             message.empty()
                 ? (importingFolder ? i18n::message("library.tasks.importing_folder") : i18n::message("library.tasks.importing_archive"))
                 : message);
  };

  std::string errorMessage;
  std::error_code fsError;
  if (!keepArchive && !Utils::EnsureDirectoryExists(outputRoot, fsError)) {
    throw TaskError(i18n::message("library.tasks.import_directory_failed",
                                  {{"detail", fsError.message()}}));
  }

  std::filesystem::path outputFolder;
  if (importingFolder || keepArchive) {
    outputFolder = importPath;
    postImportProgress(0.90, i18n::message("library.tasks.refreshing_library"));
  } else {
    auto postUnzipProgress = [&](const archive_file::UnzipProgress &value) {
      postImportProgress(value.fraction * 0.90,
                         archive_unzip_presentation::status(value.message));
    };
    const auto unzippedArchive = archive_file::unzipArchiveFully(
        importPath, outputRoot, &errorMessage, &stopToken, postUnzipProgress,
        waitForResume);
    if (unzippedArchive.has_value()) {
      outputFolder = unzippedArchive->outputFolder;
    }
  }

  if (outputFolder.empty()) {
    throw TaskError(
        stopToken.stop_requested()
            ? i18n::message("library.tasks.import_cancel")
            : (errorMessage.empty() ? i18n::message("library.tasks.import_failed")
                                    : i18n::message("library.tasks.import_error", {{"detail", errorMessage}})));
  }
  if (stopToken.stop_requested()) {
    throw TaskError(i18n::message("library.tasks.import_cancel"));
  }

  auto session = dependencies_.repository.OpenSession();
  if (!session.has_value()) {
    throw TaskError(i18n::message(importingFolder
        ? "library.tasks.import_folder_refresh_failed"
        : "library.tasks.import_archive_refresh_failed"));
  }
  session->EnsureSchema();
  if (keepArchive) {
    if (!session->InsertEntry(importPath, "android-archive-uri:" + request.androidArchiveUri))
      throw TaskError("Could not save archive reference.");
    // A failed scan can be retried from this durable entry; its URI grant must
    // remain available even when the source temporarily disappears.
    if (request.androidArchiveOwner) request.androidArchiveOwner->retained.store(true);
  } else {
    session->InsertEntry(outputRoot);
  }

  std::vector<std::filesystem::path> roots{outputFolder};
  postImportProgress(0.92, i18n::message("library.tasks.refreshing_library"));
  auto scanProgress = [&](const ChartScanProgress &value) {
    const int total = std::max(0, value.total);
    const int current = total > 0 ? std::clamp(value.current, 0, total)
                                  : std::max(0, value.current);
    const double scanFraction =
        total > 0 ? static_cast<double>(current) / std::max(1, total) : 0.0;
    progress({.current = static_cast<int>((0.92 + scanFraction * 0.08) *
                                         10000.0),
              .total = 10000,
              .stage = value.stage},
             progressStageText(value.stage));
  };
  ChartLibraryScanner scanner;
  const auto scanResult = scanner.ScanWithResult(
      *session, roots, &stopToken, scanProgress, waitForResume);
  if (stopToken.stop_requested()) {
    throw TaskError(i18n::message("library.tasks.import_cancel"));
  }
  if (!scanResult.completed) {
    throw TaskError(i18n::message(importingFolder
        ? "library.tasks.import_folder_refresh_failed"
        : "library.tasks.import_archive_refresh_failed"));
  }

  if (dependencies_.requestReload) {
    dependencies_.requestReload(true);
  }
  const std::string message =
      scanResult.changedCount > 0
          ? "Imported " + importType + ". Library refreshed."
          : "Imported " + importType + ". Library already current.";
  SDL_Log("Android import task result: %s", message.c_str());
  archive_file::appendDebugLogLine(message);
  if (!request.androidImportRetainedError.empty()) {
    return {.disposition = TaskRunDisposition::Failed,
            .detail = i18n::message(request.androidImportMove ? "library.tasks.move_incomplete"
                                                            : "library.tasks.import_error",
                                    {{"detail", request.androidImportRetainedError}})};
  }
  return {.detail = i18n::message(request.androidImportMove ? "library.tasks.moved"
                                                           : "library.tasks.complete.label")};
#else
  (void)request;
  (void)stopToken;
  (void)progress;
  (void)waitForResume;
  throw TaskError(i18n::message("library.tasks.android_unavailable"));
#endif
}

i18n::Text ChartLibraryOperations::progressStageText(
    ChartScanProgressStage stage) {
  switch (stage) {
  case ChartScanProgressStage::Preparing:
    return i18n::message("library.tasks.preparing_scan");
  case ChartScanProgressStage::ScanningRoots:
    return i18n::message("library.tasks.scanning_folders");
  case ChartScanProgressStage::IndexingArchives:
    return i18n::message("library.tasks.indexing_archives");
  case ChartScanProgressStage::PreparingUpdates:
    return i18n::message("library.tasks.preparing_updates");
  case ChartScanProgressStage::RemovingDeleted:
    return i18n::message("library.tasks.removing_deleted");
  case ChartScanProgressStage::ParsingCharts:
    return i18n::message("library.tasks.parsing_charts");
  case ChartScanProgressStage::ReadingArchive:
    return i18n::message("library.tasks.reading_archive");
  }
  return i18n::message("library.tasks.refreshing_library");
}

} // namespace chart_library_tasks
