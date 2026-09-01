#include "ChartLibraryOperations.h"
#include "ChartLibraryPlatform.h"

#include "../ArchiveFile.h"
#include "../Utils.h"
#include "../path.h"
#include "../targets.h"

#include <SDL2/SDL.h>

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

namespace chart_library_tasks {

namespace {
constexpr const char *kDefaultDifficultyTableUrls[] = {
    "https://rattoto10.jounin.jp/table.html",
    "https://rattoto10.jounin.jp/table_insane.html",
    "https://stellabms.xyz/sl/table.html",
    "https://stellabms.xyz/st/table.html",
};
} // namespace

ChartLibraryOperations::ChartLibraryOperations(
    ChartLibraryOperationsDependencies dependencies)
    : dependencies_(std::move(dependencies)) {
  if (!dependencies_.importDifficultyTableFromUrl) {
    dependencies_.importDifficultyTableFromUrl =
        [](ChartRepository::Session &session, const std::string &url,
           std::string *errorMessage,
           DifficultyTableImportProgressCallback progressCallback) {
          DifficultyTableImporter importer;
          return importer.ImportFromUrl(session, url, errorMessage,
                                        std::move(progressCallback));
        };
  }
  if (!dependencies_.importDifficultyTablesFromDirectory) {
    dependencies_.importDifficultyTablesFromDirectory =
        [](ChartRepository::Session &session,
           const std::filesystem::path &directory) {
          DifficultyTableImporter importer;
          return importer.ImportFromDirectory(session, directory);
        };
  }
}

TaskRunResult ChartLibraryOperations::run(
    const TaskRequest &request, const std::stop_token &stopToken,
    TaskProgressCallback progress, TaskPauseCallback waitForResume) {
  switch (request.kind) {
  case TaskKind::RefreshLibrary:
    return runRefresh(request, stopToken, progress, waitForResume);
  case TaskKind::IndexDownloadedPath:
    return runDownloadedIndex(request, stopToken, progress, waitForResume);
  case TaskKind::AndroidImport:
    return runAndroidImport(request, stopToken, progress, waitForResume);
  }
  throw std::runtime_error("Unknown library task");
}

TaskRunResult ChartLibraryOperations::runRefresh(
    const TaskRequest &request, const std::stop_token &stopToken,
    const TaskProgressCallback &progress,
    const TaskPauseCallback &waitForResume) {
  if (!waitForResume() || stopToken.stop_requested()) {
    return {.disposition = TaskRunDisposition::Paused, .detail = "Paused"};
  }

  auto session = dependencies_.repository.OpenSession();
  if (!session.has_value()) {
    throw std::runtime_error("Failed to open chart database");
  }
  session->EnsureSchema();

  std::vector<ChartEntry> entries;
  if (!request.folderToAdd.empty()) {
    progress({.current = 1,
              .total = 100,
              .stage = ChartScanProgressStage::Preparing},
             "Adding folder");
    if (!session->InsertEntry(request.folderToAdd, request.iosBookmark)) {
      throw std::runtime_error("Failed to add folder");
    }
    if (dependencies_.requestReload) {
      dependencies_.requestReload(true);
    }
    entries.push_back({.path = fspath_to_path_t(request.folderToAdd),
                       .iosBookmark = request.iosBookmark});
  }

  progress({.current = 2,
            .total = 100,
            .stage = ChartScanProgressStage::Preparing},
           "Importing difficulty tables");
  if (!waitForResume() || stopToken.stop_requested()) {
    return {.disposition = TaskRunDisposition::Paused, .detail = "Paused"};
  }
  seedDefaultDifficultyTablesIfNeeded(*session, stopToken, progress);
  if (!waitForResume() || stopToken.stop_requested()) {
    return {.disposition = TaskRunDisposition::Paused, .detail = "Paused"};
  }
  const int importedTables = dependencies_.importDifficultyTablesFromDirectory(
      *session, dependencies_.tablesDirectory);
  if (importedTables > 0 && !stopToken.stop_requested() &&
      dependencies_.requestReload) {
    dependencies_.requestReload(true);
  }

  if (entries.empty()) {
    entries = session->SelectEffectiveEntries();
  }
  if (stopToken.stop_requested()) {
    return {.disposition = TaskRunDisposition::Paused, .detail = "Paused"};
  }
  if (entries.empty() && dependencies_.selectInitialFolder) {
    const auto selected = dependencies_.selectInitialFolder();
    if (selected && !selected->empty()) {
      if (!session->InsertEntry(*selected)) {
        throw std::runtime_error("Failed to add selected library folder");
      }
      entries = session->SelectEffectiveEntries();
    }
  }
  if (entries.empty()) {
    return {.detail = "Complete"};
  }

  if (!waitForResume() || stopToken.stop_requested()) {
    return {.disposition = TaskRunDisposition::Paused, .detail = "Paused"};
  }

  chart_library_platform::refreshFolderAccess(entries);

  if (request.rebuildLibraryMetadata) {
    progress({.current = 8,
              .total = 100,
              .stage = ChartScanProgressStage::Preparing},
             "Clearing library caches");
    archive_file::appendDebugLogLine(
        "Manual library rebuild requested; clearing chart metadata caches.");
    if (!session->ClearChartMeta()) {
      throw std::runtime_error("Failed to clear chart metadata cache");
    }
  }

  std::vector<std::filesystem::path> roots;
  roots.reserve(entries.size());
  for (const auto &entry : entries) {
    if (stopToken.stop_requested()) {
      return {.disposition = TaskRunDisposition::Paused, .detail = "Paused"};
    }
    roots.push_back(chart_library_platform::resolveFolderEntryPath(entry));
  }

  bool checkpointPaused = false;
  auto checkpoint = [&] {
    const bool resumed = waitForResume();
    checkpointPaused = checkpointPaused || !resumed;
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

  if (stopToken.stop_requested() || checkpointPaused) {
    return {.disposition = TaskRunDisposition::Paused, .detail = "Paused"};
  }
  if (!result.completed) {
    throw std::runtime_error("Failed to refresh chart library");
  }
  if (dependencies_.requestReload) {
    dependencies_.requestReload(true);
  }
  return {.detail = "Complete"};
}

void ChartLibraryOperations::seedDefaultDifficultyTablesIfNeeded(
    ChartRepository::Session &session, const std::stop_token &stopToken,
    const TaskProgressCallback &progress) {
  if ((dependencies_.defaultDifficultyTablesSeeded &&
       dependencies_.defaultDifficultyTablesSeeded()) ||
      stopToken.stop_requested()) {
    return;
  }

  constexpr int totalTables =
      static_cast<int>(sizeof(kDefaultDifficultyTableUrls) /
                       sizeof(kDefaultDifficultyTableUrls[0]));
  int successfulTables = 0;
  bool allSucceeded = true;
  for (int i = 0; i < totalTables; ++i) {
    if (stopToken.stop_requested()) {
      return;
    }
    const char *url = kDefaultDifficultyTableUrls[i];
    progress({.current = i,
              .total = totalTables,
              .stage = ChartScanProgressStage::Preparing},
             "Adding default difficulty tables");
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
                   "Adding default table: " + detail);
        });
    if (ok) {
      ++successfulTables;
    } else {
      allSucceeded = false;
      SDL_Log("Failed to import default difficulty table %s: %s", url,
              errorMessage.empty() ? "unknown error" : errorMessage.c_str());
    }
  }

  if (stopToken.stop_requested()) {
    return;
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
}

TaskRunResult ChartLibraryOperations::runDownloadedIndex(
    const TaskRequest &request, const std::stop_token &stopToken,
    const TaskProgressCallback &progress,
    const TaskPauseCallback &waitForResume) {
  auto session = dependencies_.repository.OpenSession();
  if (!session.has_value()) {
    throw std::runtime_error("Failed to open chart database");
  }
  if (!session->EnsureSchema()) {
    throw std::runtime_error("Failed to prepare chart database");
  }
  if (!waitForResume() || stopToken.stop_requested()) {
    return {.disposition = TaskRunDisposition::Paused, .detail = "Paused"};
  }

  for (const auto &removedPath : request.downloadedRemovedPaths) {
    if (session->DeleteChartMetaInDirectory(removedPath) < 0) {
      if (dependencies_.requestReload) {
        dependencies_.requestReload(true);
      }
      throw std::runtime_error("Failed to reconcile replaced Find BMS files");
    }
  }

  const auto entries =
      main_menu_library::downloadedPathScanEntries(request.downloadedPath);
  if (entries.empty()) {
    throw std::runtime_error("Downloaded chart path is empty");
  }
  std::vector<std::filesystem::path> roots;
  roots.reserve(entries.size());
  for (const auto &entry : entries) {
    roots.push_back(chart_library_platform::resolveFolderEntryPath(entry));
  }

  bool checkpointPaused = false;
  auto checkpoint = [&] {
    const bool resumed = waitForResume();
    checkpointPaused = checkpointPaused || !resumed;
    return resumed;
  };
  auto publishScanProgress = [&](const ChartScanProgress &value) {
    progress(value, progressStageText(value.stage));
  };
  ChartLibraryScanner scanner;
  const ChartScanResult scanResult = scanner.ScanAddedWithResult(
      *session, roots, &stopToken, publishScanProgress, checkpoint);
  if (stopToken.stop_requested() || checkpointPaused) {
    return {.disposition = TaskRunDisposition::Paused, .detail = "Paused"};
  }
  if (!scanResult.completed) {
    if (dependencies_.requestReload) {
      dependencies_.requestReload(true);
    }
    throw std::runtime_error("Failed to index downloaded BMS charts");
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
    throw std::runtime_error("Downloaded BMS target was not parsed and indexed");
  }

  TaskRunResult result{.detail = "Complete"};
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
    throw std::runtime_error("Import failed: selected path is empty.");
  }

  std::error_code importPathError;
  const bool importingFolder =
      request.androidImportFolder ||
      std::filesystem::is_directory(importPath, importPathError);
  const std::string importType = importingFolder ? "folder" : "archive";
  const std::filesystem::path outputRoot =
      ChartRepository::DefaultBmsFolderPath();
  progress({.stage = ChartScanProgressStage::Preparing}, "Preparing import");
  archive_file::appendDebugLogLine(
      "Android import task requested: " + fspath_to_utf8(importPath) +
      " outputRoot=" + fspath_to_utf8(outputRoot));

  auto postImportProgress = [&](double fraction, const std::string &message) {
    progress({.current = static_cast<int>(
                  std::clamp(fraction, 0.0, 1.0) * 10000.0),
              .total = 10000,
              .stage = ChartScanProgressStage::Preparing},
             message.empty()
                 ? (importingFolder ? "Importing folder" : "Importing archive")
                 : message);
  };

  std::string errorMessage;
  std::error_code fsError;
  if (!Utils::EnsureDirectoryExists(outputRoot, fsError)) {
    throw std::runtime_error("Import failed: could not create BMS import "
                             "folder: " +
                             fsError.message());
  }

  std::filesystem::path outputFolder;
  if (importingFolder) {
    outputFolder = importPath;
    postImportProgress(0.90, "Refreshing library");
  } else {
    auto postUnzipProgress = [&](const archive_file::UnzipProgress &value) {
      postImportProgress(value.fraction * 0.90, value.message);
    };
    const auto unzippedArchive = archive_file::unzipArchiveFully(
        importPath, outputRoot, &errorMessage, &stopToken, postUnzipProgress);
    if (unzippedArchive.has_value()) {
      outputFolder = unzippedArchive->outputFolder;
    }
  }

  if (outputFolder.empty()) {
    throw std::runtime_error(
        stopToken.stop_requested()
            ? "Import cancelled"
            : (errorMessage.empty() ? "Import failed"
                                    : "Import failed: " + errorMessage));
  }
  if (stopToken.stop_requested()) {
    throw std::runtime_error("Import cancelled");
  }

  auto session = dependencies_.repository.OpenSession();
  if (!session.has_value()) {
    throw std::runtime_error("Imported " + importType +
                             ". Failed to refresh library.");
  }
  session->EnsureSchema();
  session->InsertEntry(outputRoot);

  std::vector<std::filesystem::path> roots{outputFolder};
  postImportProgress(0.92, "Refreshing library");
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
  const int changedCount = scanner.Scan(*session, roots, &stopToken,
                                        scanProgress, waitForResume);
  if (stopToken.stop_requested()) {
    throw std::runtime_error("Import cancelled");
  }

  if (!importingFolder) {
    std::filesystem::remove(importPath, fsError);
  }
  if (dependencies_.requestReload) {
    dependencies_.requestReload(true);
  }
  const std::string message =
      changedCount > 0 ? "Imported " + importType + ". Library refreshed."
                       : "Imported " + importType +
                             ". Library already current.";
  SDL_Log("Android import task result: %s", message.c_str());
  archive_file::appendDebugLogLine(message);
  return {.detail = "Complete"};
#else
  (void)request;
  (void)stopToken;
  (void)progress;
  (void)waitForResume;
  throw std::runtime_error("Android import task is unavailable.");
#endif
}

const char *ChartLibraryOperations::progressStageText(
    ChartScanProgressStage stage) noexcept {
  switch (stage) {
  case ChartScanProgressStage::Preparing:
    return "Preparing library scan";
  case ChartScanProgressStage::ScanningRoots:
    return "Scanning folders";
  case ChartScanProgressStage::IndexingArchives:
    return "Indexing archives";
  case ChartScanProgressStage::PreparingUpdates:
    return "Preparing chart updates";
  case ChartScanProgressStage::RemovingDeleted:
    return "Removing deleted charts";
  case ChartScanProgressStage::ParsingCharts:
    return "Parsing charts";
  case ChartScanProgressStage::ReadingArchive:
    return "Reading archive entries";
  }
  return "Refreshing library";
}

} // namespace chart_library_tasks
