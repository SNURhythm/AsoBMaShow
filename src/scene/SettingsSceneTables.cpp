#include "SettingsSceneShared.h"
#include "../SqliteRAII.h"
#include "../Utils.h"

#include <memory>

using namespace settings_scene;

#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
namespace {
class IOSScopedChartEntryAccess {
public:
  explicit IOSScopedChartEntryAccess(const ChartEntry &entry)
      : resolvedPath(formatChartEntryPath(entry)) {
    if (entry.iosBookmark.empty()) {
      return;
    }

    std::string bookmarkResolvedPath;
    handle = StartIOSSecurityScopedResource(
        resolvedPath, entry.iosBookmark, bookmarkResolvedPath, errorMessage);
    if (!bookmarkResolvedPath.empty()) {
      resolvedPath = bookmarkResolvedPath;
    }
  }

  IOSScopedChartEntryAccess(const IOSScopedChartEntryAccess &) = delete;
  IOSScopedChartEntryAccess &
  operator=(const IOSScopedChartEntryAccess &) = delete;

  ~IOSScopedChartEntryAccess() {
    if (handle != nullptr) {
      StopIOSSecurityScopedResource(handle);
    }
  }

  std::string resolvedPath;
  std::string errorMessage;

private:
  void *handle = nullptr;
};
} // namespace
#endif

void SettingsScene::loadDifficultyTables() {
  auto &dbHelper = ChartDBHelper::GetInstance();
  SqliteConnectionHandle settingsDbHandle(dbHelper.Connect());
  sqlite3 *settingsDb = settingsDbHandle.get();
  if (settingsDb == nullptr) {
    difficultyTables.clear();
    difficultyTableStatusMessage = "Could not open chart database.";
    difficultyTableStatusColor = {255, 177, 170, 255};
    return;
  }

  dbHelper.CreateDifficultyTableTables(settingsDb);
  difficultyTables = dbHelper.SelectDifficultyTables(settingsDb);

  if (pendingDeleteDifficultyTableId != 0) {
    const auto it =
        std::find_if(difficultyTables.begin(), difficultyTables.end(),
                     [this](const DifficultyTableInfo &table) {
                       return table.id == pendingDeleteDifficultyTableId;
                     });
    if (it == difficultyTables.end()) {
      pendingDeleteDifficultyTableId = 0;
    }
  }
}

void SettingsScene::loadChartEntries() {
  auto &dbHelper = ChartDBHelper::GetInstance();
  SqliteConnectionHandle settingsDbHandle(dbHelper.Connect());
  sqlite3 *settingsDb = settingsDbHandle.get();
  if (settingsDb == nullptr) {
    chartEntries.clear();
    chartFolderStatusMessage = "Could not open chart database.";
    chartFolderStatusColor = {255, 177, 170, 255};
    return;
  }

  dbHelper.CreateEntriesTable(settingsDb);
  chartEntries = dbHelper.SelectEffectiveEntries(settingsDb);

  if (!pendingDeleteChartEntryPath.empty()) {
    const auto it = std::find_if(chartEntries.begin(), chartEntries.end(),
                                 [this](const ChartEntry &entry) {
                                   return formatChartEntryPath(entry) ==
                                          pendingDeleteChartEntryPath;
                                 });
    if (it == chartEntries.end()) {
      pendingDeleteChartEntryPath.clear();
    }
  }
}

void SettingsScene::refreshChartEntryBackupStatuses() {
  chartEntryICloudBackupExcluded.clear();

#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  for (const auto &entry : chartEntries) {
    const std::string entryPathText = formatChartEntryPath(entry);
    IOSScopedChartEntryAccess access(entry);
    if (!access.errorMessage.empty()) {
      SDL_Log("Failed to open folder access for %s: %s",
              entryPathText.c_str(), access.errorMessage.c_str());
    }

    bool excluded = false;
    std::string errorMessage;
    if (!GetIOSFileExcludedFromBackup(access.resolvedPath, excluded,
                                      errorMessage)) {
      SDL_Log("Failed to read iCloud Backup setting for %s: %s",
              access.resolvedPath.c_str(), errorMessage.c_str());
      continue;
    }
    chartEntryICloudBackupExcluded[entryPathText] = excluded;
  }
#endif
}

void SettingsScene::toggleChartEntryICloudBackup(
    const std::string &entryPathText) {
  auto setFolderStatus = [this](const std::string &message,
                                const SDL_Color &color) {
    chartFolderStatusMessage = message;
    chartFolderStatusColor = color;
    if (chartFolderStatusText != nullptr) {
      chartFolderStatusText->setText(chartFolderStatusMessage);
      chartFolderStatusText->setColor(chartFolderStatusColor);
    }
  };

#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  const auto entryIt =
      std::find_if(chartEntries.begin(), chartEntries.end(),
                   [&entryPathText](const ChartEntry &entry) {
                     return formatChartEntryPath(entry) == entryPathText;
                   });
  if (entryIt == chartEntries.end()) {
    setFolderStatus("Folder entry was not found.", {255, 177, 170, 255});
    return;
  }

  IOSScopedChartEntryAccess access(*entryIt);
  if (!access.errorMessage.empty()) {
    SDL_Log("Failed to open folder access for %s: %s",
            entryPathText.c_str(), access.errorMessage.c_str());
  }

  bool excluded = false;
  std::string errorMessage;
  if (!GetIOSFileExcludedFromBackup(access.resolvedPath, excluded,
                                    errorMessage)) {
    setFolderStatus("Could not check iCloud Backup: " + errorMessage,
                    {255, 177, 170, 255});
    return;
  }

  const bool shouldExclude = !excluded;
  if (!SetIOSFileExcludedFromBackup(access.resolvedPath, shouldExclude,
                                    errorMessage)) {
    setFolderStatus("Could not update iCloud Backup: " + errorMessage,
                    {255, 177, 170, 255});
    return;
  }

  chartEntryICloudBackupExcluded[entryPathText] = shouldExclude;
  setFolderStatus(shouldExclude ? "iCloud Backup disabled for folder."
                                : "iCloud Backup enabled for folder.",
                  {181, 228, 165, 255});
  lastLayoutWidth = -1;
#else
  (void)entryPathText;
  setFolderStatus("iCloud Backup settings are only available on iOS.",
                  {255, 177, 170, 255});
#endif
}

void SettingsScene::requestDifficultyTableStatus(const std::string &text,
                                                 const SDL_Color &color,
                                                 bool reloadTables) {
  std::lock_guard<std::mutex> lock(difficultyTableStatusMutex);
  pendingDifficultyTableStatus = true;
  pendingDifficultyTableStatusText = text;
  pendingDifficultyTableStatusColor = color;
  pendingDifficultyTableReload = pendingDifficultyTableReload || reloadTables;
}

void SettingsScene::requestChartFolderStatus(const std::string &text,
                                             const SDL_Color &color,
                                             bool reloadTables) {
  std::lock_guard<std::mutex> lock(difficultyTableStatusMutex);
  pendingChartFolderStatus = true;
  pendingChartFolderStatusText = text;
  pendingChartFolderStatusColor = color;
  pendingDifficultyTableReload = pendingDifficultyTableReload || reloadTables;
}

void SettingsScene::requestDifficultyTableImportProgress(
    int current, int total, const std::string &tableName,
    const std::string &statusText, bool finished, bool succeeded) {
  std::lock_guard<std::mutex> lock(difficultyTableStatusMutex);
  pendingDifficultyTableImportProgress = true;
  pendingDifficultyTableImportCurrent = current;
  pendingDifficultyTableImportTotal = total;
  pendingDifficultyTableImportName = tableName;
  pendingDifficultyTableImportStatusText = statusText;
  pendingDifficultyTableImportFinished = finished;
  pendingDifficultyTableImportSucceeded = finished && succeeded;
}

void SettingsScene::applyPendingDifficultyTableUpdates() {
  bool shouldReload = false;
  bool shouldRefreshImportModal = false;
  {
    std::lock_guard<std::mutex> lock(difficultyTableStatusMutex);
    if (pendingDifficultyTableStatus) {
      difficultyTableStatusMessage = pendingDifficultyTableStatusText;
      difficultyTableStatusColor = pendingDifficultyTableStatusColor;
      if (difficultyTableStatusText != nullptr) {
        difficultyTableStatusText->setText(difficultyTableStatusMessage);
        difficultyTableStatusText->setColor(difficultyTableStatusColor);
      }
      pendingDifficultyTableStatus = false;
    }
    if (pendingChartFolderStatus) {
      chartFolderStatusMessage = pendingChartFolderStatusText;
      chartFolderStatusColor = pendingChartFolderStatusColor;
      if (chartFolderStatusText != nullptr) {
        chartFolderStatusText->setText(chartFolderStatusMessage);
        chartFolderStatusText->setColor(chartFolderStatusColor);
      }
      pendingChartFolderStatus = false;
    }
    if (pendingDifficultyTableImportProgress) {
      difficultyTableImportCurrent = pendingDifficultyTableImportCurrent;
      difficultyTableImportTotal = pendingDifficultyTableImportTotal;
      difficultyTableImportName = pendingDifficultyTableImportName;
      difficultyTableImportStatusMessage =
          pendingDifficultyTableImportStatusText;
      difficultyTableImportFinished = pendingDifficultyTableImportFinished;
      difficultyTableImportSucceeded = pendingDifficultyTableImportSucceeded;
      difficultyTableImportModalVisible = true;
      pendingDifficultyTableImportProgress = false;
      shouldRefreshImportModal = true;
    }
    shouldReload = pendingDifficultyTableReload;
    pendingDifficultyTableReload = false;
  }

  if (shouldReload) {
    loadDifficultyTables();
    loadChartEntries();
    observedLibraryRevision =
        ChartDBHelper::GetInstance().GetLibraryRevision();
    lastLayoutWidth = -1;
  }
  if (shouldRefreshImportModal) {
    refreshDifficultyTableImportModal();
  }
}

void SettingsScene::refreshTablesIfLibraryChanged() {
  const std::uint64_t revision =
      ChartDBHelper::GetInstance().GetLibraryRevision();
  if (revision == observedLibraryRevision) {
    return;
  }

  observedLibraryRevision = revision;
  if (activeTab != SettingsTab::DifficultyTables &&
      activeTab != SettingsTab::BmsLibrary) {
    return;
  }

  if (activeTab == SettingsTab::DifficultyTables) {
    loadDifficultyTables();
  } else {
    loadChartEntries();
    refreshChartEntryBackupStatuses();
  }
  lastLayoutWidth = -1;
}

void SettingsScene::refreshDifficultyTableImportModal() {
  if (difficultyTableImportModalRoot == nullptr) {
    return;
  }

  difficultyTableImportModalRoot->setSize(rendering::window_width,
                                          rendering::window_height);
  difficultyTableImportModalRoot->setVisible(difficultyTableImportModalVisible);
  if (!difficultyTableImportModalVisible) {
    return;
  }

  const bool finished = difficultyTableImportFinished;
  const bool succeeded = difficultyTableImportSucceeded;
  const int total = std::max(0, difficultyTableImportTotal);
  const int current =
      total > 0 ? std::clamp(difficultyTableImportCurrent, 0, total) : 0;
  const float progressPercent =
      total > 0
          ? (static_cast<float>(current) / static_cast<float>(total)) * 100.0f
          : 0.0f;

  if (difficultyTableImportTitleText != nullptr) {
    difficultyTableImportTitleText->setText(
        !finished ? "Importing Difficulty Tables"
                  : (succeeded ? "Import Complete" : "Import Failed"));
  }
  if (difficultyTableImportStatusText != nullptr) {
    if (!difficultyTableImportStatusMessage.empty()) {
      difficultyTableImportStatusText->setText(
          difficultyTableImportStatusMessage);
    } else {
      difficultyTableImportStatusText->setText(
          !finished ? "Downloading and importing tables..."
                    : (succeeded ? "Import finished." : "Import failed."));
    }
  }
  if (difficultyTableImportTableText != nullptr) {
    difficultyTableImportTableText->setText(
        difficultyTableImportName.empty()
            ? "Current table: Resolving table URL"
            : "Current table: " + difficultyTableImportName);
  }
  if (difficultyTableImportProgressText != nullptr) {
    difficultyTableImportProgressText->setText(
        formatImportProgressText(current, total));
  }
  if (difficultyTableImportProgressFill != nullptr) {
    difficultyTableImportProgressFill->setWidthPercent(progressPercent);
    difficultyTableImportProgressFill->setBackgroundColor(
        finished && !succeeded ? Color(191, 82, 92, 255)
                               : Color(97, 157, 142, 255));
  }
  if (difficultyTableImportCloseButton != nullptr) {
    const bool canClose = finished;
    difficultyTableImportCloseButton->setVisible(canClose);
  }

  if (rootLayout != nullptr) {
    rootLayout->applyYogaLayout();
  } else {
    difficultyTableImportModalRoot->applyYogaLayout();
  }
}

void SettingsScene::hideDifficultyTableImportModal() {
  if (difficultyTableJobRunning.load() && !difficultyTableImportFinished) {
    return;
  }
  difficultyTableImportModalVisible = false;
  refreshDifficultyTableImportModal();
}

void SettingsScene::addDifficultyTableFromUrl() {
  if (difficultyTableJobRunning) {
    return;
  }

  const std::string url =
      tableUrlInput != nullptr ? tableUrlInput->getText() : tableUrlText;
  if (url.empty()) {
    difficultyTableStatusMessage = "Enter a table webpage URL first.";
    difficultyTableStatusColor = {255, 177, 170, 255};
    if (difficultyTableStatusText != nullptr) {
      difficultyTableStatusText->setText(difficultyTableStatusMessage);
      difficultyTableStatusText->setColor(difficultyTableStatusColor);
    }
    return;
  }

  if (difficultyTableJobThread.joinable()) {
    difficultyTableJobThread.join();
  }

  difficultyTableJobRunning = true;
  pendingDeleteDifficultyTableId = 0;
  pendingDeleteChartEntryPath.clear();
  difficultyTableStatusMessage = "Adding table...";
  difficultyTableStatusColor = {239, 244, 251, 255};
  difficultyTableImportModalVisible = true;
  difficultyTableImportFinished = false;
  difficultyTableImportSucceeded = false;
  difficultyTableImportCurrent = 0;
  difficultyTableImportTotal = 1;
  difficultyTableImportName = url;
  difficultyTableImportStatusMessage = "Preparing import...";
  if (difficultyTableStatusText != nullptr) {
    difficultyTableStatusText->setText(difficultyTableStatusMessage);
    difficultyTableStatusText->setColor(difficultyTableStatusColor);
  }
  refreshDifficultyTableImportModal();

  difficultyTableJobThread = std::jthread([this,
                                           url](const std::stop_token &token) {
    auto &dbHelper = ChartDBHelper::GetInstance();
    SqliteConnectionHandle settingsDbHandle(dbHelper.Connect());
    sqlite3 *settingsDb = settingsDbHandle.get();
    if (settingsDb == nullptr) {
      if (!token.stop_requested()) {
        difficultyTableJobRunning = false;
        requestDifficultyTableImportProgress(
            0, 1, url, "Could not open chart database.", true, false);
        requestDifficultyTableStatus("Could not open chart database.",
                                     {255, 177, 170, 255});
      }
      return;
    }

    dbHelper.CreateDifficultyTableTables(settingsDb);
    std::string errorMessage;
    DifficultyTableImportProgress lastProgress{0, 1, url};
    auto progressCallback = [this, &lastProgress, &token](
                                const DifficultyTableImportProgress &progress) {
      if (token.stop_requested()) {
        return;
      }
      lastProgress = progress;
      requestDifficultyTableImportProgress(
          progress.current, progress.total, progress.tableName,
          "Downloading and importing tables...", false);
    };
    const bool imported = dbHelper.ImportDifficultyTableFromUrl(
        settingsDb, url, &errorMessage, progressCallback);

    if (token.stop_requested()) {
      difficultyTableJobRunning = false;
      return;
    }

    difficultyTableJobRunning = false;
    const std::string finalMessage =
        imported ? (errorMessage.empty() ? "Table added." : errorMessage)
                 : (errorMessage.empty() ? "Add failed." : errorMessage);
    requestDifficultyTableImportProgress(
        lastProgress.current, lastProgress.total, lastProgress.tableName,
        finalMessage, true, imported);
    requestDifficultyTableStatus(finalMessage,
                                 imported ? SDL_Color{181, 228, 165, 255}
                                          : SDL_Color{255, 177, 170, 255},
                                 imported);
  });
}

void SettingsScene::updateDifficultyTableFromSource(int tableId) {
  if (difficultyTableJobRunning || tableId <= 0) {
    return;
  }

  if (difficultyTableJobThread.joinable()) {
    difficultyTableJobThread.join();
  }

  difficultyTableJobRunning = true;
  pendingDeleteDifficultyTableId = 0;
  pendingDeleteChartEntryPath.clear();
  difficultyTableStatusMessage = "Updating table...";
  difficultyTableStatusColor = {239, 244, 251, 255};
  if (difficultyTableStatusText != nullptr) {
    difficultyTableStatusText->setText(difficultyTableStatusMessage);
    difficultyTableStatusText->setColor(difficultyTableStatusColor);
  }

  difficultyTableJobThread = std::jthread([this, tableId](
                                              const std::stop_token &token) {
    auto &dbHelper = ChartDBHelper::GetInstance();
    SqliteConnectionHandle settingsDbHandle(dbHelper.Connect());
    sqlite3 *settingsDb = settingsDbHandle.get();
    if (settingsDb == nullptr) {
      if (!token.stop_requested()) {
        requestDifficultyTableStatus("Could not open chart database.",
                                     {255, 177, 170, 255});
        difficultyTableJobRunning = false;
      }
      return;
    }

    std::string errorMessage;
    const bool updated = dbHelper.UpdateDifficultyTableFromSourceUrl(
        settingsDb, tableId, &errorMessage);

    if (token.stop_requested()) {
      difficultyTableJobRunning = false;
      return;
    }

    requestDifficultyTableStatus(
        updated ? "Table updated."
                : (errorMessage.empty() ? "Update failed." : errorMessage),
        updated ? SDL_Color{181, 228, 165, 255} : SDL_Color{255, 177, 170, 255},
        updated);
    difficultyTableJobRunning = false;
  });
}

void SettingsScene::deleteDifficultyTable(int tableId) {
  if (difficultyTableJobRunning || tableId <= 0) {
    return;
  }

  if (pendingDeleteDifficultyTableId != tableId) {
    pendingDeleteDifficultyTableId = tableId;
    pendingDeleteChartEntryPath.clear();
    difficultyTableStatusMessage = "Tap Confirm on that table to delete it.";
    difficultyTableStatusColor = {255, 213, 151, 255};
    lastLayoutWidth = -1;
    return;
  }

  if (difficultyTableJobThread.joinable()) {
    difficultyTableJobThread.join();
  }

  difficultyTableJobRunning = true;
  pendingDeleteDifficultyTableId = 0;
  difficultyTableStatusMessage = "Deleting table...";
  difficultyTableStatusColor = {239, 244, 251, 255};
  if (difficultyTableStatusText != nullptr) {
    difficultyTableStatusText->setText(difficultyTableStatusMessage);
    difficultyTableStatusText->setColor(difficultyTableStatusColor);
  }

  difficultyTableJobThread = std::jthread([this, tableId](
                                              const std::stop_token &token) {
    auto &dbHelper = ChartDBHelper::GetInstance();
    SqliteConnectionHandle settingsDbHandle(dbHelper.Connect());
    sqlite3 *settingsDb = settingsDbHandle.get();
    if (settingsDb == nullptr) {
      if (!token.stop_requested()) {
        requestDifficultyTableStatus("Could not open chart database.",
                                     {255, 177, 170, 255});
        difficultyTableJobRunning = false;
      }
      return;
    }

    const bool deleted = dbHelper.DeleteDifficultyTable(settingsDb, tableId);

    if (token.stop_requested()) {
      difficultyTableJobRunning = false;
      return;
    }

    requestDifficultyTableStatus(deleted ? "Table deleted." : "Delete failed.",
                                 deleted ? SDL_Color{181, 228, 165, 255}
                                         : SDL_Color{255, 177, 170, 255},
                                 deleted);
    difficultyTableJobRunning = false;
  });
}

void SettingsScene::refreshChartLibrary() {
  if (difficultyTableJobRunning) {
    return;
  }

  if (difficultyTableJobThread.joinable()) {
    difficultyTableJobThread.join();
  }

  difficultyTableJobRunning = true;
  pendingDeleteDifficultyTableId = 0;
  pendingDeleteChartEntryPath.clear();
  chartFolderStatusMessage = "Refreshing chart list...";
  chartFolderStatusColor = {239, 244, 251, 255};
  if (chartFolderStatusText != nullptr) {
    chartFolderStatusText->setText(chartFolderStatusMessage);
    chartFolderStatusText->setColor(chartFolderStatusColor);
  }

  difficultyTableJobThread =
      std::jthread([this](const std::stop_token &token) {
        auto &dbHelper = ChartDBHelper::GetInstance();
        SqliteConnectionHandle settingsDbHandle(dbHelper.Connect());
        sqlite3 *settingsDb = settingsDbHandle.get();
        if (settingsDb == nullptr) {
          if (!token.stop_requested()) {
            requestChartFolderStatus("Could not open chart database.",
                                     {255, 177, 170, 255});
            difficultyTableJobRunning = false;
          }
          return;
        }

        dbHelper.CreateChartMetaTable(settingsDb);
        dbHelper.CreateEntriesTable(settingsDb);
        auto entries = dbHelper.SelectEffectiveEntries(settingsDb);
        if (entries.empty()) {
          const auto defaultPath = ChartDBHelper::DefaultBmsFolderPath();
          std::error_code errorCode;
          std::filesystem::create_directories(defaultPath, errorCode);
          dbHelper.InsertEntry(settingsDb, defaultPath);
          entries = dbHelper.SelectEffectiveEntries(settingsDb);
        }

        std::vector<std::filesystem::path> roots;
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
        std::vector<std::unique_ptr<IOSScopedChartEntryAccess>> accessHandles;
        for (const auto &entry : entries) {
          if (token.stop_requested()) {
            break;
          }
          auto access = std::make_unique<IOSScopedChartEntryAccess>(entry);
          if (!access->errorMessage.empty()) {
            SDL_Log("Failed to open folder access for %s: %s",
                    path_t_to_utf8(entry.path).c_str(),
                    access->errorMessage.c_str());
          }
          roots.emplace_back(utf8_to_path_t(access->resolvedPath));
          accessHandles.push_back(std::move(access));
        }
#else
        roots.reserve(entries.size());
        for (const auto &entry : entries) {
          roots.emplace_back(entry.path);
        }
#endif

        const int changedCount =
            token.stop_requested()
                ? -1
                : dbHelper.ScanChartRoots(settingsDb, roots, &token);

        if (token.stop_requested()) {
          difficultyTableJobRunning = false;
          return;
        }

        const bool succeeded = changedCount >= 0;
        std::string statusText;
        if (!succeeded) {
          statusText = "Refresh failed.";
        } else if (changedCount == 0) {
          statusText = "Chart list refreshed. No changes found.";
        } else if (changedCount == 1) {
          statusText = "Chart list refreshed. Updated 1 chart entry.";
        } else {
          statusText = "Chart list refreshed. Updated " +
                       std::to_string(changedCount) + " chart entries.";
        }
        requestChartFolderStatus(
            statusText,
            succeeded ? SDL_Color{181, 228, 165, 255}
                      : SDL_Color{255, 177, 170, 255},
            true);
        difficultyTableJobRunning = false;
      });
}

void SettingsScene::deleteChartEntry(const std::string &entryPathText) {
  if (difficultyTableJobRunning || entryPathText.empty()) {
    return;
  }

#if TARGET_OS_ANDROID
  if (ChartDBHelper::IsDefaultBmsFolderPath(
          std::filesystem::path(utf8_to_path_t(entryPathText)))) {
    chartFolderStatusMessage = "The default BMS folder is built in.";
    chartFolderStatusColor = ui_theme::sdl(ui_theme::textSecondary());
    if (chartFolderStatusText != nullptr) {
      chartFolderStatusText->setText(chartFolderStatusMessage);
      chartFolderStatusText->setColor(chartFolderStatusColor);
    }
    pendingDeleteChartEntryPath.clear();
    return;
  }
#endif

  if (pendingDeleteChartEntryPath != entryPathText) {
    pendingDeleteChartEntryPath = entryPathText;
    pendingDeleteDifficultyTableId = 0;
    lastLayoutWidth = -1;
    return;
  }

  if (difficultyTableJobThread.joinable()) {
    difficultyTableJobThread.join();
  }

  difficultyTableJobRunning = true;
  pendingDeleteChartEntryPath.clear();
  chartFolderStatusMessage = "Removing folder...";
  chartFolderStatusColor = {239, 244, 251, 255};
  if (chartFolderStatusText != nullptr) {
    chartFolderStatusText->setText(chartFolderStatusMessage);
    chartFolderStatusText->setColor(chartFolderStatusColor);
  }

  difficultyTableJobThread =
      std::jthread([this, entryPathText](const std::stop_token &token) {
        auto &dbHelper = ChartDBHelper::GetInstance();
        SqliteConnectionHandle settingsDbHandle(dbHelper.Connect());
        sqlite3 *settingsDb = settingsDbHandle.get();
        if (settingsDb == nullptr) {
          if (!token.stop_requested()) {
            requestChartFolderStatus("Could not open chart database.",
                                     {255, 177, 170, 255});
            difficultyTableJobRunning = false;
          }
          return;
        }

        dbHelper.CreateChartMetaTable(settingsDb);
        dbHelper.CreateEntriesTable(settingsDb);

        const auto entries = dbHelper.SelectEffectiveEntries(settingsDb);
        const auto entryIt =
            std::find_if(entries.begin(), entries.end(),
                         [&entryPathText](const ChartEntry &entry) {
                           return formatChartEntryPath(entry) == entryPathText;
                         });

        if (entryIt == entries.end() || !entryIt->removable) {
          if (!token.stop_requested()) {
            requestChartFolderStatus(
                entryIt == entries.end()
                    ? "Folder entry was not found."
                    : "The default BMS folder is built in.",
                                     {255, 177, 170, 255}, true);
            difficultyTableJobRunning = false;
          }
          return;
        }

        const std::filesystem::path entryPath(entryIt->path);
        dbHelper.BeginTransaction(settingsDb);
        const int removedChartCount =
            dbHelper.DeleteChartMetaInDirectory(settingsDb, entryPath);
        const bool removed = removedChartCount >= 0 &&
                             dbHelper.DeleteEntry(settingsDb, entryPath);
        if (removed) {
          dbHelper.CommitTransaction(settingsDb);
        } else {
          sqlite3_exec(settingsDb, "ROLLBACK", nullptr, nullptr, nullptr);
        }

        if (token.stop_requested()) {
          difficultyTableJobRunning = false;
          return;
        }

        std::string statusText;
        if (removed) {
          statusText = removedChartCount == 1
                           ? "Folder removed. Removed 1 cached chart."
                           : "Folder removed. Removed " +
                                 std::to_string(removedChartCount) +
                                 " cached charts.";
        } else {
          statusText = "Remove failed.";
        }
        requestChartFolderStatus(statusText,
                                 removed ? SDL_Color{181, 228, 165, 255}
                                         : SDL_Color{255, 177, 170, 255},
                                 true);
        difficultyTableJobRunning = false;
      });
}
