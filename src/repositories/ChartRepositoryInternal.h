#pragma once

#include "ChartRepository.h"
#include "ScoreRepository.h"
#include "SqliteRAII.h"
#include "../LibraryFolderClearData.h"
#include "ScoreRepositoryModels.h"
#include "../sqlite3.h"

#include <mutex>
#include <span>

struct ChartRepository::Impl {
  explicit Impl(std::filesystem::path path);

  std::filesystem::path databasePath;
  std::mutex readinessMutex;
  bool ready = false;
};

struct ChartSessionStorage {
  explicit ChartSessionStorage(sqlite3 *database);

  sqlite3 *database() const;

  SqliteConnectionHandle connection;
};

struct ChartRepository::Session::Impl {
  Impl(sqlite3 *database, ScoreRepository *scoresValue);

  ScoreRepository &scoreRepository();
  sqlite3 *database() const;

  std::shared_ptr<ChartSessionStorage> storage;
  ScoreRepository *scores;
  ScoreRepository fallbackScores;
};

namespace chart_repository_detail {

bool EnsureCoreSchema(sqlite3 *database);
bool EnsureDifficultySchema(sqlite3 *database);
void InvalidateDifficultyLabelCache();
void BumpLibraryRevision();
void SelectAllChartMeta(sqlite3 *database,
                        std::vector<bms_parser::ChartMeta> &chartMetas);
ChartMetaPathBatchReadOutcome SelectChartMetaByPaths(
    sqlite3 *database, std::span<const std::filesystem::path> paths);

chart_library::FolderClearDataByLongNoteMode
LoadFolderClearDataByLongNoteMode(
    sqlite3 *database, const ScoreClearRankCache &projectedChartRanks,
    const ScoreClearRankCache &localCourseRanks);

} // namespace chart_repository_detail
