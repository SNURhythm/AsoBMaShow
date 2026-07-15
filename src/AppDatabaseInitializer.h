#pragma once

#include "ChartDBHelper.h"
#include "ReplayDBHelper.h"
#include "ScoreDBHelper.h"
#include "repositories/SqliteRAII.h"
#include "audio/MusicPlaylistDB.h"

#include <filesystem>
#include <utility>

namespace app_database_initializer {

struct DatabaseInitializationStatus {
  bool chart = false;
  bool score = false;
  bool replay = false;
  bool music = false;

  [[nodiscard]] bool ok() const { return chart && score && replay && music; }
};

template <typename ChartInit, typename ScoreInit, typename ReplayInit,
          typename MusicInit>
DatabaseInitializationStatus
initializeApplicationDatabasesWith(ChartInit &&chartInit, ScoreInit &&scoreInit,
                                   ReplayInit &&replayInit,
                                   MusicInit &&musicInit) {
  DatabaseInitializationStatus status;
  status.chart = static_cast<bool>(std::forward<ChartInit>(chartInit)());
  status.score = static_cast<bool>(std::forward<ScoreInit>(scoreInit)());
  status.replay = static_cast<bool>(std::forward<ReplayInit>(replayInit)());
  status.music = static_cast<bool>(std::forward<MusicInit>(musicInit)());
  return status;
}

inline bool initializeChartDatabase() {
  ChartDBHelper &helper = ChartDBHelper::GetInstance();
  SqliteConnectionHandle db(helper.Connect());
  if (!db) {
    return false;
  }

  bool ok = true;
  ok = helper.CreateChartMetaTable(db.get()) && ok;
  ok = helper.CreateSolidArchiveTable(db.get()) && ok;
  ok = helper.CreateFavoritesTable(db.get()) && ok;
  ok = helper.CreateEntriesTable(db.get()) && ok;
  ok = helper.CreateDifficultyTableTables(db.get()) && ok;
  ok = helper.CreateChartStateTables(db.get()) && ok;
  return ok;
}

inline bool initializeScoreDatabase() {
  ScoreDBHelper &helper = ScoreDBHelper::GetInstance();
  return helper.EnsureSchema();
}

inline bool initializeScoreDatabase(const std::filesystem::path &databasePath) {
  ScoreDBHelper helper(databasePath);
  return helper.EnsureSchema();
}

inline bool initializeReplayDatabase() {
  ReplayDBHelper &helper = ReplayDBHelper::GetInstance();
  return helper.EnsureSchema();
}

inline bool initializeReplayDatabase(
    const std::filesystem::path &databasePath) {
  ReplayDBHelper helper(databasePath);
  return helper.EnsureSchema();
}

inline bool initializeMusicDatabase() {
  MusicPlaylistDB helper;
  SqliteConnectionHandle db(helper.Connect());
  if (!db) {
    return false;
  }
  return helper.CreateTables(db.get());
}

inline DatabaseInitializationStatus initializeApplicationDatabases() {
  return initializeApplicationDatabasesWith(
      initializeChartDatabase, [] { return initializeScoreDatabase(); },
      [] { return initializeReplayDatabase(); }, initializeMusicDatabase);
}

} // namespace app_database_initializer
