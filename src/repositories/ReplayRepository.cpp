#include "ReplayRepository.h"
#include "ReplayRepositoryInternal.h"

#include "../BmsMetadataText.h"
#include "ChartSqlExpressions.h"
#include "../LongNoteModeUtils.h"
#include "../ProfileDatabaseActivity.h"
#include "SqliteRAII.h"
#include "../Uuid.h"
#include "../Utils.h"
#include "../path.h"

#include <SDL2/SDL.h>
#include <algorithm>
#include <bit>
#include <cctype>
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

std::optional<int> readPragmaInt(sqlite3 *database, const char *pragma) {
  sqlite3_stmt *statement = nullptr;
  if (sqlite3_prepare_v2(database, pragma, -1, &statement, nullptr) !=
      SQLITE_OK) {
    return std::nullopt;
  }
  const int stepResult = sqlite3_step(statement);
  std::optional<int> result;
  if (stepResult == SQLITE_ROW &&
      sqlite3_column_type(statement, 0) == SQLITE_INTEGER) {
    result = sqlite3_column_int(statement, 0);
  }
  sqlite3_finalize(statement);
  return result;
}

void rememberSessionSchemaMarker(sqlite3 *database, int &schemaVersion,
                                 int &userVersion) {
  schemaVersion =
      readPragmaInt(database, "PRAGMA schema_version").value_or(-1);
  userVersion =
      readPragmaInt(database, "PRAGMA user_version").value_or(-1);
}

bool sessionSchemaMarkerIsCurrent(sqlite3 *database, int schemaVersion,
                                  int userVersion) {
  if (schemaVersion < 0 || userVersion < 0) {
    return false;
  }
  return readPragmaInt(database, "PRAGMA schema_version") == schemaVersion &&
         readPragmaInt(database, "PRAGMA user_version") == userVersion;
}

} // namespace

ReplayRepository::Impl::Impl(std::filesystem::path path)
    : databasePath(std::move(path)) {}

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
}

std::filesystem::path ReplayRepository::GetDatabasePath() const {
  std::lock_guard lock(impl_->sessionMutex);
  return impl_->databasePath;
}

std::filesystem::path ReplayRepository::GetResolvedDatabasePath() const {
  std::lock_guard lock(impl_->sessionMutex);
  return GetResolvedDatabasePathLocked();
}

std::filesystem::path ReplayRepository::GetResolvedProfileRoot() const {
  std::lock_guard lock(impl_->sessionMutex);
  return GetResolvedDatabasePathLocked().parent_path();
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
    if (sessionSchemaMarkerIsCurrent(
            impl_->sessionDatabase, impl_->sessionSchemaVersion,
            impl_->sessionUserVersion)) {
      errorMessage.clear();
      return true;
    }
    if (replay_repository_detail::MigrateSchema(impl_->sessionDatabase)) {
      rememberSessionSchemaMarker(impl_->sessionDatabase,
                                  impl_->sessionSchemaVersion,
                                  impl_->sessionUserVersion);
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
  if (!replay_repository_detail::PrepareReplayDatabaseOnConnection(
          candidate.get(), resolvedPath)) {
    errorMessage = "replay database validation failed";
    return false;
  }

  sqlite3 *oldDatabase = impl_->sessionDatabase;
  impl_->sessionDatabase = candidate.release();
  impl_->databasePath = std::move(databasePath);
  closeSqliteDatabase(oldDatabase);
  rememberSessionSchemaMarker(impl_->sessionDatabase,
                              impl_->sessionSchemaVersion,
                              impl_->sessionUserVersion);
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
  impl_->sessionSchemaVersion = -1;
  impl_->sessionUserVersion = -1;
  closeSqliteDatabase(database);
}

bool ReplayRepository::EnsureSessionDatabaseLocked() {
  if (impl_->sessionDatabase != nullptr) {
    if (sqlite3_get_autocommit(impl_->sessionDatabase) != 0) {
      if (sessionSchemaMarkerIsCurrent(
              impl_->sessionDatabase, impl_->sessionSchemaVersion,
              impl_->sessionUserVersion)) {
        return true;
      }
      if (!replay_repository_detail::MigrateSchema(impl_->sessionDatabase)) {
        return false;
      }
      rememberSessionSchemaMarker(impl_->sessionDatabase,
                                  impl_->sessionSchemaVersion,
                                  impl_->sessionUserVersion);
      return true;
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
  if (!replay_repository_detail::PrepareReplayDatabaseOnConnection(
          candidate.get(), path)) {
    return false;
  }
  impl_->sessionDatabase = candidate.release();
  rememberSessionSchemaMarker(impl_->sessionDatabase,
                              impl_->sessionSchemaVersion,
                              impl_->sessionUserVersion);
  return true;
}

bool ReplayRepository::EnsureSchema() {
  profile_database_activity::WriteGuard operation;
  std::lock_guard lock(impl_->sessionMutex);
  return EnsureSessionDatabaseLocked();
}
