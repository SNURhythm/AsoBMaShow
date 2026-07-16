#pragma once

#include "ReplayRepository.h"
#include "../sqlite3.h"

#include <mutex>

struct ReplayRepository::Impl {
  explicit Impl(std::filesystem::path path = {});

  mutable std::mutex sessionMutex;
  std::filesystem::path databasePath;
  sqlite3 *sessionDatabase = nullptr;
};

namespace replay_repository_detail {

sqlite3 *OpenDatabase(const std::filesystem::path &path,
                      std::string &errorMessage);
std::filesystem::path
ResolvedDatabasePath(const std::filesystem::path &databasePath);
bool EquivalentDatabasePaths(const std::filesystem::path &first,
                             const std::filesystem::path &second);
bool MigrateSchema(sqlite3 *database);

bool CreateReplayTablesOnConnection(sqlite3 *database);
std::optional<int> SaveReplayOnConnection(
    sqlite3 *database, const ReplayData &replay,
    const std::string &provenanceJson);
std::optional<int> SaveCourseReplayOnConnection(
    sqlite3 *database, const CourseReplayData &replay,
    const std::string &courseProvenanceJson,
    const std::vector<std::string> &stageProvenanceJson);
std::vector<ReplaySummary>
ListReplaysOnConnection(sqlite3 *database,
                        const bms_parser::ChartMeta &chartMeta, int limit);
std::vector<ReplaySummary>
ListCourseReplaysOnConnection(sqlite3 *database,
                              const CourseReplayLookup &lookup, int limit);
std::optional<ReplayData>
LoadReplayOnConnection(sqlite3 *database, int replayId,
                       const bms_parser::ChartMeta &chartMeta);
std::optional<CourseReplayData>
LoadCourseReplayOnConnection(sqlite3 *database, int replayId);
bool RecoverCourseRecordsOnConnection(
    sqlite3 *database,
    std::span<const course_identity::Definition> definitions,
    std::span<const CourseScoreEvidence> scoreEvidence,
    std::string &errorMessage);
std::optional<ReplayData>
LoadLatestReplayOnConnection(sqlite3 *database,
                             const bms_parser::ChartMeta &chartMeta);

} // namespace replay_repository_detail
