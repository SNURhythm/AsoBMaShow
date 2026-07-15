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
      return score_repository_detail::CurrentSchemaIsValid(
          impl_->sessionDatabase);
    }
    SDL_Log("Discarding score database with an unfinished transaction");
    CloseSessionDatabaseLocked();
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
