#include "ScoreRepository.h"
#include "ScoreRepositoryInternal.h"

#include "../BmsMetadataText.h"
#include "ChartRepository.h"
#include "ChartSqlExpressions.h"
#include "../CoursePlaySession.h"
#include "../LongNoteModeUtils.h"
#include "../ProfileDatabaseActivity.h"
#include "ReplayRepository.h"
#include "../ResultPersistenceModel.h"
#include "ScoreCacheQueries.h"
#include "SqliteRAII.h"
#include "../Utils.h"
#include "../Uuid.h"
#include "../path.h"

#include <SDL2/SDL.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

ScoreRepository::Impl::Impl(std::filesystem::path path)
    : databasePath(std::move(path)),
      chartDatabasePath(Utils::GetDocumentsPath("db") / "chart.db") {}

ScoreRepository::ScoreRepository() : impl_(std::make_unique<Impl>()) {}

ScoreRepository::ScoreRepository(std::filesystem::path databasePath)
    : impl_(std::make_unique<Impl>(std::move(databasePath))) {}

ScoreRepository::~ScoreRepository() {
  std::lock_guard lock(impl_->sessionMutex);
  CloseSessionDatabaseLocked();
}

void ScoreRepository::SetDatabasePath(std::filesystem::path databasePath) {
  profile_database_activity::WriteGuard operation;
  std::lock_guard lock(impl_->sessionMutex);
  if (!score_repository_detail::EquivalentDatabasePaths(impl_->databasePath,
                                                        databasePath)) {
    CloseSessionDatabaseLocked();
  }
  impl_->databasePath = std::move(databasePath);
  score_repository_detail::IncrementRevision();
}

void ScoreRepository::SetChartDatabasePath(
    std::filesystem::path chartDatabasePath) {
  profile_database_activity::WriteGuard operation;
  std::lock_guard lock(impl_->sessionMutex);
  if (impl_->chartDatabasePath != chartDatabasePath) {
    CloseSessionDatabaseLocked();
    impl_->chartDatabasePath = std::move(chartDatabasePath);
  }
}

std::filesystem::path ScoreRepository::GetDatabasePath() const {
  std::lock_guard lock(impl_->sessionMutex);
  return impl_->databasePath;
}

std::filesystem::path ScoreRepository::GetResolvedDatabasePath() const {
  std::lock_guard lock(impl_->sessionMutex);
  return GetResolvedDatabasePathLocked();
}

std::filesystem::path ScoreRepository::GetResolvedDatabasePathLocked() const {
  return score_repository_detail::ResolvedDatabasePath(impl_->databasePath);
}

ScoreRepository::PreparedScoreQueryDatabase::State::State(
    const ScoreRepository &helper, sqlite3 *chartDatabase)
    : sessionLock(helper.impl_->sessionMutex) {
  const std::filesystem::path path = helper.GetResolvedDatabasePathLocked();
  error = score_cache_queries::prepareScoreQueryDatabase(chartDatabase, path);
}

ScoreRepository::PreparedScoreQueryDatabase::~PreparedScoreQueryDatabase() =
    default;

const std::optional<std::string> &
ScoreRepository::PreparedScoreQueryDatabase::error() const {
  return state_->error;
}

bool ScoreRepository::BindDatabasePath(std::filesystem::path databasePath,
                                       std::string &errorMessage) {
  profile_database_activity::WriteGuard operation;
  if (databasePath.empty()) {
    errorMessage = "score database path is empty";
    return false;
  }

  std::lock_guard lock(impl_->sessionMutex);
  if (impl_->sessionDatabase != nullptr &&
      sqlite3_get_autocommit(impl_->sessionDatabase) == 0) {
    SDL_Log("Discarding score database with an unfinished transaction");
    CloseSessionDatabaseLocked();
  }
  if (impl_->sessionDatabase != nullptr &&
      score_repository_detail::EquivalentDatabasePaths(impl_->databasePath,
                                                       databasePath)) {
    if (!score_repository_detail::CurrentSchemaIsValid(
            impl_->sessionDatabase)) {
      errorMessage = "score database validation failed";
      return false;
    }
    impl_->databasePath = std::move(databasePath);
    score_repository_detail::IncrementRevision();
    errorMessage.clear();
    return true;
  }

  std::string openError;
  SqliteConnectionHandle candidate(
      score_repository_detail::OpenDatabase(databasePath, openError));
  if (candidate.get() == nullptr) {
    score_repository_detail::LogDatabaseOpenFailure(databasePath, openError);
    errorMessage = "score database validation failed";
    return false;
  }
  if (!score_repository_detail::EnsureSchemaOnConnection(
          candidate.get(), impl_->chartDatabasePath)) {
    errorMessage = "score database validation failed";
    return false;
  }

  sqlite3 *previous = impl_->sessionDatabase;
  impl_->databasePath = std::move(databasePath);
  impl_->sessionDatabase = candidate.release();
  closeSqliteDatabase(previous);
  score_repository_detail::IncrementRevision();
  errorMessage.clear();
  return true;
}

bool ScoreRepository::HasActiveReads() {
  return profile_database_activity::readsActive();
}

bool ScoreRepository::HasActiveWrites() {
  return profile_database_activity::writesActive();
}

void ScoreRepository::CloseSessionDatabaseLocked() {
  closeSqliteDatabase(impl_->sessionDatabase);
  impl_->sessionDatabase = nullptr;
}

void ScoreRepository::Shutdown() {
  profile_database_activity::WriteGuard operation;
  std::lock_guard lock(impl_->sessionMutex);
  CloseSessionDatabaseLocked();
}

bool ScoreRepository::EnsureSessionDatabaseLocked() {
  if (impl_->sessionDatabase != nullptr) {
    if (sqlite3_get_autocommit(impl_->sessionDatabase) != 0) {
      if (score_repository_detail::CurrentSchemaIsValid(
              impl_->sessionDatabase)) {
        return true;
      }
      // A deferred migration can intentionally leave the database below the
      // current version while chart metadata is rebuilding. Reopen it so the
      // next EnsureSchema call can retry once that authority is available.
      CloseSessionDatabaseLocked();
    } else {
      SDL_Log("Discarding score database with an unfinished transaction");
      CloseSessionDatabaseLocked();
    }
  }

  const std::filesystem::path path = GetResolvedDatabasePathLocked();
  std::string openError;
  SqliteConnectionHandle candidate(
      score_repository_detail::OpenDatabase(path, openError));
  if (candidate.get() == nullptr) {
    score_repository_detail::LogDatabaseOpenFailure(path, openError);
    return false;
  }
  if (!score_repository_detail::EnsureSchemaOnConnection(
          candidate.get(), impl_->chartDatabasePath)) {
    return false;
  }

  impl_->sessionDatabase = candidate.release();
  return true;
}

bool ScoreRepository::EnsureSchema() {
  profile_database_activity::WriteGuard operation;
  std::lock_guard lock(impl_->sessionMutex);
  return EnsureSessionDatabaseLocked();
}

bool ScoreRepository::ClearImportedIrScoresSnapshot(
    const std::filesystem::path &snapshotDatabasePath,
    std::string &errorMessage) {
  if (snapshotDatabasePath.empty()) {
    errorMessage = "imported IR score snapshot path is empty";
    return false;
  }
  std::error_code filesystemError;
  const auto status =
      std::filesystem::symlink_status(snapshotDatabasePath, filesystemError);
  if (filesystemError || !std::filesystem::is_regular_file(status) ||
      std::filesystem::is_symlink(status)) {
    errorMessage = "imported IR score snapshot is not a regular database file";
    return false;
  }

  ScoreRepository snapshot(snapshotDatabasePath);
  if (!snapshot.EnsureSchema()) {
    errorMessage = "imported IR score snapshot schema is unavailable";
    return false;
  }

  profile_database_activity::WriteGuard operation;
  std::lock_guard lock(snapshot.impl_->sessionMutex);
  if (!snapshot.EnsureSessionDatabaseLocked()) {
    errorMessage = "imported IR score snapshot storage is unavailable";
    return false;
  }
  sqlite3 *database = snapshot.impl_->sessionDatabase;
  std::string transactionError;
  SqliteTransactionHandle transaction(database, "BEGIN IMMEDIATE TRANSACTION",
                                      transactionError);
  if (!transaction.active()) {
    errorMessage = "could not start imported IR score snapshot cleanup";
    return false;
  }

  SqliteStatementHandle deletion;
  if (prepareSqliteStatement(database,
                             "DELETE FROM scores WHERE score_source=?",
                             deletion) != SQLITE_OK ||
      sqlite3_bind_int(deletion.get(), 1,
                       static_cast<int>(ScoreStorageSource::ImportedIr)) !=
          SQLITE_OK ||
      sqlite3_step(deletion.get()) != SQLITE_DONE) {
    errorMessage = "could not clear imported IR score snapshot rows";
    return false;
  }
  if (const auto error =
          score_cache_queries::rebuildScoreSummaryTables(database)) {
    errorMessage =
        "could not rebuild imported IR score snapshot summaries: " + *error;
    return false;
  }

  SqliteStatementHandle verify;
  if (prepareSqliteStatement(database,
                             "SELECT COUNT(*) FROM scores WHERE score_source=?",
                             verify) != SQLITE_OK ||
      sqlite3_bind_int(verify.get(), 1,
                       static_cast<int>(ScoreStorageSource::ImportedIr)) !=
          SQLITE_OK ||
      sqlite3_step(verify.get()) != SQLITE_ROW ||
      sqlite3_column_type(verify.get(), 0) != SQLITE_INTEGER ||
      sqlite3_column_int64(verify.get(), 0) != 0 ||
      sqlite3_step(verify.get()) != SQLITE_DONE) {
    errorMessage = "imported IR score snapshot cleanup could not be verified";
    return false;
  }
  if (!transaction.commit(transactionError)) {
    errorMessage = "could not commit imported IR score snapshot cleanup";
    return false;
  }

  deletion.reset();
  verify.reset();
  int walFrames = 0;
  int checkpointedFrames = 0;
  const int checkpointResult =
      sqlite3_wal_checkpoint_v2(database, "main", SQLITE_CHECKPOINT_TRUNCATE,
                                &walFrames, &checkpointedFrames);
  if (checkpointResult != SQLITE_OK ||
      (walFrames >= 0 && checkpointedFrames != walFrames)) {
    errorMessage =
        "could not checkpoint the cleaned imported IR score snapshot";
    return false;
  }
  errorMessage.clear();
  return true;
}
