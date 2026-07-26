#pragma once

#include "repositories/ChartRepository.h"
#include "repositories/ReplayRepository.h"
#include "repositories/ScoreRepository.h"
#include "repositories/MusicPlaylistRepository.h"

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

inline bool initializeChartDatabase(ChartRepository &repository) {
  return repository.EnsureReady();
}

inline bool initializeScoreDatabase(ScoreRepository &repository) {
  return repository.EnsureSchema();
}

inline bool initializeScoreDatabase(const std::filesystem::path &databasePath) {
  ScoreRepository helper(databasePath);
  return helper.EnsureSchema();
}

inline bool initializeReplayDatabase(ReplayRepository &repository) {
  return repository.EnsureSchema();
}

inline bool initializeReplayDatabase(
    const std::filesystem::path &databasePath) {
  ReplayRepository helper(databasePath);
  return helper.EnsureSchema();
}

inline bool
initializeReplayDatabase(const std::filesystem::path &databasePath,
                         const std::filesystem::path &chartDatabasePath) {
  ReplayRepository helper(databasePath);
  helper.SetChartDatabasePath(chartDatabasePath);
  return helper.EnsureSchema();
}

inline bool initializeMusicDatabase(MusicPlaylistRepository &repository) {
  return repository.EnsureReady();
}

inline DatabaseInitializationStatus
initializeApplicationDatabases(ChartRepository &charts,
                               ScoreRepository &scores,
                               ReplayRepository &replays,
                               MusicPlaylistRepository &music) {
  return initializeApplicationDatabasesWith(
      [&] { return initializeChartDatabase(charts); },
      [&] {
        scores.SetChartDatabasePath(charts.DatabasePath());
        return initializeScoreDatabase(scores);
      },
      [&] {
        replays.SetChartDatabasePath(charts.DatabasePath());
        return initializeReplayDatabase(replays);
      },
      [&] { return initializeMusicDatabase(music); });
}

} // namespace app_database_initializer
