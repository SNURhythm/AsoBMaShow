#include "../i18n/Localization.h"
#include "SettingsSceneShared.h"
#include "DifficultyTableUrlCompletion.h"
#include "../ChartLibraryScanner.h"
#include "../DifficultyTableImporter.h"
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
  auto session = context.chartRepository.OpenSession();
  if (!session.has_value()) {
    difficultyTables.clear();
    difficultyTableStatusMessage = i18n::tr("settings.difficulty_tables.could_not_open_chart_database.message");
    difficultyTableStatusColor = {255, 177, 170, 255};
    return;
  }

  difficultyTables = session->SelectDifficultyTables();

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
  auto session = context.chartRepository.OpenSession();
  if (!session.has_value()) {
    chartEntries.clear();
    chartFolderStatusMessage = i18n::tr("settings.difficulty_tables.could_not_open_chart_database.message");
    chartFolderStatusColor = {255, 177, 170, 255};
    return;
  }

  chartEntries = session->SelectEffectiveEntries();

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
    setFolderStatus(i18n::tr("settings.difficulty_tables.folder_entry_not_found.message"), {255, 177, 170, 255});
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
    setFolderStatus(i18n::tr("settings.difficulty_tables.could_not_check_i_cloud_backup.prefix") + errorMessage,
                    {255, 177, 170, 255});
    return;
  }

  const bool shouldExclude = !excluded;
  if (!SetIOSFileExcludedFromBackup(access.resolvedPath, shouldExclude,
                                    errorMessage)) {
    setFolderStatus(i18n::tr("settings.difficulty_tables.could_not_update_i_cloud_backup.prefix") + errorMessage,
                    {255, 177, 170, 255});
    return;
  }

  chartEntryICloudBackupExcluded[entryPathText] = shouldExclude;
  setFolderStatus(shouldExclude ? i18n::tr("settings.difficulty_tables.i_cloud_backup_disabled_folder.message")
                                : i18n::tr("settings.difficulty_tables.i_cloud_backup_enabled_folder.message"),
                  {181, 228, 165, 255});
  lastLayoutWidth = -1;
#else
  (void)entryPathText;
  setFolderStatus(i18n::tr("settings.difficulty_tables.icloud_backup.ios_only_notice"),
                  {255, 177, 170, 255});
#endif
}

void SettingsScene::applyPendingDifficultyTableUpdates() {
  auto pending = libraryTask.takeUpdates();
  if (pending.tableStatus) {
    difficultyTableStatusMessage = std::move(pending.tableStatus->text);
    difficultyTableStatusColor = pending.tableStatus->succeeded
                                     ? SDL_Color{181, 228, 165, 255}
                                     : SDL_Color{255, 177, 170, 255};
    if (difficultyTableStatusText != nullptr) {
      difficultyTableStatusText->setText(difficultyTableStatusMessage);
      difficultyTableStatusText->setColor(difficultyTableStatusColor);
    }
  }
  if (pending.folderStatus) {
    chartFolderStatusMessage = std::move(pending.folderStatus->text);
    chartFolderStatusColor = pending.folderStatus->succeeded
                                ? SDL_Color{181, 228, 165, 255}
                                : SDL_Color{255, 177, 170, 255};
    if (chartFolderStatusText != nullptr) {
      chartFolderStatusText->setText(chartFolderStatusMessage);
      chartFolderStatusText->setColor(chartFolderStatusColor);
    }
  }
  if (pending.importProgress) {
    const auto &progress = *pending.importProgress;
    difficultyTableImportCurrent = progress.current;
    difficultyTableImportTotal = progress.total;
    difficultyTableImportName = progress.tableName;
    difficultyTableImportStatusMessage = progress.statusText;
    difficultyTableImportFinished = progress.finished;
    difficultyTableImportSucceeded = progress.succeeded;
    difficultyTableImportModalVisible = true;
  }

  if (pending.reload) {
    loadDifficultyTables();
    loadChartEntries();
    observedLibraryRevision = context.chartRepository.GetLibraryRevision();
    lastLayoutWidth = -1;
  }
  if (pending.importProgress) {
    const auto &progress = *pending.importProgress;
    const bool clearedUrl = settings_ui::applyDifficultyTableUrlCompletion(
        progress.finished, progress.succeeded, progress.submittedUrl, tableUrlText);
    if (clearedUrl && tableUrlInput != nullptr) {
      tableUrlInput->setEditingText(tableUrlText);
    }
    refreshDifficultyTableImportModal();
  }
}

void SettingsScene::refreshTablesIfLibraryChanged() {
  const std::uint64_t revision = context.chartRepository.GetLibraryRevision();
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
    difficultyTableImportTitleText->setLocalizedText(
        !finished ? i18n::message("settings.difficulty_tables.importing_difficulty_tables.label")
                  : (succeeded ? i18n::message("settings.difficulty_tables.import_complete.label") : i18n::message("settings.difficulty_tables.import_failed.label")));
  }
  if (difficultyTableImportStatusText != nullptr) {
    if (!difficultyTableImportStatusMessage.empty()) {
      difficultyTableImportStatusText->setText(
          difficultyTableImportStatusMessage);
    } else {
      difficultyTableImportStatusText->setLocalizedText(
          !finished ? i18n::message("settings.difficulty_tables.downloading_importing_tables.progress")
                    : (succeeded ? i18n::message("settings.difficulty_tables.import_finished.message") : i18n::message("settings.difficulty_tables.import_failed.message")));
    }
  }
  if (difficultyTableImportTableText != nullptr) {
    difficultyTableImportTableText->setLocalizedText(
        difficultyTableImportName.empty()
            ? i18n::message("settings.difficulty_tables.current_table_resolving_table_url.label")
            : i18n::message("settings.difficulty_tables.current_table",
                            {{"prefix", i18n::message("settings.difficulty_tables.current_table.prefix")},
                             {"name", difficultyTableImportName}}));
  }
  if (difficultyTableImportProgressText != nullptr) {
    difficultyTableImportProgressText->setLocalizedText(
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
  if (libraryTask.running() && !difficultyTableImportFinished) {
    return;
  }
  difficultyTableImportModalVisible = false;
  refreshDifficultyTableImportModal();
}

void SettingsScene::addDifficultyTableFromUrl() {
  if (libraryTask.running()) {
    return;
  }

  const std::string url =
      tableUrlInput != nullptr ? tableUrlInput->getText() : tableUrlText;
  if (url.empty()) {
    difficultyTableStatusMessage = i18n::tr("settings.difficulty_tables.enter_table_webpage_url_first.message");
    difficultyTableStatusColor = {255, 177, 170, 255};
    if (difficultyTableStatusText != nullptr) {
      difficultyTableStatusText->setText(difficultyTableStatusMessage);
      difficultyTableStatusText->setColor(difficultyTableStatusColor);
    }
    return;
  }

  pendingDeleteDifficultyTableId = 0;
  pendingDeleteChartEntryPath.clear();
  difficultyTableStatusMessage = i18n::tr("settings.difficulty_tables.adding_table.progress");
  difficultyTableStatusColor = {239, 244, 251, 255};
  difficultyTableImportModalVisible = true;
  difficultyTableImportFinished = false;
  difficultyTableImportSucceeded = false;
  difficultyTableImportCurrent = 0;
  difficultyTableImportTotal = 1;
  difficultyTableImportName = url;
  difficultyTableImportStatusMessage = i18n::tr("settings.difficulty_tables.preparing_import.progress");
  if (difficultyTableStatusText != nullptr) {
    difficultyTableStatusText->setText(difficultyTableStatusMessage);
    difficultyTableStatusText->setColor(difficultyTableStatusColor);
  }
  refreshDifficultyTableImportModal();

  libraryTask.start([&repository = context.chartRepository, url](
                        const std::stop_token &token,
                        const SettingsLibraryTask::Publisher &updates) {
    auto session = repository.OpenSession();
    if (!session.has_value()) {
      if (!token.stop_requested()) {
        updates.importProgress({
            0, 1, url, i18n::tr("settings.difficulty_tables.could_not_open_chart_database.message"), true, false, url});
        updates.tableStatus(i18n::tr("settings.difficulty_tables.could_not_open_chart_database.message"), false);
      }
      return;
    }

    std::string errorMessage;
    DifficultyTableImportProgress lastProgress{0, 1, url};
    auto progressCallback = [&updates, &lastProgress, &token](
                                const DifficultyTableImportProgress &progress) {
      if (token.stop_requested()) {
        return;
      }
      lastProgress = progress;
      updates.importProgress({
          progress.current, progress.total, progress.tableName,
          i18n::tr("settings.difficulty_tables.downloading_importing_tables.progress"), false, false, {}});
    };
    DifficultyTableImporter importer;
    const bool imported = importer.ImportFromUrl(
        *session, url, &errorMessage, progressCallback);

    if (token.stop_requested()) {
      return;
    }

    const std::string finalMessage =
        imported ? (errorMessage.empty() ? i18n::tr("settings.difficulty_tables.table_added.message") : errorMessage)
                 : (errorMessage.empty() ? i18n::tr("settings.difficulty_tables.add_failed.message") : errorMessage);
    updates.importProgress({
        lastProgress.current, lastProgress.total, lastProgress.tableName,
        finalMessage, true, imported, url});
    updates.tableStatus(finalMessage, imported, imported);
  });
}

void SettingsScene::updateDifficultyTableFromSource(int tableId) {
  if (libraryTask.running() || tableId <= 0) {
    return;
  }

  pendingDeleteDifficultyTableId = 0;
  pendingDeleteChartEntryPath.clear();
  difficultyTableStatusMessage = i18n::tr("settings.difficulty_tables.updating_table.progress");
  difficultyTableStatusColor = {239, 244, 251, 255};
  if (difficultyTableStatusText != nullptr) {
    difficultyTableStatusText->setText(difficultyTableStatusMessage);
    difficultyTableStatusText->setColor(difficultyTableStatusColor);
  }

  libraryTask.start([&repository = context.chartRepository, tableId](
                        const std::stop_token &token,
                        const SettingsLibraryTask::Publisher &updates) {
    auto session = repository.OpenSession();
    if (!session.has_value()) {
      if (!token.stop_requested()) {
        updates.tableStatus(i18n::tr("settings.difficulty_tables.could_not_open_chart_database.message"), false);
      }
      return;
    }

    std::string errorMessage;
    DifficultyTableImporter importer;
    const bool updated =
        importer.UpdateFromSourceUrl(*session, tableId, &errorMessage);

    if (token.stop_requested()) {
      return;
    }

    updates.tableStatus(
        updated ? i18n::tr("settings.difficulty_tables.table_updated.message")
                : (errorMessage.empty() ? i18n::tr("settings.difficulty_tables.update_failed.message") : errorMessage),
        updated, updated);
  });
}

void SettingsScene::deleteDifficultyTable(int tableId) {
  if (libraryTask.running() || tableId <= 0) {
    return;
  }

  if (pendingDeleteDifficultyTableId != tableId) {
    pendingDeleteDifficultyTableId = tableId;
    pendingDeleteChartEntryPath.clear();
    difficultyTableStatusMessage = i18n::tr("settings.difficulty_tables.tap_confirm_on_table_delete.message");
    difficultyTableStatusColor = {255, 213, 151, 255};
    lastLayoutWidth = -1;
    return;
  }

  pendingDeleteDifficultyTableId = 0;
  difficultyTableStatusMessage = i18n::tr("settings.difficulty_tables.deleting_table.progress");
  difficultyTableStatusColor = {239, 244, 251, 255};
  if (difficultyTableStatusText != nullptr) {
    difficultyTableStatusText->setText(difficultyTableStatusMessage);
    difficultyTableStatusText->setColor(difficultyTableStatusColor);
  }

  libraryTask.start([&repository = context.chartRepository, tableId](
                        const std::stop_token &token,
                        const SettingsLibraryTask::Publisher &updates) {
    auto session = repository.OpenSession();
    if (!session.has_value()) {
      if (!token.stop_requested()) {
        updates.tableStatus(i18n::tr("settings.difficulty_tables.could_not_open_chart_database.message"), false);
      }
      return;
    }

    const bool deleted = session->DeleteDifficultyTable(tableId);

    if (token.stop_requested()) {
      return;
    }

    updates.tableStatus(deleted ? i18n::tr("settings.difficulty_tables.table_deleted.message") : i18n::tr("settings.difficulty_tables.delete_failed.message"),
                        deleted, deleted);
  });
}

void SettingsScene::refreshChartLibrary() {
  if (libraryTask.running()) {
    return;
  }

  pendingDeleteDifficultyTableId = 0;
  pendingDeleteChartEntryPath.clear();
  if (context.requestRebuildChartLibrary) {
    context.requestRebuildChartLibrary();
    chartFolderStatusMessage = i18n::tr("settings.difficulty_tables.chart_list_rebuild_started_in_background.message");
    chartFolderStatusColor = {181, 228, 165, 255};
    if (chartFolderStatusText != nullptr) {
      chartFolderStatusText->setText(chartFolderStatusMessage);
      chartFolderStatusText->setColor(chartFolderStatusColor);
    }
    return;
  }

  chartFolderStatusMessage = i18n::tr("settings.difficulty_tables.rebuilding_chart_list.progress");
  chartFolderStatusColor = {239, 244, 251, 255};
  if (chartFolderStatusText != nullptr) {
    chartFolderStatusText->setText(chartFolderStatusMessage);
    chartFolderStatusText->setColor(chartFolderStatusColor);
  }

  libraryTask.start(
      [&repository = context.chartRepository](
          const std::stop_token &token,
          const SettingsLibraryTask::Publisher &updates) {
        auto session = repository.OpenSession();
        if (!session.has_value()) {
          if (!token.stop_requested()) {
            updates.folderStatus(i18n::tr("settings.difficulty_tables.could_not_open_chart_database.message"), false);
          }
          return;
        }

        auto entries = session->SelectEffectiveEntries();
        if (entries.empty()) {
          const auto defaultPath = ChartRepository::DefaultBmsFolderPath();
          std::error_code errorCode;
          if (!Utils::EnsureDirectoryExists(defaultPath, errorCode)) {
            if (!token.stop_requested()) {
              updates.folderStatus(i18n::tr("settings.difficulty_tables.could_not_create_default_bms_folder.prefix") +
                                           errorCode.message(),
                                       false);
            }
            return;
          }
          session->InsertEntry(defaultPath);
          entries = session->SelectEffectiveEntries();
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

        ChartLibraryScanner scanner;
        const int changedCount =
            token.stop_requested()
                ? -1
                : (session->ClearChartMeta()
                       ? scanner.Scan(*session, roots, &token)
                       : -1);

        if (token.stop_requested()) {
          return;
        }

        const bool succeeded = changedCount >= 0;
        std::string statusText;
        if (!succeeded) {
          statusText = i18n::tr("settings.difficulty_tables.refresh_failed.message");
        } else if (changedCount == 0) {
          statusText = i18n::tr("settings.difficulty_tables.chart_list_refreshed_no_changes_found.message");
        } else if (changedCount == 1) {
          statusText = i18n::tr("settings.difficulty_tables.chart_list_refreshed_updated_1_chart_entry.message");
        } else {
          statusText = i18n::format(
              "settings.difficulty_tables.library_refresh.updated_count",
              {{"count", std::to_string(changedCount)}});
        }
        updates.folderStatus(statusText, succeeded, true);
      });
}

void SettingsScene::setFindBmsDownloadEntry(
    const std::string &entryPathText) {
  auto session = context.chartRepository.OpenSession();
  if (!session.has_value()) {
    chartFolderStatusMessage = i18n::tr("settings.difficulty_tables.could_not_open_chart_database.message");
    chartFolderStatusColor = {255, 177, 170, 255};
  } else if (!session->SetPrimaryStorageEntry(
                 std::filesystem::path(utf8_to_path_t(entryPathText)))) {
    chartFolderStatusMessage =
        i18n::tr("settings.difficulty_tables.could_not_use_folder_find_bms_downloads.message");
    chartFolderStatusColor = {255, 177, 170, 255};
  } else {
    chartFolderStatusMessage = i18n::tr("settings.difficulty_tables.find_bms_download_folder_updated.message");
    chartFolderStatusColor = {181, 228, 165, 255};
    loadChartEntries();
    refreshChartEntryBackupStatuses();
  }

  if (chartFolderStatusText != nullptr) {
    chartFolderStatusText->setText(chartFolderStatusMessage);
    chartFolderStatusText->setColor(chartFolderStatusColor);
  }
  lastLayoutWidth = -1;
}

void SettingsScene::deleteChartEntry(const std::string &entryPathText) {
  if (libraryTask.running() || entryPathText.empty()) {
    return;
  }

#if TARGET_OS_ANDROID
  if (ChartRepository::IsDefaultBmsFolderPath(
          std::filesystem::path(utf8_to_path_t(entryPathText)))) {
    chartFolderStatusMessage = i18n::tr("settings.difficulty_tables.default_bms_folder_built_in.message");
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

  pendingDeleteChartEntryPath.clear();
  chartFolderStatusMessage = i18n::tr("settings.difficulty_tables.removing_folder.progress");
  chartFolderStatusColor = {239, 244, 251, 255};
  if (chartFolderStatusText != nullptr) {
    chartFolderStatusText->setText(chartFolderStatusMessage);
    chartFolderStatusText->setColor(chartFolderStatusColor);
  }

  libraryTask.start(
      [&repository = context.chartRepository, entryPathText](
          const std::stop_token &token,
          const SettingsLibraryTask::Publisher &updates) {
        auto session = repository.OpenSession();
        if (!session.has_value()) {
          if (!token.stop_requested()) {
            updates.folderStatus(i18n::tr("settings.difficulty_tables.could_not_open_chart_database.message"), false);
          }
          return;
        }

        const auto entries = session->SelectEffectiveEntries();
        const auto entryIt =
            std::find_if(entries.begin(), entries.end(),
                         [&entryPathText](const ChartEntry &entry) {
                           return formatChartEntryPath(entry) == entryPathText;
                         });

        if (entryIt == entries.end() || !entryIt->removable) {
          if (!token.stop_requested()) {
            updates.folderStatus(
                entryIt == entries.end()
                    ? i18n::tr("settings.difficulty_tables.folder_entry_not_found.message")
                    : i18n::tr("settings.difficulty_tables.default_bms_folder_built_in.message"),
                                     false, true);
          }
          return;
        }

        const std::filesystem::path entryPath(entryIt->path);
        int removedChartCount = -1;
        const bool removed = session->DeleteEntryAndChartMetaInDirectory(
            entryPath, removedChartCount);

        if (token.stop_requested()) {
          return;
        }

        std::string statusText;
        if (removed) {
          statusText = removedChartCount == 1
                           ? i18n::tr("settings.difficulty_tables.folder_removed_removed_1_cached_chart.message")
                           : i18n::format("settings.library.folder.remove_summary",
                                          {{"count", std::to_string(removedChartCount)}});
        } else {
          statusText = i18n::tr("settings.difficulty_tables.remove_failed.message");
        }
        updates.folderStatus(statusText,
                             removed, true);
      });
}
