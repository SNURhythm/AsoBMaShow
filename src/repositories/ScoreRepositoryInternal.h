#pragma once

#include "ScoreRepository.h"
#include "../ProfileDatabaseActivity.h"
#include "../sqlite3.h"

#include <mutex>

struct ScoreRepository::PreparedScoreQueryDatabase::State {
  State(const ScoreRepository &repository, sqlite3 *chartDatabase);

  profile_database_activity::WriteGuard operation;
  std::unique_lock<std::mutex> sessionLock;
  std::optional<std::string> error;
};

namespace score_repository_detail {

bool CreateScoreTableOnConnection(
    sqlite3 *database, const std::filesystem::path &chartDatabasePath);
bool CreateCourseScoreTableOnConnection(sqlite3 *database);
bool EnsureSchemaOnConnection(
    sqlite3 *database, const std::filesystem::path &chartDatabasePath);
bool InsertCourseScoreOnConnection(
    sqlite3 *database, const CoursePlaySession &session,
    const RhythmState &state, int completedCharts, int totalCharts,
    const ScoreProvenance &provenance, const std::string &provenanceJson);
std::optional<ScoreBestSnapshot> LoadBestScoreOnConnection(
    sqlite3 *database, const bms_parser::ChartMeta &chartMeta,
    const std::optional<std::string> &beforeCreatedAt,
    const std::optional<std::string> &excludeAttemptId);
std::optional<ScoreBestSnapshot>
LoadBestCourseScoreOnConnection(sqlite3 *database,
                                const CoursePlaySession &session);
CourseScoreRecoveryResult RecoverCourseRecordsOnConnection(
    sqlite3 *database,
    std::span<const course_identity::Definition> definitions);
ScoreClearRankCache LoadBestClearRanksOnConnection(
    sqlite3 *database, std::string_view schema = {});
ScoreBestCache LoadBestScoresOnConnection(sqlite3 *database,
                                          std::string_view schema = {});

} // namespace score_repository_detail
