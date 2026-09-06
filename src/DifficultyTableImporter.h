#pragma once

#include "DifficultyTableModel.h"
#include "repositories/ChartRepository.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

using DifficultyTableTextFetcher = std::function<std::optional<std::string>(
    const std::string &url, std::string *errorMessage)>;

struct DifficultyTableImportProgress {
  int current = 0;
  int total = 0;
  std::string tableName;
};

using DifficultyTableImportProgressCallback =
    std::function<void(const DifficultyTableImportProgress &)>;
using DifficultyTableImportCheckpoint = std::function<bool()>;
using DifficultyTableImportPauseProbe = std::function<bool()>;

using DifficultyTableControlledTextFetcher =
    std::function<std::optional<std::string>(
        const std::string &url, std::string *errorMessage,
        const DifficultyTableImportCheckpoint &checkpoint,
        const DifficultyTableImportPauseProbe &pauseRequested)>;

class DifficultyTableImporter {
public:
  DifficultyTableImporter();
  explicit DifficultyTableImporter(DifficultyTableTextFetcher fetchText);

  bool ImportFromUrl(
      ChartRepository::Session &session, const std::string &pageUrl,
      std::string *errorMessage = nullptr,
      DifficultyTableImportProgressCallback progressCallback = nullptr,
      DifficultyTableImportCheckpoint checkpoint = nullptr,
      DifficultyTableImportPauseProbe pauseRequested = nullptr);
  bool UpdateFromSourceUrl(ChartRepository::Session &session, int tableId,
                           std::string *errorMessage = nullptr,
                           DifficultyTableImportCheckpoint checkpoint =
                               nullptr,
                           DifficultyTableImportPauseProbe pauseRequested =
                               nullptr);
  int ImportFromDirectory(ChartRepository::Session &session,
                          const std::filesystem::path &directory,
                          DifficultyTableImportCheckpoint checkpoint = nullptr);

private:
  DifficultyTableControlledTextFetcher fetchText_;
};
