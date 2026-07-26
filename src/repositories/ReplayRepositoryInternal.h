#pragma once

#include "ReplayRepository.h"
#include "../sqlite3.h"

#include <mutex>
#include <span>

struct ReplayRepository::Impl {
  explicit Impl(std::filesystem::path path = {});

  mutable std::mutex sessionMutex;
  std::filesystem::path databasePath;
  std::filesystem::path chartDatabasePath;
  sqlite3 *sessionDatabase = nullptr;
};

namespace replay_repository_detail {

enum class IrDraftStageStatus {
  Succeeded,
  StorageFailure,
  IntegrityConflict,
};

struct IrDraftStageOutcome {
  IrDraftStageStatus status = IrDraftStageStatus::StorageFailure;
  std::string diagnostic;
};

sqlite3 *OpenDatabase(const std::filesystem::path &path,
                      std::string &errorMessage);
std::filesystem::path
ResolvedDatabasePath(const std::filesystem::path &databasePath);
bool EquivalentDatabasePaths(const std::filesystem::path &first,
                             const std::filesystem::path &second);
bool MigrateSchema(sqlite3 *database,
                   const std::filesystem::path &chartDatabasePath = {});

ReservationOutcome ReserveReplayFileOnConnection(sqlite3 *database,
                                                 std::string_view attemptId,
                                                 std::string_view stem);
result_persistence::StageOutcome StageCompletedChartAttemptOnConnection(
    sqlite3 *database, const result_persistence::PersistedChartResult &result,
    const ir::IrSubmissionSnapshot &irSnapshot,
    const ReplayFileReference &replayFile,
    std::span<const ir::IrOutboxDraft> irDrafts);
result_persistence::StageOutcome StageCompletedCourseAttemptOnConnection(
    sqlite3 *database,
    const result_persistence::PersistedCourseResult &result,
    const ReplayFileReference &replayFile);
ResultReadOutcome LoadChartResultOnConnection(sqlite3 *database, int resultId);
CourseResultReadOutcome LoadCourseResultOnConnection(sqlite3 *database,
                                                     int resultId);
std::vector<ReplaySummary> ListCompactChartResultsOnConnection(
    sqlite3 *database, const bms_parser::ChartMeta &chartMeta, int limit,
    std::string_view irProviderId, std::string_view irServerOrigin);
std::vector<ReplaySummary> ListCompactCourseResultsOnConnection(
    sqlite3 *database, const CourseReplayLookup &lookup, int limit);
ir::IrSubmissionSnapshotReadOutcome
LoadIrSubmissionSnapshotOnConnection(sqlite3 *database,
                                     std::string_view attemptId);

bool CreateReplayTablesOnConnection(
    sqlite3 *database,
    const std::filesystem::path &chartDatabasePath = {});
bool CreateCompactReplaySchema11OnConnection(sqlite3 *database);
std::vector<ReplaySummary> ListReplaysOnConnection(
    sqlite3 *database, const bms_parser::ChartMeta &chartMeta, int limit,
    std::string_view irProviderId = {}, std::string_view irServerOrigin = {});
IrUploadReplayReadOutcome
ListIrUploadCandidateReplaysOnConnection(sqlite3 *database,
                                         std::string_view providerId,
                                         std::string_view serverOrigin);
std::vector<ReplaySummary>
ListCourseReplaysOnConnection(sqlite3 *database,
                              const CourseReplayLookup &lookup, int limit);
IrDraftStageOutcome
InsertInactiveIrDraftsOnConnection(sqlite3 *database,
                                   std::span<const ir::IrOutboxDraft> drafts);
IrDraftStageOutcome
VerifyIrDraftsOnConnection(sqlite3 *database, std::string_view attemptId,
                           std::span<const ir::IrOutboxDraft> drafts);

} // namespace replay_repository_detail
