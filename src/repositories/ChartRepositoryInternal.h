#pragma once

#include "ChartRepository.h"
#include "ScoreRepository.h"
#include "SqliteRAII.h"
#include "../LibraryFolderClearData.h"
#include "ScoreRepositoryModels.h"
#include "../sqlite3.h"

#include <mutex>

struct ChartRepository::Impl {
  explicit Impl(std::filesystem::path path);

  std::filesystem::path databasePath;
  std::mutex readinessMutex;
  bool ready = false;
};

struct ChartRepository::Session::Impl {
  Impl(ChartRepository &owner, sqlite3 *database, ScoreRepository *scoresValue);

  ScoreRepository &scoreRepository();

  ChartRepository *repository;
  SqliteConnectionHandle connection;
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

chart_library::FolderClearDataByLongNoteMode
LoadFolderClearDataByLongNoteMode(sqlite3 *database,
                                  const ScoreClearRankCache &scoreRanks);

} // namespace chart_repository_detail
