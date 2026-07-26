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
  removeStaleReplayTemporaryFiles(GetResolvedDatabasePathLocked());
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
    if (replay_repository_detail::MigrateSchema(impl_->sessionDatabase)) {
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
          candidate.get())) {
    errorMessage = "replay database validation failed";
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
      return replay_repository_detail::MigrateSchema(impl_->sessionDatabase);
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
          candidate.get())) {
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
