#include "ReplayRepository.h"
#include "ReplayRepositoryInternal.h"
#ifdef ASOBMASHOW_ENABLE_REPLAY_MIGRATION_TEST_ACCESS
#include "ReplayRepositoryMigrationTestAccess.h"
#endif

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

#ifdef ASOBMASHOW_ENABLE_REPLAY_MIGRATION_TEST_ACCESS
void *gSessionSchemaValidatedContext = nullptr;
replay_repository_test::SessionSchemaValidatedHook
    gSessionSchemaValidatedHook = nullptr;

void runSessionSchemaValidatedHook() {
  if (gSessionSchemaValidatedHook != nullptr) {
    gSessionSchemaValidatedHook(gSessionSchemaValidatedContext);
  }
}
#else
void runSessionSchemaValidatedHook() {}
#endif

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

bool readSessionSchemaMarker(sqlite3 *database, int &schemaVersion,
                             int &userVersion) {
  const auto observedSchemaVersion =
      readPragmaInt(database, "PRAGMA schema_version");
  const auto observedUserVersion =
      readPragmaInt(database, "PRAGMA user_version");
  if (!observedSchemaVersion || !observedUserVersion) {
    return false;
  }
  schemaVersion = *observedSchemaVersion;
  userVersion = *observedUserVersion;
  return true;
}

bool sessionSchemaMarkerIsCurrent(sqlite3 *database, int schemaVersion,
                                  int userVersion) {
  if (schemaVersion < 0 || userVersion < 0) {
    return false;
  }
  return readPragmaInt(database, "PRAGMA schema_version") == schemaVersion &&
         readPragmaInt(database, "PRAGMA user_version") == userVersion;
}

bool validateAndRememberSessionSchemaMarker(sqlite3 *database,
                                            int &schemaVersion,
                                            int &userVersion) {
  std::string transactionError;
  SqliteTransactionHandle transaction(database, "BEGIN IMMEDIATE TRANSACTION",
                                      transactionError);
  if (!transaction.active()) {
    SDL_Log("Could not lock replay database schema validation: %s",
            transactionError.c_str());
    return false;
  }
  if (!replay_repository_detail::MigrateSchema(database)) {
    return false;
  }
  runSessionSchemaValidatedHook();

  int observedSchemaVersion = -1;
  int observedUserVersion = -1;
  if (!readSessionSchemaMarker(database, observedSchemaVersion,
                               observedUserVersion)) {
    return false;
  }
  if (!transaction.commit(transactionError)) {
    SDL_Log("Could not commit replay database schema validation: %s",
            transactionError.c_str());
    return false;
  }
  schemaVersion = observedSchemaVersion;
  userVersion = observedUserVersion;
  return true;
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
    if (validateAndRememberSessionSchemaMarker(
            impl_->sessionDatabase, impl_->sessionSchemaVersion,
            impl_->sessionUserVersion)) {
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

  int schemaVersion = -1;
  int userVersion = -1;
  if (!validateAndRememberSessionSchemaMarker(candidate.get(), schemaVersion,
                                              userVersion)) {
    errorMessage = "replay database validation failed";
    return false;
  }
  sqlite3 *oldDatabase = impl_->sessionDatabase;
  impl_->sessionDatabase = candidate.release();
  impl_->databasePath = std::move(databasePath);
  impl_->sessionSchemaVersion = schemaVersion;
  impl_->sessionUserVersion = userVersion;
  closeSqliteDatabase(oldDatabase);
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
      return validateAndRememberSessionSchemaMarker(
          impl_->sessionDatabase, impl_->sessionSchemaVersion,
          impl_->sessionUserVersion);
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
  int schemaVersion = -1;
  int userVersion = -1;
  if (!validateAndRememberSessionSchemaMarker(candidate.get(), schemaVersion,
                                              userVersion)) {
    return false;
  }
  impl_->sessionDatabase = candidate.release();
  impl_->sessionSchemaVersion = schemaVersion;
  impl_->sessionUserVersion = userVersion;
  return true;
}

bool ReplayRepository::EnsureSchema() {
  profile_database_activity::WriteGuard operation;
  std::lock_guard lock(impl_->sessionMutex);
  return EnsureSessionDatabaseLocked();
}

#ifdef ASOBMASHOW_ENABLE_REPLAY_MIGRATION_TEST_ACCESS
void replay_repository_test::SetSessionSchemaValidatedHook(
    void *context, SessionSchemaValidatedHook hook) {
  gSessionSchemaValidatedContext = context;
  gSessionSchemaValidatedHook = hook;
}
#endif
