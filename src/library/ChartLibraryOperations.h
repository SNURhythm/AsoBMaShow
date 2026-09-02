#pragma once

#include "ChartLibraryTaskService.h"
#include "../DifficultyTableImporter.h"
#include "../repositories/ChartRepository.h"

#include <filesystem>
#include <functional>
#include <optional>

namespace chart_library_tasks {

struct ChartLibraryOperationsDependencies {
  ChartRepository &repository;
  std::filesystem::path tablesDirectory;
  std::function<bool()> defaultDifficultyTablesSeeded;
  std::function<void(bool)> setDefaultDifficultyTablesSeeded;
  std::function<bool()> saveSettings;
  std::function<void(bool includeFolders)> requestReload;
  std::function<bool(ChartRepository::Session &, const std::string &,
                     std::string *, DifficultyTableImportProgressCallback,
                     const DifficultyTableImportCheckpoint &)>
      importDifficultyTableFromUrl;
  std::function<bool(ChartRepository::Session &, int, std::string *,
                     const DifficultyTableImportCheckpoint &)>
      updateDifficultyTableFromSourceUrl;
  std::function<int(ChartRepository::Session &,
                    const std::filesystem::path &)>
      importDifficultyTablesFromDirectory;
  std::function<std::optional<std::filesystem::path>()> selectInitialFolder;
  std::function<void(const std::vector<ChartEntry> &)> refreshFolderAccess;
  std::function<std::uint64_t()> pendingScanFlushRequest;
  std::function<void(std::uint64_t)> completeScanFlush;
};

class ChartLibraryOperations final {
public:
  explicit ChartLibraryOperations(ChartLibraryOperationsDependencies);

  TaskRunResult run(const TaskRequest &, const std::stop_token &,
                    TaskProgressCallback, TaskPauseCallback);

private:
  TaskRunResult runRefresh(const TaskRequest &, const std::stop_token &,
                           const TaskProgressCallback &,
                           const TaskPauseCallback &);
  TaskRunResult runPathRefresh(const TaskRequest &, const std::stop_token &,
                               const TaskProgressCallback &,
                               const TaskPauseCallback &);
  TaskRunResult runDifficultyTableUpdate(const TaskRequest &,
                                         const std::stop_token &,
                                         const TaskProgressCallback &,
                                         const TaskPauseCallback &);
  TaskRunResult runDownloadedIndex(const TaskRequest &,
                                   const std::stop_token &,
                                   const TaskProgressCallback &,
                                   const TaskPauseCallback &);
  TaskRunResult runAndroidImport(const TaskRequest &, const std::stop_token &,
                                 const TaskProgressCallback &,
                                 const TaskPauseCallback &);
  void seedDefaultDifficultyTablesIfNeeded(
      ChartRepository::Session &, const std::stop_token &,
      const TaskProgressCallback &, const TaskPauseCallback &);
  static const char *progressStageText(ChartScanProgressStage) noexcept;

  ChartLibraryOperationsDependencies dependencies_;
};

} // namespace chart_library_tasks
