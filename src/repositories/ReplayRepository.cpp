#include "ReplayRepository.h"
#include "ReplayRepositoryInternal.h"

#include "../BmsMetadataText.h"
#include "ChartSqlExpressions.h"
#include "../LongNoteModeUtils.h"
#include "../ProfileDatabaseActivity.h"
#include "../replay/ReplayFileStore.h"
#include "SqliteRAII.h"
#include "../Uuid.h"
#include "../Utils.h"
#include "../path.h"

#include <SDL2/SDL.h>
#include <algorithm>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

constexpr auto kStaleReplayTemporaryAge = std::chrono::hours(1);

void removeStaleReplayTemporaryFiles(
    const std::filesystem::path &resolvedDatabasePath) {
  if (resolvedDatabasePath.empty() ||
      resolvedDatabasePath.parent_path().empty()) {
    return;
  }
  replay::ReplayFileStore store(resolvedDatabasePath.parent_path());
  store.removeStaleTemporaryFiles(std::chrono::system_clock::now() -
                                  kStaleReplayTemporaryAge);
}

bool recoverReplayFileReservations(sqlite3 *database,
                                   const std::filesystem::path &profileRoot) {
  if (database == nullptr || profileRoot.empty()) {
    return false;
  }

  std::string transactionError;
  SqliteTransactionHandle transaction(database, "BEGIN IMMEDIATE TRANSACTION",
                                      transactionError);
  if (!transaction.active()) {
    return false;
  }

  SqliteStatementHandle candidates;
  if (prepareSqliteStatement(
          database,
          "SELECT r.attempt_id,r.stem,r.history_index,r.relative_path,"
          "r.finalized_content_sha256,r.finalized_compressed_size FROM "
          "replay_file_reservations r WHERE NOT EXISTS(SELECT 1 FROM "
          "chart_results c WHERE c.attempt_id=r.attempt_id) AND NOT EXISTS("
          "SELECT 1 FROM course_results c WHERE c.attempt_id=r.attempt_id)",
          candidates) != SQLITE_OK) {
    return false;
  }

  struct AbandonedReservation {
    std::string attemptId;
    std::filesystem::path relativePath;
    std::optional<replay::ReplayFileMetadata> ownedFinalFile;
  };
  std::vector<AbandonedReservation> abandoned;
  replay::ReplayFileStore store(profileRoot);
  int step = SQLITE_OK;
  while ((step = sqlite3_step(candidates.get())) == SQLITE_ROW) {
    if (sqlite3_column_type(candidates.get(), 0) != SQLITE_TEXT ||
        sqlite3_column_type(candidates.get(), 1) != SQLITE_TEXT ||
        sqlite3_column_type(candidates.get(), 2) != SQLITE_INTEGER ||
        sqlite3_column_type(candidates.get(), 3) != SQLITE_TEXT) {
      continue;
    }
    const std::string attemptId = sqliteColumnString(candidates.get(), 0);
    const std::string stem = sqliteColumnString(candidates.get(), 1);
    const auto historyIndex = sqlite3_column_int64(candidates.get(), 2);
    const std::filesystem::path storedRelativePath(
        sqliteColumnString(candidates.get(), 3));
    const bool hashNull =
        sqlite3_column_type(candidates.get(), 4) == SQLITE_NULL;
    const bool sizeNull =
        sqlite3_column_type(candidates.get(), 5) == SQLITE_NULL;
    if (hashNull != sizeNull) {
      return false;
    }
    std::string pathDiagnostic;
    const auto identity =
        replay::pathForStem(stem, historyIndex, pathDiagnostic);
    if (!identity || identity->relativePath != storedRelativePath) {
      continue;
    }

    std::error_code pathError;
    const auto status = std::filesystem::symlink_status(
        profileRoot / identity->relativePath, pathError);
    const bool definitelyMissing =
        (!pathError &&
         status.type() == std::filesystem::file_type::not_found) ||
        pathError == std::errc::no_such_file_or_directory;
    if (pathError && !definitelyMissing) {
      return false;
    }
    std::optional<replay::ReplayFileMetadata> ownedFinalFile;
    if (!definitelyMissing && !hashNull) {
      if (sqlite3_column_type(candidates.get(), 4) != SQLITE_TEXT ||
          sqlite3_column_type(candidates.get(), 5) != SQLITE_INTEGER ||
          sqlite3_column_int64(candidates.get(), 5) <= 0) {
        return false;
      }
      replay::ReplayFileMetadata metadata{
          .relativePath = identity->relativePath,
          .sha256 = sqliteColumnString(candidates.get(), 4),
          .compressedSize = static_cast<std::uint64_t>(
              sqlite3_column_int64(candidates.get(), 5)),
          .codecVersion = replay::BeatorajaReplayCodec::kCodecVersion,
      };
      const auto inspection = store.inspect(metadata);
      if (inspection.state == replay::ReplayFileState::IoFailure) {
        return false;
      }
      if (inspection.state == replay::ReplayFileState::Available) {
        ownedFinalFile = std::move(metadata);
      }
    }
    abandoned.push_back({.attemptId = attemptId,
                         .relativePath = identity->relativePath,
                         .ownedFinalFile = std::move(ownedFinalFile)});
  }
  if (step != SQLITE_DONE) {
    return false;
  }
  candidates.reset();

  for (const auto &reservation : abandoned) {
    if (reservation.ownedFinalFile) {
      std::string removeDiagnostic;
      if (!store.removeIfMatches(*reservation.ownedFinalFile,
                                 removeDiagnostic)) {
        SDL_Log("Could not remove orphan replay reservation file: %s",
                removeDiagnostic.c_str());
        return false;
      }
    }
    SqliteStatementHandle remove;
    if (prepareSqliteStatement(
            database,
            "DELETE FROM replay_file_reservations WHERE attempt_id=?",
            remove) != SQLITE_OK ||
        !bindSqliteText(remove.get(), 1, reservation.attemptId) ||
        sqlite3_step(remove.get()) != SQLITE_DONE ||
        sqlite3_changes(database) != 1) {
      return false;
    }
  }

  if (executeSqlite(database, "DELETE FROM replay_stem_sequences") ||
      executeSqlite(
          database,
          "INSERT INTO replay_stem_sequences(stem,last_history_index) "
          "SELECT stem,MAX(history_index) FROM (SELECT stem,history_index "
          "FROM replay_files UNION ALL SELECT stem,history_index FROM "
          "replay_file_reservations) GROUP BY stem")) {
    return false;
  }
  return transaction.commit(transactionError);
}

} // namespace

ReplayRepository::Impl::Impl(std::filesystem::path path)
    : databasePath(std::move(path)),
      chartDatabasePath(Utils::GetDocumentsPath("db") / "chart.db") {}

ReplayRepository::ReplayRepository() : impl_(std::make_unique<Impl>()) {}

ReplayRepository::ReplayRepository(std::filesystem::path databasePath)
    : impl_(std::make_unique<Impl>(std::move(databasePath))) {}

ReplayRepository::~ReplayRepository() {
  std::lock_guard lock(impl_->sessionMutex);
  ShutdownLocked();
}

void ReplayRepository::SetDatabasePath(std::filesystem::path databasePath) {
  profile_database_activity::WriteGuard operation;
  std::lock_guard lock(impl_->sessionMutex);
  if (!replay_repository_detail::EquivalentDatabasePaths(impl_->databasePath,
                                                         databasePath)) {
    ShutdownLocked();
  }
  impl_->databasePath = std::move(databasePath);
  removeStaleReplayTemporaryFiles(GetResolvedDatabasePathLocked());
}

void ReplayRepository::SetChartDatabasePath(
    std::filesystem::path chartDatabasePath) {
  std::lock_guard lock(impl_->sessionMutex);
  impl_->chartDatabasePath = std::move(chartDatabasePath);
}

std::filesystem::path ReplayRepository::GetDatabasePath() const {
  std::lock_guard lock(impl_->sessionMutex);
  return impl_->databasePath;
}

std::filesystem::path ReplayRepository::GetResolvedDatabasePath() const {
  std::lock_guard lock(impl_->sessionMutex);
  return GetResolvedDatabasePathLocked();
}

std::filesystem::path ReplayRepository::GetResolvedDatabasePathLocked() const {
  return replay_repository_detail::ResolvedDatabasePath(impl_->databasePath);
}

bool ReplayRepository::BindDatabasePath(std::filesystem::path databasePath,
                                        std::string &errorMessage) {
  profile_database_activity::WriteGuard operation;
  if (databasePath.empty()) {
    errorMessage = "replay database path is empty";
    return false;
  }

  std::lock_guard lock(impl_->sessionMutex);
  if (impl_->sessionDatabase != nullptr &&
      sqlite3_get_autocommit(impl_->sessionDatabase) == 0) {
    SDL_Log("Discarding replay database with an unfinished transaction");
    ShutdownLocked();
  }
  if (impl_->sessionDatabase != nullptr &&
      replay_repository_detail::EquivalentDatabasePaths(impl_->databasePath,
                                                        databasePath)) {
    impl_->databasePath = std::move(databasePath);
    if (replay_repository_detail::MigrateSchema(
            impl_->sessionDatabase, impl_->chartDatabasePath)) {
      removeStaleReplayTemporaryFiles(GetResolvedDatabasePathLocked());
      errorMessage.clear();
      return true;
    }
    errorMessage = "replay database validation failed";
    return false;
  }

  const std::filesystem::path resolvedPath =
      replay_repository_detail::ResolvedDatabasePath(databasePath);
  std::string openError;
  SqliteConnectionHandle candidate(
      replay_repository_detail::OpenDatabase(resolvedPath, openError));
  if (!candidate) {
    SDL_Log("Refusing to bind replay database %s: %s",
            fspath_to_utf8(resolvedPath).c_str(), openError.c_str());
    errorMessage = "replay database validation failed";
    return false;
  }
  if (!replay_repository_detail::CreateReplayTablesOnConnection(
          candidate.get(), impl_->chartDatabasePath)) {
    errorMessage = "replay database validation failed";
    return false;
  }
  if (!recoverReplayFileReservations(candidate.get(),
                                     resolvedPath.parent_path())) {
    errorMessage = "replay reservation recovery failed";
    return false;
  }

  sqlite3 *oldDatabase = impl_->sessionDatabase;
  impl_->sessionDatabase = candidate.release();
  impl_->databasePath = std::move(databasePath);
  closeSqliteDatabase(oldDatabase);
  removeStaleReplayTemporaryFiles(GetResolvedDatabasePathLocked());
  errorMessage.clear();
  return true;
}

bool ReplayRepository::HasActiveReads() {
  return profile_database_activity::readsActive();
}

bool ReplayRepository::HasActiveWrites() {
  return profile_database_activity::writesActive();
}

void ReplayRepository::Shutdown() {
  profile_database_activity::WriteGuard operation;
  std::lock_guard lock(impl_->sessionMutex);
  ShutdownLocked();
}

void ReplayRepository::ShutdownLocked() {
  sqlite3 *database = impl_->sessionDatabase;
  impl_->sessionDatabase = nullptr;
  closeSqliteDatabase(database);
}

bool ReplayRepository::EnsureSessionDatabaseLocked() {
  if (impl_->sessionDatabase != nullptr) {
    if (sqlite3_get_autocommit(impl_->sessionDatabase) != 0) {
      return replay_repository_detail::MigrateSchema(
          impl_->sessionDatabase, impl_->chartDatabasePath);
    }
    SDL_Log("Discarding replay database with an unfinished transaction");
    ShutdownLocked();
  }

  const std::filesystem::path path = GetResolvedDatabasePathLocked();
  std::string openError;
  SqliteConnectionHandle candidate(
      replay_repository_detail::OpenDatabase(path, openError));
  if (!candidate) {
    SDL_Log("Refusing to open replay database %s: %s",
            fspath_to_utf8(path).c_str(), openError.c_str());
    return false;
  }
  if (!replay_repository_detail::CreateReplayTablesOnConnection(
          candidate.get(), impl_->chartDatabasePath)) {
    return false;
  }
  if (!recoverReplayFileReservations(candidate.get(), path.parent_path())) {
    return false;
  }
  impl_->sessionDatabase = candidate.release();
  return true;
}

bool ReplayRepository::EnsureSchema() {
  profile_database_activity::WriteGuard operation;
  std::lock_guard lock(impl_->sessionMutex);
  return EnsureSessionDatabaseLocked();
}
