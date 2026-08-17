#pragma once

#include "ReplayRepository.h"
#include "../sqlite3.h"

#include <mutex>
#include <span>

struct ReplayRepository::Impl {
  explicit Impl(std::filesystem::path path = {});

  mutable std::mutex sessionMutex;
  std::filesystem::path databasePath;
  sqlite3 *sessionDatabase = nullptr;
  int sessionSchemaVersion = -1;
  int sessionUserVersion = -1;
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
bool MigrateSchema(sqlite3 *database);

bool CreateReplayTablesOnConnection(sqlite3 *database);
bool PrepareReplayDatabaseOnConnection(sqlite3 *database,
                                       const std::filesystem::path &path);
std::vector<LegacyChartResultSummary>
ListLegacyChartSummariesOnConnection(sqlite3 *database,
                                     const bms_parser::ChartMeta &chartMeta,
                                     std::size_t limit);
std::vector<LegacyCourseResultSummary> ListLegacyCourseSummariesOnConnection(
    sqlite3 *database, const CourseReplayLookup &lookup, std::size_t limit);
IrDraftStageOutcome ValidateIrDraftsForAttempt(
    const result_persistence::ChartResultAttempt &attempt,
    std::span<const ir::IrOutboxDraft> drafts);
IrDraftStageOutcome
InsertInactiveIrDraftsOnConnection(sqlite3 *database,
                                   std::span<const ir::IrOutboxDraft> drafts);
IrDraftStageOutcome
VerifyIrDraftsOnConnection(sqlite3 *database, std::string_view attemptId,
                           std::span<const ir::IrOutboxDraft> drafts);

} // namespace replay_repository_detail
