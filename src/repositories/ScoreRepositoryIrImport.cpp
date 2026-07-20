#include "ScoreRepository.h"
#include "ScoreRepositoryInternal.h"

#include "../ProfileDatabaseActivity.h"
#include "../ResultPersistenceModel.h"
#include "../ScoreProvenance.h"
#include "../ir/IrProfileSettings.h"
#include "../ir/IrOutboxModels.h"
#include "ScoreCacheQueries.h"
#include "SqliteRAII.h"

#include <ctime>
#include <iomanip>
#include <limits>
#include <mutex>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>

namespace {

bool validProviderId(std::string_view value) {
  if (value.empty() || value.size() > ir::kMaximumIrProviderIdBytes ||
      value.front() < 'a' || value.front() > 'z') {
    return false;
  }
  return std::ranges::all_of(value, [](unsigned char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9') || character == '_' ||
           character == '-';
  });
}

std::optional<std::string> formatUnixMillis(std::int64_t unixMillis) {
  if (unixMillis <= 0) {
    return std::nullopt;
  }
  const std::time_t seconds = static_cast<std::time_t>(unixMillis / 1'000);
  std::tm utc{};
#if defined(_WIN32)
  if (gmtime_s(&utc, &seconds) != 0) {
    return std::nullopt;
  }
#else
  if (gmtime_r(&seconds, &utc) == nullptr) {
    return std::nullopt;
  }
#endif
  std::ostringstream output;
  output << std::put_time(&utc, "%Y-%m-%d %H:%M:%S") << '.'
         << std::setw(3) << std::setfill('0') << (unixMillis % 1'000);
  return output.str();
}

bool bindText(sqlite3_stmt *statement, int index, std::string_view value) {
  return value.size() <=
             static_cast<std::size_t>(std::numeric_limits<int>::max()) &&
         sqlite3_bind_text(statement, index, value.data(),
                           static_cast<int>(value.size()), SQLITE_TRANSIENT) ==
             SQLITE_OK;
}

bool importedScoresAreCurrentOnConnection(
    sqlite3 *database, std::string_view providerId,
    std::string_view serverOrigin, std::int64_t syncGeneration,
    std::size_t scoreCount) {
  if (scoreCount > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return false;
  }
  SqliteStatementHandle statement;
  constexpr const char *query =
      "SELECT COUNT(*), COALESCE(MIN(source_sync_generation),0), "
      "COALESCE(MAX(source_sync_generation),0) FROM scores WHERE "
      "score_source=? AND source_provider_id=? AND source_server_origin=?";
  if (prepareSqliteStatement(database, query, statement) != SQLITE_OK ||
      sqlite3_bind_int(statement.get(), 1,
                       static_cast<int>(ScoreStorageSource::ImportedIr)) !=
          SQLITE_OK ||
      !bindText(statement.get(), 2, providerId) ||
      !bindText(statement.get(), 3, serverOrigin) ||
      sqlite3_step(statement.get()) != SQLITE_ROW) {
    return false;
  }
  const sqlite3_int64 count = sqlite3_column_int64(statement.get(), 0);
  const sqlite3_int64 minimum = sqlite3_column_int64(statement.get(), 1);
  const sqlite3_int64 maximum = sqlite3_column_int64(statement.get(), 2);
  if (sqlite3_step(statement.get()) != SQLITE_DONE || count < 0) {
    return false;
  }
  if (scoreCount == 0) {
    return count == 0;
  }
  return syncGeneration > 0 &&
         count == static_cast<sqlite3_int64>(scoreCount) &&
         minimum == syncGeneration && maximum == syncGeneration;
}

result_persistence::ChartScoreWrite
importedWrite(const ir::IrRemoteScore &remote) {
  return {
      .chartPath = {},
      .chartMd5 = remote.chartMd5,
      .chartSha256 = remote.chartSha256,
      .chartTitle = remote.title,
      .chartArtist = remote.artist,
      .longNoteMode = -1,
      .score = remote.score,
      .maxScore = remote.noteCount * 2,
      .maxCombo = remote.maxCombo.value_or(0),
      .comboBreak = 0,
      .pGreat = remote.judgements.pGreat.value_or(0),
      .great = remote.judgements.great.value_or(0),
      .good = remote.judgements.good.value_or(0),
      .bad = remote.judgements.bad.value_or(0),
      .poor = remote.judgements.poor.value_or(0),
      .kPoor = 0,
      .fast = remote.fast.value_or(0),
      .slow = remote.slow.value_or(0),
      .finalGauge = remote.finalGauge.value_or(0.0F),
      .clearType = remote.lampRank,
      .provenance = ScoreProvenance::Legacy(),
  };
}

ImportedIrScoreProjectionOutcome invalidOutcome(std::string diagnostic) {
  return {.status = ImportedIrScoreProjectionStatus::Invalid,
          .diagnostic = ir::sanitizeDiagnostic(diagnostic)};
}

} // namespace

bool ScoreRepository::ImportedIrScoresAreCurrent(
    std::string_view providerId, std::string_view serverOrigin,
    std::int64_t syncGeneration, std::size_t scoreCount) {
  const auto normalizedOrigin = ir::normalizeServerOrigin(serverOrigin);
  if (!validProviderId(providerId) || !normalizedOrigin ||
      *normalizedOrigin != serverOrigin ||
      scoreCount > ir::kMaximumIrRemoteScoreSnapshotEntries ||
      (scoreCount > 0 && syncGeneration <= 0)) {
    return false;
  }
  profile_database_activity::ReadGuard operation;
  std::lock_guard lock(impl_->sessionMutex);
  return EnsureSessionDatabaseLocked() &&
         importedScoresAreCurrentOnConnection(
             impl_->sessionDatabase, providerId, serverOrigin, syncGeneration,
             scoreCount);
}

ImportedIrScoreProjectionOutcome ScoreRepository::ReplaceImportedIrScores(
    std::string_view providerId, std::string_view serverOrigin,
    std::int64_t syncGeneration,
    std::span<const ir::IrRemoteScore> remoteScores) {
  const auto normalizedOrigin = ir::normalizeServerOrigin(serverOrigin);
  if (!validProviderId(providerId) || !normalizedOrigin ||
      *normalizedOrigin != serverOrigin || syncGeneration <= 0 ||
      remoteScores.size() > ir::kMaximumIrRemoteScoreSnapshotEntries) {
    return invalidOutcome("Imported IR score snapshot identity is invalid");
  }

  std::unordered_set<std::string_view> remoteIds;
  remoteIds.reserve(remoteScores.size());
  for (const auto &remote : remoteScores) {
    std::string diagnostic;
    if (!ir::validateIrRemoteScore(remote, diagnostic)) {
      return invalidOutcome(diagnostic);
    }
    if (remote.chartMd5.empty() || remote.chartSha256.empty()) {
      return invalidOutcome(
          "Imported IR score requires both chart hashes");
    }
    if (!remoteIds.emplace(remote.remoteScoreId).second) {
      return invalidOutcome(
          "Imported IR score snapshot has duplicate identity");
    }
  }

  const ScoreProvenance provenance = ScoreProvenance::Legacy();
  std::string provenanceError;
  const auto provenanceJson =
      serializeValidatedScoreProvenance(provenance, provenanceError);
  if (!provenanceJson.has_value()) {
    return invalidOutcome("Imported IR score provenance is invalid: " +
                          provenanceError);
  }

  profile_database_activity::WriteGuard operation;
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {.status = ImportedIrScoreProjectionStatus::StorageFailure,
            .diagnostic = "score storage is unavailable"};
  }
  sqlite3 *database = impl_->sessionDatabase;
  if (importedScoresAreCurrentOnConnection(
          database, providerId, serverOrigin, syncGeneration,
          remoteScores.size())) {
    return {.status = ImportedIrScoreProjectionStatus::AlreadyCurrent,
            .projectedScores = static_cast<int>(remoteScores.size())};
  }

  std::string transactionError;
  SqliteTransactionHandle transaction(database, "BEGIN IMMEDIATE TRANSACTION",
                                      transactionError);
  if (!transaction.active()) {
    return {.status = ImportedIrScoreProjectionStatus::StorageFailure,
            .diagnostic = "could not start imported IR score transaction"};
  }

  SqliteStatementHandle deletion;
  constexpr const char *deleteQuery =
      "DELETE FROM scores WHERE score_source=? AND source_provider_id=? AND "
      "source_server_origin=?";
  if (prepareSqliteStatement(database, deleteQuery, deletion) != SQLITE_OK ||
      sqlite3_bind_int(deletion.get(), 1,
                       static_cast<int>(ScoreStorageSource::ImportedIr)) !=
          SQLITE_OK ||
      !bindText(deletion.get(), 2, providerId) ||
      !bindText(deletion.get(), 3, serverOrigin) ||
      sqlite3_step(deletion.get()) != SQLITE_DONE) {
    return {.status = ImportedIrScoreProjectionStatus::StorageFailure,
            .diagnostic = "could not replace imported IR score rows"};
  }

  for (const auto &remote : remoteScores) {
    result_persistence::ChartScoreWrite write = importedWrite(remote);
    const auto createdAt = formatUnixMillis(
        remote.timeAchievedUnixMillis.value_or(remote.timeAddedUnixMillis));
    if (!createdAt.has_value()) {
      return invalidOutcome("Imported IR score timestamp is invalid");
    }
    const score_repository_detail::ScoreStorageMetadata storage{
        .source = ScoreStorageSource::ImportedIr,
        .providerId = providerId,
        .serverOrigin = serverOrigin,
        .remoteScoreId = remote.remoteScoreId,
        .syncGeneration = syncGeneration,
    };
    const auto inserted = score_repository_detail::InsertScoreWriteOnConnection(
        database, write, std::nullopt, *createdAt, *provenanceJson, storage);
    if (inserted.status != score_repository_detail::ScoreWriteStatus::Inserted) {
      return {.status = ImportedIrScoreProjectionStatus::StorageFailure,
              .diagnostic = inserted.diagnostic.empty()
                                ? "could not persist imported IR score"
                                : inserted.diagnostic};
    }
  }

  if (const auto error =
          score_cache_queries::rebuildScoreSummaryTables(database)) {
    return {.status = ImportedIrScoreProjectionStatus::StorageFailure,
            .diagnostic = "could not rebuild imported IR score summaries: " +
                          *error};
  }
  if (!transaction.commit(transactionError)) {
    return {.status = ImportedIrScoreProjectionStatus::StorageFailure,
            .diagnostic = "could not commit imported IR score snapshot"};
  }
  score_repository_detail::IncrementRevision();
  return {.status = ImportedIrScoreProjectionStatus::Applied,
          .projectedScores = static_cast<int>(remoteScores.size())};
}

ImportedIrScoreProjectionOutcome
ScoreRepository::ClearImportedIrScores(std::string_view providerId) {
  if (!validProviderId(providerId)) {
    return invalidOutcome("Imported IR score provider identity is invalid");
  }

  profile_database_activity::WriteGuard operation;
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {.status = ImportedIrScoreProjectionStatus::StorageFailure,
            .diagnostic = "score storage is unavailable"};
  }

  sqlite3 *database = impl_->sessionDatabase;
  std::string transactionError;
  SqliteTransactionHandle transaction(database, "BEGIN IMMEDIATE TRANSACTION",
                                      transactionError);
  if (!transaction.active()) {
    return {.status = ImportedIrScoreProjectionStatus::StorageFailure,
            .diagnostic = "could not start imported IR score transaction"};
  }

  SqliteStatementHandle deletion;
  constexpr const char *deleteQuery =
      "DELETE FROM scores WHERE score_source=? AND source_provider_id=?";
  if (prepareSqliteStatement(database, deleteQuery, deletion) != SQLITE_OK ||
      sqlite3_bind_int(deletion.get(), 1,
                       static_cast<int>(ScoreStorageSource::ImportedIr)) !=
          SQLITE_OK ||
      !bindText(deletion.get(), 2, providerId) ||
      sqlite3_step(deletion.get()) != SQLITE_DONE) {
    return {.status = ImportedIrScoreProjectionStatus::StorageFailure,
            .diagnostic = "could not clear imported IR score rows"};
  }
  const int removedScores = sqlite3_changes(database);
  if (removedScores == 0) {
    return {.status = ImportedIrScoreProjectionStatus::AlreadyCurrent};
  }

  if (const auto error =
          score_cache_queries::rebuildScoreSummaryTables(database)) {
    return {.status = ImportedIrScoreProjectionStatus::StorageFailure,
            .diagnostic = "could not rebuild imported IR score summaries: " +
                          *error};
  }
  if (!transaction.commit(transactionError)) {
    return {.status = ImportedIrScoreProjectionStatus::StorageFailure,
            .diagnostic = "could not commit imported IR score clear"};
  }
  score_repository_detail::IncrementRevision();
  return {.status = ImportedIrScoreProjectionStatus::Applied,
          .projectedScores = removedScores};
}
