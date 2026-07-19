#include "ReplayRepository.h"
#include "ReplayRepositoryInternal.h"

#include "../ir/IrProfileSettings.h"
#include "../ProfileDatabaseActivity.h"
#include "../Uuid.h"
#include "SqliteRAII.h"

#include <algorithm>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

namespace {

constexpr const char *kIrOutboxColumns =
    "id,provider_id,attempt_id,chart_md5,chart_sha256,payload_json,"
    "ruleset_id,ruleset_revision,validation_fingerprint,state,"
    "local_result_ready,request_attempt_count,consecutive_failure_count,"
    "remote_poll_count,next_attempt_at_ms,next_request_user_intent,"
    "remote_job_id,remote_origin,last_error_code,last_error_message,"
    "created_at_ms,updated_at_ms,completed_at_ms";

constexpr const char *kIrSubmissionReceiptColumns =
    "id,provider_id,server_origin,replay_id,attempt_id,chart_md5,chart_sha256,"
    "remote_user_id,remote_chart_id,remote_score_id,confirmation_source,"
    "observed_in_snapshot,confirmed_at_ms";

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

bool columnIs(sqlite3_stmt *stmt, int column, int type) {
  return sqlite3_column_type(stmt, column) == type;
}

bool nullableInteger(sqlite3_stmt *stmt, int column) {
  const int type = sqlite3_column_type(stmt, column);
  return type == SQLITE_NULL || type == SQLITE_INTEGER;
}

bool nullableText(sqlite3_stmt *stmt, int column) {
  const int type = sqlite3_column_type(stmt, column);
  return type == SQLITE_NULL || type == SQLITE_TEXT;
}

std::optional<std::int64_t> optionalInteger(sqlite3_stmt *stmt, int column) {
  if (sqlite3_column_type(stmt, column) == SQLITE_NULL) {
    return std::nullopt;
  }
  return sqlite3_column_int64(stmt, column);
}

std::string optionalText(sqlite3_stmt *stmt, int column) {
  return sqlite3_column_type(stmt, column) == SQLITE_NULL
             ? std::string{}
             : sqliteColumnString(stmt, column);
}

bool decodeIrOutboxRow(sqlite3_stmt *stmt, ir::IrOutboxEntry &entry,
                       std::string &diagnostic) {
  if (!columnIs(stmt, 0, SQLITE_INTEGER) || !columnIs(stmt, 1, SQLITE_TEXT) ||
      !columnIs(stmt, 2, SQLITE_TEXT) || !nullableText(stmt, 3) ||
      !columnIs(stmt, 4, SQLITE_TEXT) || !columnIs(stmt, 5, SQLITE_TEXT) ||
      !columnIs(stmt, 6, SQLITE_TEXT) ||
      !columnIs(stmt, 7, SQLITE_INTEGER) ||
      !columnIs(stmt, 8, SQLITE_TEXT) ||
      !columnIs(stmt, 9, SQLITE_INTEGER) ||
      !columnIs(stmt, 10, SQLITE_INTEGER) ||
      !columnIs(stmt, 11, SQLITE_INTEGER) ||
      !columnIs(stmt, 12, SQLITE_INTEGER) ||
      !columnIs(stmt, 13, SQLITE_INTEGER) || !nullableInteger(stmt, 14) ||
      !columnIs(stmt, 15, SQLITE_INTEGER) || !nullableText(stmt, 16) ||
      !nullableText(stmt, 17) || !nullableText(stmt, 18) ||
      !nullableText(stmt, 19) || !columnIs(stmt, 20, SQLITE_INTEGER) ||
      !columnIs(stmt, 21, SQLITE_INTEGER) || !nullableInteger(stmt, 22)) {
    diagnostic = "IR outbox row has unexpected SQLite value types";
    return false;
  }
  const int state = sqlite3_column_int(stmt, 9);
  const int localReady = sqlite3_column_int(stmt, 10);
  const int userIntent = sqlite3_column_int(stmt, 15);
  if (!ir::isKnownIrOutboxState(state) ||
      (localReady != 0 && localReady != 1) ||
      (userIntent != 0 && userIntent != 1)) {
    diagnostic = "IR outbox row contains an unknown state or boolean";
    return false;
  }
  entry = {
      .id = sqlite3_column_int64(stmt, 0),
      .providerId = sqliteColumnString(stmt, 1),
      .attemptId = sqliteColumnString(stmt, 2),
      .chartMd5 = optionalText(stmt, 3),
      .chartSha256 = sqliteColumnString(stmt, 4),
      .payloadJson = sqliteColumnString(stmt, 5),
      .rulesetProof =
          {
              .rulesetId = sqliteColumnString(stmt, 6),
              .rulesetRevision = sqlite3_column_int(stmt, 7),
              .validationFingerprint = sqliteColumnString(stmt, 8),
          },
      .state = static_cast<ir::IrOutboxState>(state),
      .localResultReady = localReady != 0,
      .requestAttemptCount = sqlite3_column_int(stmt, 11),
      .consecutiveFailureCount = sqlite3_column_int(stmt, 12),
      .remotePollCount = sqlite3_column_int(stmt, 13),
      .nextAttemptAtUnixMillis = optionalInteger(stmt, 14),
      .nextRequestUserIntent = userIntent != 0,
      .remoteJobId = optionalText(stmt, 16),
      .remoteOrigin = optionalText(stmt, 17),
      .lastErrorCode = optionalText(stmt, 18),
      .lastErrorMessage = optionalText(stmt, 19),
      .createdAtUnixMillis = sqlite3_column_int64(stmt, 20),
      .updatedAtUnixMillis = sqlite3_column_int64(stmt, 21),
      .completedAtUnixMillis = optionalInteger(stmt, 22),
  };
  return ir::validateIrOutboxEntry(entry, diagnostic);
}

bool decodeIrSubmissionReceiptRow(sqlite3_stmt *stmt,
                                  ir::IrSubmissionReceipt &receipt,
                                  std::string &diagnostic) {
  if (!columnIs(stmt, 0, SQLITE_INTEGER) || !columnIs(stmt, 1, SQLITE_TEXT) ||
      !columnIs(stmt, 2, SQLITE_TEXT) || !columnIs(stmt, 3, SQLITE_INTEGER) ||
      !columnIs(stmt, 4, SQLITE_TEXT) || !nullableText(stmt, 5) ||
      !columnIs(stmt, 6, SQLITE_TEXT) || !nullableInteger(stmt, 7) ||
      !nullableText(stmt, 8) || !nullableText(stmt, 9) ||
      !columnIs(stmt, 10, SQLITE_INTEGER) ||
      !columnIs(stmt, 11, SQLITE_INTEGER) ||
      !columnIs(stmt, 12, SQLITE_INTEGER)) {
    diagnostic = "IR receipt row has unexpected SQLite value types";
    return false;
  }
  const sqlite3_int64 replayId = sqlite3_column_int64(stmt, 3);
  if (replayId <= 0 || replayId > std::numeric_limits<int>::max()) {
    diagnostic = "IR receipt replay ID is out of range";
    return false;
  }
  receipt = {
      .id = sqlite3_column_int64(stmt, 0),
      .providerId = sqliteColumnString(stmt, 1),
      .serverOrigin = sqliteColumnString(stmt, 2),
      .replayId = static_cast<int>(replayId),
      .attemptId = sqliteColumnString(stmt, 4),
      .chartMd5 = optionalText(stmt, 5),
      .chartSha256 = sqliteColumnString(stmt, 6),
      .remoteUserId = optionalInteger(stmt, 7),
      .remoteChartId = optionalText(stmt, 8),
      .remoteScoreId = optionalText(stmt, 9),
      .source = static_cast<ir::IrReceiptConfirmationSource>(
          sqlite3_column_int(stmt, 10)),
      .observedInSnapshot = sqlite3_column_int(stmt, 11) != 0,
      .confirmedAtUnixMillis = sqlite3_column_int64(stmt, 12),
  };
  return ir::validateIrSubmissionReceipt(receipt, diagnostic);
}

enum class RowLookupStatus { Found, NotFound, StorageFailure, Invalid };

struct RowLookup {
  RowLookupStatus status = RowLookupStatus::StorageFailure;
  std::optional<ir::IrOutboxEntry> entry;
  std::string diagnostic;
};

RowLookup loadByQuery(sqlite3 *db, const std::string &query, const auto &bind) {
  SqliteStatementHandle stmt;
  if (prepareSqliteStatement(db, query, stmt) != SQLITE_OK ||
      !bind(stmt.get())) {
    return {.status = RowLookupStatus::StorageFailure,
            .diagnostic = "could not prepare IR outbox lookup"};
  }
  const int rc = sqlite3_step(stmt.get());
  if (rc == SQLITE_DONE) {
    return {.status = RowLookupStatus::NotFound};
  }
  if (rc != SQLITE_ROW) {
    return {.status = RowLookupStatus::StorageFailure,
            .diagnostic = "IR outbox lookup did not complete"};
  }
  ir::IrOutboxEntry entry;
  std::string diagnostic;
  if (!decodeIrOutboxRow(stmt.get(), entry, diagnostic)) {
    return {.status = RowLookupStatus::Invalid,
            .diagnostic = std::move(diagnostic)};
  }
  if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
    return {.status = RowLookupStatus::StorageFailure,
            .diagnostic = "IR outbox lookup returned duplicate rows"};
  }
  return {.status = RowLookupStatus::Found, .entry = std::move(entry)};
}

RowLookup loadById(sqlite3 *db, std::int64_t rowId) {
  return loadByQuery(db,
                     std::string("SELECT ") + kIrOutboxColumns +
                         " FROM ir_outbox WHERE id=?",
                     [&](sqlite3_stmt *stmt) {
                       return sqlite3_bind_int64(stmt, 1, rowId) == SQLITE_OK;
                     });
}

RowLookup loadByIdentity(sqlite3 *db, std::string_view providerId,
                         std::string_view attemptId) {
  return loadByQuery(
      db,
      std::string("SELECT ") + kIrOutboxColumns +
          " FROM ir_outbox WHERE provider_id=? AND attempt_id=?",
      [&](sqlite3_stmt *stmt) {
        return sqlite3_bind_text(stmt, 1, providerId.data(),
                                 static_cast<int>(providerId.size()),
                                 SQLITE_TRANSIENT) == SQLITE_OK &&
               sqlite3_bind_text(stmt, 2, attemptId.data(),
                                 static_cast<int>(attemptId.size()),
                                 SQLITE_TRANSIENT) == SQLITE_OK;
      });
}

bool bindOptionalInteger(sqlite3_stmt *stmt, int column,
                         const std::optional<std::int64_t> &value) {
  return value ? sqlite3_bind_int64(stmt, column, *value) == SQLITE_OK
               : sqlite3_bind_null(stmt, column) == SQLITE_OK;
}

bool bindOptionalText(sqlite3_stmt *stmt, int column,
                      const std::optional<std::string> &value) {
  return value ? bindSqliteText(stmt, column, *value)
               : sqlite3_bind_null(stmt, column) == SQLITE_OK;
}

ir::IrOutboxMutationOutcome mutationFromChanges(sqlite3 *db) {
  const int changes = sqlite3_changes(db);
  return {.status = changes > 0 ? ir::IrOutboxMutationStatus::Updated
                                : ir::IrOutboxMutationStatus::NotFound,
          .affectedRows = static_cast<std::size_t>(std::max(0, changes))};
}

} // namespace

replay_repository_detail::IrDraftStageOutcome
replay_repository_detail::ValidateIrDraftsForAttempt(
    const result_persistence::ChartResultAttempt &attempt,
    std::span<const ir::IrOutboxDraft> drafts) {
  using replay_repository_detail::IrDraftStageStatus;
  std::vector<const ir::IrOutboxDraft *> sorted;
  sorted.reserve(drafts.size());
  for (const ir::IrOutboxDraft &draft : drafts) {
    std::string diagnostic;
    if (!ir::validateIrOutboxDraft(draft, diagnostic)) {
      return {.status = IrDraftStageStatus::IntegrityConflict,
              .diagnostic = std::move(diagnostic)};
    }
    if (draft.attemptId != attempt.attemptId) {
      return {.status = IrDraftStageStatus::IntegrityConflict,
              .diagnostic = "IR draft attempt identity does not match the "
                            "chart result"};
    }
    if (draft.chartMd5 != attempt.score.chartMd5 ||
        draft.chartSha256 != attempt.score.chartSha256) {
      return {.status = IrDraftStageStatus::IntegrityConflict,
              .diagnostic =
                  "IR draft chart identity does not match the chart result"};
    }
    sorted.push_back(&draft);
  }
  std::ranges::sort(sorted, {}, &ir::IrOutboxDraft::providerId);
  for (std::size_t index = 1; index < sorted.size(); ++index) {
    if (sorted[index - 1]->providerId == sorted[index]->providerId) {
      return {.status = IrDraftStageStatus::IntegrityConflict,
              .diagnostic =
                  "automatic IR drafts contain a duplicate provider ID"};
    }
  }
  return {.status = IrDraftStageStatus::Succeeded};
}

replay_repository_detail::IrDraftStageOutcome
replay_repository_detail::InsertInactiveIrDraftsOnConnection(
    sqlite3 *database, std::span<const ir::IrOutboxDraft> drafts) {
  using replay_repository_detail::IrDraftStageStatus;
  if (drafts.empty()) {
    return {.status = IrDraftStageStatus::Succeeded};
  }
  SqliteStatementHandle insert;
  constexpr const char *query =
      "INSERT INTO ir_outbox("
      "provider_id,attempt_id,chart_md5,chart_sha256,payload_json,ruleset_id,"
      "ruleset_revision,validation_fingerprint,state,"
      "local_result_ready,next_request_user_intent,created_at_ms,updated_at_ms)"
      " VALUES(?,?,?,?,?,?,?,?,0,0,0,?,?)";
  if (prepareSqliteStatement(database, query, insert) != SQLITE_OK) {
    return {.status = IrDraftStageStatus::StorageFailure,
            .diagnostic = "could not prepare automatic IR draft staging"};
  }
  for (const ir::IrOutboxDraft &draft : drafts) {
    if (sqlite3_reset(insert.get()) != SQLITE_OK ||
        sqlite3_clear_bindings(insert.get()) != SQLITE_OK ||
        !bindSqliteText(insert.get(), 1, draft.providerId) ||
        !bindSqliteText(insert.get(), 2, draft.attemptId) ||
        (draft.chartMd5.empty()
             ? sqlite3_bind_null(insert.get(), 3) != SQLITE_OK
             : !bindSqliteText(insert.get(), 3, draft.chartMd5)) ||
        !bindSqliteText(insert.get(), 4, draft.chartSha256) ||
        !bindSqliteText(insert.get(), 5, draft.payloadJson) ||
        !bindSqliteText(insert.get(), 6, draft.rulesetProof.rulesetId) ||
        sqlite3_bind_int(insert.get(), 7,
                         draft.rulesetProof.rulesetRevision) != SQLITE_OK ||
        !bindSqliteText(insert.get(), 8,
                        draft.rulesetProof.validationFingerprint) ||
        sqlite3_bind_int64(insert.get(), 9, draft.createdAtUnixMillis) !=
            SQLITE_OK ||
        sqlite3_bind_int64(insert.get(), 10, draft.createdAtUnixMillis) !=
            SQLITE_OK ||
        sqlite3_step(insert.get()) != SQLITE_DONE ||
        sqlite3_changes(database) != 1) {
      return {.status = IrDraftStageStatus::StorageFailure,
              .diagnostic = "could not stage an automatic IR draft"};
    }
  }
  return {.status = IrDraftStageStatus::Succeeded};
}

replay_repository_detail::IrDraftStageOutcome
replay_repository_detail::VerifyIrDraftsOnConnection(
    sqlite3 *database, std::string_view attemptId,
    std::span<const ir::IrOutboxDraft> drafts) {
  using replay_repository_detail::IrDraftStageStatus;
  SqliteStatementHandle statement;
  const std::string query = std::string("SELECT ") + kIrOutboxColumns +
                            " FROM ir_outbox WHERE attempt_id=? ORDER BY "
                            "provider_id";
  if (prepareSqliteStatement(database, query, statement) != SQLITE_OK ||
      sqlite3_bind_text(statement.get(), 1, attemptId.data(),
                        static_cast<int>(attemptId.size()),
                        SQLITE_TRANSIENT) != SQLITE_OK) {
    return {.status = IrDraftStageStatus::StorageFailure,
            .diagnostic = "could not prepare automatic IR draft validation"};
  }
  std::vector<ir::IrOutboxEntry> stored;
  int result = SQLITE_OK;
  while ((result = sqlite3_step(statement.get())) == SQLITE_ROW) {
    ir::IrOutboxEntry entry;
    std::string diagnostic;
    if (!decodeIrOutboxRow(statement.get(), entry, diagnostic)) {
      return {.status = IrDraftStageStatus::IntegrityConflict,
              .diagnostic = std::move(diagnostic)};
    }
    stored.push_back(std::move(entry));
  }
  if (result != SQLITE_DONE) {
    return {.status = IrDraftStageStatus::StorageFailure,
            .diagnostic = "automatic IR draft validation did not complete"};
  }

  std::vector<const ir::IrOutboxDraft *> expected;
  expected.reserve(drafts.size());
  for (const ir::IrOutboxDraft &draft : drafts) {
    expected.push_back(&draft);
  }
  std::ranges::sort(expected, {}, &ir::IrOutboxDraft::providerId);
  if (stored.size() != expected.size()) {
    return {.status = IrDraftStageStatus::IntegrityConflict,
            .diagnostic =
                "stored automatic IR drafts differ from the staged set"};
  }
  for (std::size_t index = 0; index < stored.size(); ++index) {
    const ir::IrOutboxEntry &entry = stored[index];
    const ir::IrOutboxDraft &draft = *expected[index];
    if (entry.providerId != draft.providerId ||
        entry.attemptId != draft.attemptId ||
        entry.chartMd5 != draft.chartMd5 ||
        entry.chartSha256 != draft.chartSha256 ||
        entry.payloadJson != draft.payloadJson ||
        entry.rulesetProof != draft.rulesetProof ||
        entry.createdAtUnixMillis != draft.createdAtUnixMillis) {
      return {.status = IrDraftStageStatus::IntegrityConflict,
              .diagnostic =
                  "stored automatic IR draft differs from the staged draft"};
    }
  }
  return {.status = IrDraftStageStatus::Succeeded};
}

ir::IrOutboxInsertOutcome
ReplayRepository::EnqueueReadyIrOutboxDraft(const ir::IrOutboxDraft &draft,
                                            bool userIntent) {
  std::string validation;
  if (!ir::validateIrOutboxDraft(draft, validation)) {
    return {.status = ir::IrOutboxInsertStatus::Invalid,
            .diagnostic = std::move(validation)};
  }
  profile_database_activity::WriteGuard operation;
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {.status = ir::IrOutboxInsertStatus::StorageFailure,
            .diagnostic = "replay storage is unavailable"};
  }
  std::string transactionError;
  SqliteTransactionHandle transaction(
      impl_->sessionDatabase, "BEGIN IMMEDIATE TRANSACTION", transactionError);
  if (!transaction.active()) {
    return {.status = ir::IrOutboxInsertStatus::StorageFailure,
            .diagnostic = "could not start IR outbox insertion"};
  }
  SqliteStatementHandle insert;
  const char *query =
      "INSERT OR IGNORE INTO ir_outbox("
      "provider_id,attempt_id,chart_md5,chart_sha256,payload_json,ruleset_id,"
      "ruleset_revision,validation_fingerprint,state,"
      "local_result_ready,next_request_user_intent,created_at_ms,updated_at_ms)"
      " VALUES(?,?,?,?,?,?,?,?,0,1,?,?,?)";
  if (prepareSqliteStatement(impl_->sessionDatabase, query, insert) !=
          SQLITE_OK ||
      !bindSqliteText(insert.get(), 1, draft.providerId) ||
      !bindSqliteText(insert.get(), 2, draft.attemptId) ||
      (draft.chartMd5.empty()
           ? sqlite3_bind_null(insert.get(), 3) != SQLITE_OK
           : !bindSqliteText(insert.get(), 3, draft.chartMd5)) ||
      !bindSqliteText(insert.get(), 4, draft.chartSha256) ||
      !bindSqliteText(insert.get(), 5, draft.payloadJson) ||
      !bindSqliteText(insert.get(), 6, draft.rulesetProof.rulesetId) ||
      sqlite3_bind_int(insert.get(), 7,
                       draft.rulesetProof.rulesetRevision) != SQLITE_OK ||
      !bindSqliteText(insert.get(), 8,
                      draft.rulesetProof.validationFingerprint) ||
      sqlite3_bind_int(insert.get(), 9, userIntent ? 1 : 0) != SQLITE_OK ||
      sqlite3_bind_int64(insert.get(), 10, draft.createdAtUnixMillis) !=
          SQLITE_OK ||
      sqlite3_bind_int64(insert.get(), 11, draft.createdAtUnixMillis) !=
          SQLITE_OK ||
      sqlite3_step(insert.get()) != SQLITE_DONE) {
    return {.status = ir::IrOutboxInsertStatus::StorageFailure,
            .diagnostic = "could not insert IR outbox row"};
  }
  const bool inserted = sqlite3_changes(impl_->sessionDatabase) == 1;
  RowLookup loaded =
      loadByIdentity(impl_->sessionDatabase, draft.providerId, draft.attemptId);
  if (loaded.status != RowLookupStatus::Found || !loaded.entry) {
    return {.status = loaded.status == RowLookupStatus::Invalid
                          ? ir::IrOutboxInsertStatus::IntegrityConflict
                          : ir::IrOutboxInsertStatus::StorageFailure,
            .diagnostic = std::move(loaded.diagnostic)};
  }
  if (!inserted &&
      (loaded.entry->chartMd5 != draft.chartMd5 ||
       loaded.entry->chartSha256 != draft.chartSha256 ||
       loaded.entry->payloadJson != draft.payloadJson ||
       loaded.entry->rulesetProof != draft.rulesetProof ||
       loaded.entry->createdAtUnixMillis != draft.createdAtUnixMillis)) {
    return {.status = ir::IrOutboxInsertStatus::IntegrityConflict,
            .diagnostic = "IR attempt ID already names another payload"};
  }
  if (!transaction.commit(transactionError)) {
    return {.status = ir::IrOutboxInsertStatus::StorageFailure,
            .diagnostic = "could not commit IR outbox insertion"};
  }
  return {.status = inserted ? ir::IrOutboxInsertStatus::Inserted
                             : ir::IrOutboxInsertStatus::AlreadyExists,
          .entry = std::move(loaded.entry)};
}

ir::IrOutboxReadOutcome
ReplayRepository::LoadIrOutbox(std::string_view providerId,
                               std::string_view attemptId) {
  if (!validProviderId(providerId) || !uuid::isCanonicalLowerV4(attemptId)) {
    return {.status = ir::IrOutboxReadStatus::Invalid,
            .diagnostic = "IR outbox identity is invalid"};
  }
  profile_database_activity::ReadGuard operation;
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {.status = ir::IrOutboxReadStatus::StorageFailure,
            .diagnostic = "replay storage is unavailable"};
  }
  RowLookup loaded =
      loadByIdentity(impl_->sessionDatabase, providerId, attemptId);
  switch (loaded.status) {
  case RowLookupStatus::Found:
    return {.status = ir::IrOutboxReadStatus::Found,
            .entry = std::move(loaded.entry)};
  case RowLookupStatus::NotFound:
    return {.status = ir::IrOutboxReadStatus::NotFound};
  case RowLookupStatus::Invalid:
    return {.status = ir::IrOutboxReadStatus::IntegrityConflict,
            .diagnostic = std::move(loaded.diagnostic)};
  case RowLookupStatus::StorageFailure:
    return {.status = ir::IrOutboxReadStatus::StorageFailure,
            .diagnostic = std::move(loaded.diagnostic)};
  }
  return {};
}

ir::IrOutboxBatchOutcome ReplayRepository::ListDueIrOutbox(std::int64_t nowMs,
                                                           std::size_t limit) {
  if (nowMs < 0 || limit > 256) {
    return {.status = ir::IrOutboxBatchStatus::Invalid,
            .diagnostic = "IR outbox due query is invalid"};
  }
  if (limit == 0) {
    return {.status = ir::IrOutboxBatchStatus::Loaded};
  }
  profile_database_activity::ReadGuard operation;
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {.status = ir::IrOutboxBatchStatus::StorageFailure,
            .diagnostic = "replay storage is unavailable"};
  }
  const std::string query =
      std::string("SELECT ") + kIrOutboxColumns +
      " FROM ir_outbox WHERE local_result_ready=1 AND state IN (0,2) "
      "AND (next_attempt_at_ms IS NULL OR next_attempt_at_ms<=?) "
      "ORDER BY COALESCE(next_attempt_at_ms,0),id LIMIT ?";
  SqliteStatementHandle stmt;
  if (prepareSqliteStatement(impl_->sessionDatabase, query, stmt) !=
          SQLITE_OK ||
      sqlite3_bind_int64(stmt.get(), 1, nowMs) != SQLITE_OK ||
      sqlite3_bind_int64(stmt.get(), 2, static_cast<sqlite3_int64>(limit)) !=
          SQLITE_OK) {
    return {.status = ir::IrOutboxBatchStatus::StorageFailure,
            .diagnostic = "could not prepare IR outbox due query"};
  }
  ir::IrOutboxBatchOutcome result{.status = ir::IrOutboxBatchStatus::Loaded};
  int rc = SQLITE_OK;
  while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
    ir::IrOutboxEntry entry;
    if (!decodeIrOutboxRow(stmt.get(), entry, result.diagnostic)) {
      result.status = ir::IrOutboxBatchStatus::IntegrityConflict;
      result.entries.clear();
      return result;
    }
    result.entries.push_back(std::move(entry));
  }
  if (rc != SQLITE_DONE) {
    return {.status = ir::IrOutboxBatchStatus::StorageFailure,
            .diagnostic = "IR outbox due query did not complete"};
  }
  return result;
}

ir::IrOutboxBatchOutcome ReplayRepository::ListDueIrOutbox(
    std::string_view providerId, std::int64_t nowMs, std::size_t limit) {
  if (!validProviderId(providerId) || nowMs < 0 || limit > 256) {
    return {.status = ir::IrOutboxBatchStatus::Invalid,
            .diagnostic = "IR provider outbox due query is invalid"};
  }
  if (limit == 0) {
    return {.status = ir::IrOutboxBatchStatus::Loaded};
  }
  profile_database_activity::ReadGuard operation;
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {.status = ir::IrOutboxBatchStatus::StorageFailure,
            .diagnostic = "replay storage is unavailable"};
  }
  const std::string query =
      std::string("SELECT ") + kIrOutboxColumns +
      " FROM ir_outbox WHERE local_result_ready=1 AND state IN (0,2) "
      "AND provider_id=? AND "
      "(next_attempt_at_ms IS NULL OR next_attempt_at_ms<=?) "
      "ORDER BY COALESCE(next_attempt_at_ms,0),id LIMIT ?";
  SqliteStatementHandle stmt;
  if (prepareSqliteStatement(impl_->sessionDatabase, query, stmt) !=
          SQLITE_OK ||
      sqlite3_bind_text(stmt.get(), 1, providerId.data(),
                        static_cast<int>(providerId.size()),
                        SQLITE_TRANSIENT) != SQLITE_OK ||
      sqlite3_bind_int64(stmt.get(), 2, nowMs) != SQLITE_OK ||
      sqlite3_bind_int64(stmt.get(), 3, static_cast<sqlite3_int64>(limit)) !=
          SQLITE_OK) {
    return {.status = ir::IrOutboxBatchStatus::StorageFailure,
            .diagnostic = "could not prepare IR provider outbox due query"};
  }
  ir::IrOutboxBatchOutcome result{.status = ir::IrOutboxBatchStatus::Loaded};
  int rc = SQLITE_OK;
  while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
    ir::IrOutboxEntry entry;
    if (!decodeIrOutboxRow(stmt.get(), entry, result.diagnostic)) {
      result.status = ir::IrOutboxBatchStatus::IntegrityConflict;
      result.entries.clear();
      return result;
    }
    result.entries.push_back(std::move(entry));
  }
  if (rc != SQLITE_DONE) {
    return {.status = ir::IrOutboxBatchStatus::StorageFailure,
            .diagnostic = "IR provider outbox due query did not complete"};
  }
  return result;
}

std::optional<std::int64_t> ReplayRepository::NextIrOutboxAttemptAfter(
    std::string_view providerId, std::int64_t nowMs) {
  if (!validProviderId(providerId) || nowMs < 0) {
    return std::nullopt;
  }
  profile_database_activity::ReadGuard operation;
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return std::nullopt;
  }
  SqliteStatementHandle stmt;
  if (prepareSqliteStatement(
          impl_->sessionDatabase,
          "SELECT MIN(next_attempt_at_ms) FROM ir_outbox WHERE "
          "local_result_ready=1 AND state IN (0,2) AND provider_id=? AND "
          "next_attempt_at_ms>?",
          stmt) != SQLITE_OK ||
      sqlite3_bind_text(stmt.get(), 1, providerId.data(),
                        static_cast<int>(providerId.size()),
                        SQLITE_TRANSIENT) != SQLITE_OK ||
      sqlite3_bind_int64(stmt.get(), 2, nowMs) != SQLITE_OK ||
      sqlite3_step(stmt.get()) != SQLITE_ROW ||
      !nullableInteger(stmt.get(), 0)) {
    return std::nullopt;
  }
  return optionalInteger(stmt.get(), 0);
}

ir::IrOutboxBatchOutcome ReplayRepository::ListIrOutbox(std::size_t limit) {
  if (limit > 4096) {
    return {.status = ir::IrOutboxBatchStatus::Invalid,
            .diagnostic = "IR outbox list query is invalid"};
  }
  if (limit == 0) {
    return {.status = ir::IrOutboxBatchStatus::Loaded};
  }
  profile_database_activity::ReadGuard operation;
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {.status = ir::IrOutboxBatchStatus::StorageFailure,
            .diagnostic = "replay storage is unavailable"};
  }
  const std::string query =
      std::string("SELECT ") + kIrOutboxColumns +
      " FROM ir_outbox WHERE local_result_ready=1 ORDER BY updated_at_ms "
      "DESC,id DESC LIMIT ?";
  SqliteStatementHandle statement;
  if (prepareSqliteStatement(impl_->sessionDatabase, query, statement) !=
          SQLITE_OK ||
      sqlite3_bind_int64(statement.get(), 1,
                         static_cast<sqlite3_int64>(limit)) != SQLITE_OK) {
    return {.status = ir::IrOutboxBatchStatus::StorageFailure,
            .diagnostic = "could not prepare IR outbox list query"};
  }
  ir::IrOutboxBatchOutcome result{.status = ir::IrOutboxBatchStatus::Loaded};
  int status = SQLITE_OK;
  while ((status = sqlite3_step(statement.get())) == SQLITE_ROW) {
    ir::IrOutboxEntry entry;
    if (!decodeIrOutboxRow(statement.get(), entry, result.diagnostic)) {
      result.status = ir::IrOutboxBatchStatus::IntegrityConflict;
      result.entries.clear();
      return result;
    }
    result.entries.push_back(std::move(entry));
  }
  if (status != SQLITE_DONE) {
    return {.status = ir::IrOutboxBatchStatus::StorageFailure,
            .diagnostic = "IR outbox list query did not complete"};
  }
  return result;
}

ir::IrOutboxClaimOutcome ReplayRepository::ClaimIrOutbox(
    std::int64_t rowId, ir::IrOutboxState expectedState, std::int64_t nowMs) {
  if (rowId <= 0 || nowMs < 0 ||
      (expectedState != ir::IrOutboxState::Pending &&
       expectedState != ir::IrOutboxState::AwaitingRemoteResult)) {
    return {.status = ir::IrOutboxClaimStatus::Invalid,
            .diagnostic = "IR outbox claim is invalid"};
  }
  profile_database_activity::WriteGuard operation;
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {.status = ir::IrOutboxClaimStatus::StorageFailure,
            .diagnostic = "replay storage is unavailable"};
  }
  std::string transactionError;
  SqliteTransactionHandle transaction(
      impl_->sessionDatabase, "BEGIN IMMEDIATE TRANSACTION", transactionError);
  if (!transaction.active()) {
    return {.status = ir::IrOutboxClaimStatus::StorageFailure,
            .diagnostic = "could not start IR outbox claim"};
  }
  RowLookup before = loadById(impl_->sessionDatabase, rowId);
  if (before.status == RowLookupStatus::NotFound) {
    return {.status = ir::IrOutboxClaimStatus::NotFound};
  }
  if (before.status == RowLookupStatus::Invalid) {
    return {.status = ir::IrOutboxClaimStatus::IntegrityConflict,
            .diagnostic = std::move(before.diagnostic)};
  }
  if (before.status != RowLookupStatus::Found || !before.entry) {
    return {.status = ir::IrOutboxClaimStatus::StorageFailure,
            .diagnostic = std::move(before.diagnostic)};
  }
  if (before.entry->state != expectedState || !before.entry->localResultReady) {
    return {.status = ir::IrOutboxClaimStatus::StateMismatch};
  }
  const bool consumed = expectedState == ir::IrOutboxState::Pending &&
                        before.entry->nextRequestUserIntent;
  SqliteStatementHandle update;
  const char *query =
      "UPDATE ir_outbox SET state=1,request_attempt_count="
      "request_attempt_count+1,next_request_user_intent="
      "CASE WHEN ?=0 THEN 0 ELSE next_request_user_intent END,updated_at_ms=? "
      "WHERE id=? AND state=? AND local_result_ready=1";
  if (prepareSqliteStatement(impl_->sessionDatabase, query, update) !=
          SQLITE_OK ||
      sqlite3_bind_int(update.get(), 1, static_cast<int>(expectedState)) !=
          SQLITE_OK ||
      sqlite3_bind_int64(update.get(), 2, nowMs) != SQLITE_OK ||
      sqlite3_bind_int64(update.get(), 3, rowId) != SQLITE_OK ||
      sqlite3_bind_int(update.get(), 4, static_cast<int>(expectedState)) !=
          SQLITE_OK ||
      sqlite3_step(update.get()) != SQLITE_DONE) {
    return {.status = ir::IrOutboxClaimStatus::StorageFailure,
            .diagnostic = "could not claim IR outbox row"};
  }
  if (sqlite3_changes(impl_->sessionDatabase) != 1) {
    return {.status = ir::IrOutboxClaimStatus::StateMismatch};
  }
  RowLookup after = loadById(impl_->sessionDatabase, rowId);
  if (after.status != RowLookupStatus::Found || !after.entry) {
    return {.status = after.status == RowLookupStatus::Invalid
                          ? ir::IrOutboxClaimStatus::IntegrityConflict
                          : ir::IrOutboxClaimStatus::StorageFailure,
            .diagnostic = std::move(after.diagnostic)};
  }
  if (!transaction.commit(transactionError)) {
    return {.status = ir::IrOutboxClaimStatus::StorageFailure,
            .diagnostic = "could not commit IR outbox claim"};
  }
  return {.status = ir::IrOutboxClaimStatus::Claimed,
          .entry = std::move(after.entry),
          .consumedUserIntent = consumed};
}

ir::IrOutboxMutationOutcome ReplayRepository::BlockIrOutboxConfiguration(
    std::int64_t rowId, ir::IrOutboxState expectedState,
    std::string_view errorCode, std::string_view errorMessage,
    std::int64_t nowMs) {
  if (rowId <= 0 || nowMs < 0 ||
      (expectedState != ir::IrOutboxState::Pending &&
       expectedState != ir::IrOutboxState::AwaitingRemoteResult)) {
    return {.status = ir::IrOutboxMutationStatus::Invalid,
            .diagnostic = "IR configuration block is invalid"};
  }
  std::string storedCode = ir::sanitizeDiagnostic(errorCode);
  if (storedCode.size() > ir::kMaximumIrErrorCodeBytes) {
    storedCode.resize(ir::kMaximumIrErrorCodeBytes);
  }
  const std::string storedMessage = ir::sanitizeDiagnostic(errorMessage);
  profile_database_activity::WriteGuard operation;
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {.status = ir::IrOutboxMutationStatus::StorageFailure,
            .diagnostic = "replay storage is unavailable"};
  }
  SqliteStatementHandle statement;
  const char *query =
      "UPDATE ir_outbox SET state=3,consecutive_failure_count=0,"
      "next_attempt_at_ms=NULL,last_error_code=?,last_error_message=?,"
      "updated_at_ms=? WHERE id=? AND state=? AND local_result_ready=1";
  const std::optional<std::string> code =
      storedCode.empty() ? std::nullopt
                         : std::optional<std::string>(storedCode);
  const std::optional<std::string> message =
      storedMessage.empty() ? std::nullopt
                            : std::optional<std::string>(storedMessage);
  if (prepareSqliteStatement(impl_->sessionDatabase, query, statement) !=
          SQLITE_OK ||
      !bindOptionalText(statement.get(), 1, code) ||
      !bindOptionalText(statement.get(), 2, message) ||
      sqlite3_bind_int64(statement.get(), 3, nowMs) != SQLITE_OK ||
      sqlite3_bind_int64(statement.get(), 4, rowId) != SQLITE_OK ||
      sqlite3_bind_int(statement.get(), 5, static_cast<int>(expectedState)) !=
          SQLITE_OK ||
      sqlite3_step(statement.get()) != SQLITE_DONE) {
    return {.status = ir::IrOutboxMutationStatus::StorageFailure,
            .diagnostic = "could not block IR outbox configuration"};
  }
  const auto outcome = mutationFromChanges(impl_->sessionDatabase);
  if (outcome.status == ir::IrOutboxMutationStatus::Updated) {
    return outcome;
  }
  RowLookup current = loadById(impl_->sessionDatabase, rowId);
  return {.status = current.status == RowLookupStatus::NotFound
                        ? ir::IrOutboxMutationStatus::NotFound
                        : ir::IrOutboxMutationStatus::StateMismatch};
}

ir::IrOutboxMutationOutcome ReplayRepository::ApplyIrOutboxDelivery(
    const ir::IrOutboxDeliveryUpdate &update) {
  const bool hasJob = update.remoteJobId && !update.remoteJobId->empty();
  const bool hasOrigin = update.remoteOrigin && !update.remoteOrigin->empty();
  const bool remotePairValid =
      hasJob == hasOrigin &&
      (!hasJob ||
       (update.remoteJobId->size() <= ir::kMaximumIrRemoteValueBytes &&
        update.remoteOrigin->size() <= ir::kMaximumIrRemoteValueBytes));
  const bool targetValid =
      update.nextState == ir::IrOutboxState::Pending ||
      update.nextState == ir::IrOutboxState::AwaitingRemoteResult ||
      update.nextState == ir::IrOutboxState::BlockedConfiguration ||
      update.nextState == ir::IrOutboxState::FailedPermanent ||
      update.nextState == ir::IrOutboxState::Succeeded;
  const bool succeeds = update.nextState == ir::IrOutboxState::Succeeded;
  if (update.rowId <= 0 || update.updatedAtUnixMillis < 0 ||
      update.consecutiveFailureCount < 0 || update.remotePollCount < 0 ||
      (update.nextAttemptAtUnixMillis && *update.nextAttemptAtUnixMillis < 0) ||
      (update.completedAtUnixMillis && *update.completedAtUnixMillis < 0) ||
      !targetValid || !remotePairValid ||
      (update.nextState == ir::IrOutboxState::AwaitingRemoteResult &&
       !hasJob) ||
      (update.nextState != ir::IrOutboxState::AwaitingRemoteResult &&
       update.nextState != ir::IrOutboxState::BlockedConfiguration && hasJob) ||
      (succeeds != update.completedAtUnixMillis.has_value()) ||
      (succeeds != update.successfulReceipt.has_value())) {
    return {.status = ir::IrOutboxMutationStatus::Invalid,
            .diagnostic = "IR delivery update is invalid"};
  }
  if (update.successfulReceipt) {
    std::string diagnostic;
    if (!ir::validateIrSuccessfulReceiptDraft(*update.successfulReceipt,
                                              diagnostic)) {
      return {.status = ir::IrOutboxMutationStatus::Invalid,
              .diagnostic = std::move(diagnostic)};
    }
  }
  profile_database_activity::WriteGuard operation;
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {.status = ir::IrOutboxMutationStatus::StorageFailure,
            .diagnostic = "replay storage is unavailable"};
  }
  std::string errorCode = ir::sanitizeDiagnostic(update.lastErrorCode);
  if (errorCode.size() > ir::kMaximumIrErrorCodeBytes) {
    errorCode.resize(ir::kMaximumIrErrorCodeBytes);
  }
  const std::string errorMessage =
      ir::sanitizeDiagnostic(update.lastErrorMessage);
  SqliteStatementHandle stmt;
  const char *query =
      "UPDATE ir_outbox SET state=?,consecutive_failure_count=?,"
      "remote_poll_count=?,next_attempt_at_ms=?,remote_job_id=?,remote_origin=?,"
      "last_error_code=?,last_error_message=?,updated_at_ms=?,"
      "completed_at_ms=? WHERE id=? AND state=1";
  const std::optional<std::string> remoteJob =
      hasJob ? update.remoteJobId : std::nullopt;
  const std::optional<std::string> remoteOrigin =
      hasOrigin ? update.remoteOrigin : std::nullopt;
  const std::optional<std::string> storedErrorCode =
      errorCode.empty() ? std::nullopt : std::optional<std::string>(errorCode);
  const std::optional<std::string> storedErrorMessage =
      errorMessage.empty() ? std::nullopt
                           : std::optional<std::string>(errorMessage);
  std::string transactionError;
  SqliteTransactionHandle transaction(
      impl_->sessionDatabase, "BEGIN IMMEDIATE TRANSACTION", transactionError);
  if (!transaction.active()) {
    return {.status = ir::IrOutboxMutationStatus::StorageFailure,
            .diagnostic = "could not start IR delivery update"};
  }
  RowLookup claimed = loadById(impl_->sessionDatabase, update.rowId);
  if (claimed.status == RowLookupStatus::NotFound) {
    return {.status = ir::IrOutboxMutationStatus::NotFound};
  }
  if (claimed.status != RowLookupStatus::Found || !claimed.entry) {
    return {.status = ir::IrOutboxMutationStatus::StorageFailure,
            .diagnostic = claimed.diagnostic.empty()
                              ? "could not load claimed IR outbox row"
                              : std::move(claimed.diagnostic)};
  }
  if (claimed.entry->state != ir::IrOutboxState::Uploading) {
    return {.status = ir::IrOutboxMutationStatus::StateMismatch};
  }
  if (prepareSqliteStatement(impl_->sessionDatabase, query, stmt) !=
          SQLITE_OK ||
      sqlite3_bind_int(stmt.get(), 1, static_cast<int>(update.nextState)) !=
          SQLITE_OK ||
      sqlite3_bind_int(stmt.get(), 2, update.consecutiveFailureCount) !=
          SQLITE_OK ||
      sqlite3_bind_int(stmt.get(), 3, update.remotePollCount) != SQLITE_OK ||
      !bindOptionalInteger(stmt.get(), 4, update.nextAttemptAtUnixMillis) ||
      !bindOptionalText(stmt.get(), 5, remoteJob) ||
      !bindOptionalText(stmt.get(), 6, remoteOrigin) ||
      !bindOptionalText(stmt.get(), 7, storedErrorCode) ||
      !bindOptionalText(stmt.get(), 8, storedErrorMessage) ||
      sqlite3_bind_int64(stmt.get(), 9, update.updatedAtUnixMillis) !=
          SQLITE_OK ||
      !bindOptionalInteger(stmt.get(), 10, update.completedAtUnixMillis) ||
      sqlite3_bind_int64(stmt.get(), 11, update.rowId) != SQLITE_OK ||
      sqlite3_step(stmt.get()) != SQLITE_DONE) {
    return {.status = ir::IrOutboxMutationStatus::StorageFailure,
            .diagnostic = "could not apply IR outbox delivery"};
  }
  if (sqlite3_changes(impl_->sessionDatabase) != 1) {
    RowLookup current = loadById(impl_->sessionDatabase, update.rowId);
    return {.status = current.status == RowLookupStatus::NotFound
                          ? ir::IrOutboxMutationStatus::NotFound
                          : ir::IrOutboxMutationStatus::StateMismatch};
  }
  if (succeeds) {
    SqliteStatementHandle replay;
    if (prepareSqliteStatement(
            impl_->sessionDatabase,
            "SELECT id FROM replays WHERE attempt_id=?", replay) != SQLITE_OK ||
        !bindSqliteText(replay.get(), 1, claimed.entry->attemptId)) {
      return {.status = ir::IrOutboxMutationStatus::StorageFailure,
              .diagnostic = "could not prepare IR receipt replay lookup"};
    }
    const int replayStep = sqlite3_step(replay.get());
    if (replayStep != SQLITE_ROW ||
        sqlite3_column_type(replay.get(), 0) != SQLITE_INTEGER) {
      return {.status = ir::IrOutboxMutationStatus::StorageFailure,
              .diagnostic = replayStep == SQLITE_DONE
                                ? "IR receipt replay attempt is missing"
                                : "IR receipt replay lookup did not complete"};
    }
    const sqlite3_int64 replayId = sqlite3_column_int64(replay.get(), 0);
    if (replayId <= 0 || replayId > std::numeric_limits<int>::max() ||
        sqlite3_step(replay.get()) != SQLITE_DONE) {
      return {.status = ir::IrOutboxMutationStatus::StorageFailure,
              .diagnostic = "IR receipt replay lookup is invalid"};
    }

    const ir::IrSuccessfulReceiptDraft &receipt = *update.successfulReceipt;
    constexpr const char *receiptQuery =
        "INSERT INTO ir_submission_receipts("
        "provider_id,server_origin,replay_id,attempt_id,chart_md5,chart_sha256,"
        "remote_user_id,remote_chart_id,remote_score_id,confirmation_source,"
        "observed_in_snapshot,confirmed_at_ms) VALUES(?,?,?,?,?,?,?,?,?,?,?,?) "
        "ON CONFLICT(provider_id,server_origin,replay_id) DO UPDATE SET "
        "attempt_id=excluded.attempt_id,"
        "chart_md5=excluded.chart_md5,"
        "chart_sha256=excluded.chart_sha256,"
        "remote_user_id=COALESCE(excluded.remote_user_id,remote_user_id),"
        "remote_chart_id=CASE WHEN excluded.remote_chart_id<>'' THEN "
        "excluded.remote_chart_id ELSE remote_chart_id END,"
        "remote_score_id=CASE WHEN excluded.remote_score_id<>'' THEN "
        "excluded.remote_score_id ELSE remote_score_id END,"
        "confirmation_source=excluded.confirmation_source,"
        "observed_in_snapshot=MAX(observed_in_snapshot,"
        "excluded.observed_in_snapshot),"
        "confirmed_at_ms=MAX(confirmed_at_ms,excluded.confirmed_at_ms)";
    SqliteStatementHandle receiptStatement;
    if (prepareSqliteStatement(impl_->sessionDatabase, receiptQuery,
                               receiptStatement) != SQLITE_OK ||
        !bindSqliteText(receiptStatement.get(), 1,
                        claimed.entry->providerId) ||
        !bindSqliteText(receiptStatement.get(), 2, receipt.serverOrigin) ||
        sqlite3_bind_int64(receiptStatement.get(), 3, replayId) != SQLITE_OK ||
        !bindSqliteText(receiptStatement.get(), 4, claimed.entry->attemptId) ||
        (claimed.entry->chartMd5.empty()
             ? sqlite3_bind_null(receiptStatement.get(), 5) != SQLITE_OK
             : !bindSqliteText(receiptStatement.get(), 5,
                               claimed.entry->chartMd5)) ||
        !bindSqliteText(receiptStatement.get(), 6,
                        claimed.entry->chartSha256) ||
        (receipt.remoteUserId
             ? sqlite3_bind_int64(receiptStatement.get(), 7,
                                  *receipt.remoteUserId) != SQLITE_OK
             : sqlite3_bind_null(receiptStatement.get(), 7) != SQLITE_OK) ||
        !bindSqliteText(receiptStatement.get(), 8, receipt.remoteChartId) ||
        !bindSqliteText(receiptStatement.get(), 9, receipt.remoteScoreId) ||
        sqlite3_bind_int(receiptStatement.get(), 10,
                         static_cast<int>(receipt.source)) != SQLITE_OK ||
        sqlite3_bind_int(receiptStatement.get(), 11,
                         receipt.observedInSnapshot ? 1 : 0) != SQLITE_OK ||
        sqlite3_bind_int64(receiptStatement.get(), 12,
                           receipt.confirmedAtUnixMillis) != SQLITE_OK ||
        sqlite3_step(receiptStatement.get()) != SQLITE_DONE) {
      return {.status = ir::IrOutboxMutationStatus::StorageFailure,
              .diagnostic = "could not persist IR submission receipt"};
    }
  }
  if (!transaction.commit(transactionError)) {
    return {.status = ir::IrOutboxMutationStatus::StorageFailure,
            .diagnostic = "could not commit IR delivery update"};
  }
  return {.status = ir::IrOutboxMutationStatus::Updated, .affectedRows = 1};
}

ir::IrReceiptReadOutcome ReplayRepository::LoadIrSubmissionReceipt(
    std::string_view providerId, std::string_view serverOrigin,
    std::string_view attemptId) {
  const auto normalizedOrigin = ir::normalizeServerOrigin(serverOrigin);
  if (!validProviderId(providerId) ||
      !uuid::isCanonicalLowerV4(attemptId) || !normalizedOrigin ||
      *normalizedOrigin != serverOrigin) {
    return {.status = ir::IrReceiptReadStatus::Invalid,
            .diagnostic = "IR receipt identity is invalid"};
  }
  profile_database_activity::ReadGuard operation;
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {.status = ir::IrReceiptReadStatus::StorageFailure,
            .diagnostic = "replay storage is unavailable"};
  }
  const std::string query =
      std::string("SELECT ") + kIrSubmissionReceiptColumns +
      " FROM ir_submission_receipts WHERE provider_id=? AND "
      "server_origin=? AND attempt_id=?";
  SqliteStatementHandle statement;
  if (prepareSqliteStatement(impl_->sessionDatabase, query, statement) !=
          SQLITE_OK ||
      sqlite3_bind_text(statement.get(), 1, providerId.data(),
                        static_cast<int>(providerId.size()),
                        SQLITE_TRANSIENT) != SQLITE_OK ||
      sqlite3_bind_text(statement.get(), 2, serverOrigin.data(),
                        static_cast<int>(serverOrigin.size()),
                        SQLITE_TRANSIENT) != SQLITE_OK ||
      sqlite3_bind_text(statement.get(), 3, attemptId.data(),
                        static_cast<int>(attemptId.size()),
                        SQLITE_TRANSIENT) != SQLITE_OK) {
    return {.status = ir::IrReceiptReadStatus::StorageFailure,
            .diagnostic = "could not prepare IR receipt lookup"};
  }
  const int firstStep = sqlite3_step(statement.get());
  if (firstStep == SQLITE_DONE) {
    return {.status = ir::IrReceiptReadStatus::NotFound};
  }
  if (firstStep != SQLITE_ROW) {
    return {.status = ir::IrReceiptReadStatus::StorageFailure,
            .diagnostic = "IR receipt lookup did not complete"};
  }
  ir::IrSubmissionReceipt receipt;
  std::string diagnostic;
  if (!decodeIrSubmissionReceiptRow(statement.get(), receipt, diagnostic)) {
    return {.status = ir::IrReceiptReadStatus::Invalid,
            .diagnostic = std::move(diagnostic)};
  }
  if (sqlite3_step(statement.get()) != SQLITE_DONE) {
    return {.status = ir::IrReceiptReadStatus::StorageFailure,
            .diagnostic = "IR receipt lookup returned duplicate rows"};
  }
  return {.status = ir::IrReceiptReadStatus::Found,
          .receipt = std::move(receipt)};
}

ir::IrOutboxMutationOutcome ReplayRepository::ClearIrSubmissionReceipts(
    std::string_view providerId, std::string_view serverOrigin) {
  const auto normalizedOrigin = ir::normalizeServerOrigin(serverOrigin);
  if (!validProviderId(providerId) || !normalizedOrigin ||
      *normalizedOrigin != serverOrigin) {
    return {.status = ir::IrOutboxMutationStatus::Invalid,
            .diagnostic = "IR receipt identity is invalid"};
  }
  profile_database_activity::WriteGuard operation;
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {.status = ir::IrOutboxMutationStatus::StorageFailure,
            .diagnostic = "replay storage is unavailable"};
  }
  SqliteStatementHandle statement;
  if (prepareSqliteStatement(
          impl_->sessionDatabase,
          "DELETE FROM ir_submission_receipts WHERE provider_id=? AND "
          "server_origin=?",
          statement) != SQLITE_OK ||
      sqlite3_bind_text(statement.get(), 1, providerId.data(),
                        static_cast<int>(providerId.size()),
                        SQLITE_TRANSIENT) != SQLITE_OK ||
      sqlite3_bind_text(statement.get(), 2, serverOrigin.data(),
                        static_cast<int>(serverOrigin.size()),
                        SQLITE_TRANSIENT) != SQLITE_OK ||
      sqlite3_step(statement.get()) != SQLITE_DONE) {
    return {.status = ir::IrOutboxMutationStatus::StorageFailure,
            .diagnostic = "could not clear IR submission receipts"};
  }
  return mutationFromChanges(impl_->sessionDatabase);
}

ir::IrOutboxMutationOutcome
ReplayRepository::RetryIrOutbox(std::int64_t rowId, std::int64_t nowMs) {
  if (rowId <= 0 || nowMs < 0) {
    return {.status = ir::IrOutboxMutationStatus::Invalid};
  }
  profile_database_activity::WriteGuard operation;
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {.status = ir::IrOutboxMutationStatus::StorageFailure};
  }
  SqliteStatementHandle stmt;
  const char *query =
      "UPDATE ir_outbox SET "
      "state=CASE WHEN state=2 OR (state=3 AND remote_job_id IS NOT NULL) "
      "THEN 2 ELSE 0 END,consecutive_failure_count=0,next_attempt_at_ms=?,"
      "next_request_user_intent=CASE WHEN state=2 OR (state=3 AND "
      "remote_job_id IS NOT NULL) THEN 0 ELSE 1 END,"
      "remote_job_id=CASE WHEN state=2 OR (state=3 AND remote_job_id IS NOT "
      "NULL) THEN remote_job_id ELSE NULL END,"
      "remote_origin=CASE WHEN state=2 OR (state=3 AND remote_job_id IS NOT "
      "NULL) THEN remote_origin ELSE NULL END,last_error_code=NULL,"
      "last_error_message=NULL,updated_at_ms=?,completed_at_ms=NULL "
      "WHERE id=? AND state IN (0,2,3,4) AND "
      "ruleset_id<>'legacy-unknown' AND validation_fingerprint<>''";
  if (prepareSqliteStatement(impl_->sessionDatabase, query, stmt) !=
          SQLITE_OK ||
      sqlite3_bind_int64(stmt.get(), 1, nowMs) != SQLITE_OK ||
      sqlite3_bind_int64(stmt.get(), 2, nowMs) != SQLITE_OK ||
      sqlite3_bind_int64(stmt.get(), 3, rowId) != SQLITE_OK ||
      sqlite3_step(stmt.get()) != SQLITE_DONE) {
    return {.status = ir::IrOutboxMutationStatus::StorageFailure};
  }
  const auto outcome = mutationFromChanges(impl_->sessionDatabase);
  if (outcome.status == ir::IrOutboxMutationStatus::Updated) {
    return outcome;
  }
  RowLookup current = loadById(impl_->sessionDatabase, rowId);
  if (current.status == RowLookupStatus::Found && current.entry &&
      (current.entry->rulesetProof.rulesetId == "legacy-unknown" ||
       current.entry->rulesetProof.validationFingerprint.empty())) {
    return {.status = ir::IrOutboxMutationStatus::Invalid,
            .diagnostic = "legacy IR outbox rows cannot be retried"};
  }
  return {.status = current.status == RowLookupStatus::NotFound
                        ? ir::IrOutboxMutationStatus::NotFound
                        : ir::IrOutboxMutationStatus::StateMismatch};
}

ir::IrOutboxMutationOutcome
ReplayRepository::RetryAllIrOutbox(std::string_view providerId,
                                   std::int64_t nowMs) {
  if (!validProviderId(providerId) || nowMs < 0) {
    return {.status = ir::IrOutboxMutationStatus::Invalid};
  }
  profile_database_activity::WriteGuard operation;
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {.status = ir::IrOutboxMutationStatus::StorageFailure};
  }
  SqliteStatementHandle stmt;
  const char *query =
      "UPDATE ir_outbox SET "
      "state=CASE WHEN state=2 OR (state=3 AND remote_job_id IS NOT NULL) "
      "THEN 2 ELSE 0 END,consecutive_failure_count=0,next_attempt_at_ms=?,"
      "next_request_user_intent=CASE WHEN state=2 OR (state=3 AND "
      "remote_job_id IS NOT NULL) THEN 0 ELSE 1 END,"
      "remote_job_id=CASE WHEN state=2 OR (state=3 AND remote_job_id IS NOT "
      "NULL) THEN remote_job_id ELSE NULL END,"
      "remote_origin=CASE WHEN state=2 OR (state=3 AND remote_job_id IS NOT "
      "NULL) THEN remote_origin ELSE NULL END,last_error_code=NULL,"
      "last_error_message=NULL,updated_at_ms=?,completed_at_ms=NULL WHERE "
      "provider_id=? AND local_result_ready=1 AND state IN (0,2,3,4) AND "
      "ruleset_id<>'legacy-unknown' AND validation_fingerprint<>''";
  if (prepareSqliteStatement(impl_->sessionDatabase, query, stmt) !=
          SQLITE_OK ||
      sqlite3_bind_int64(stmt.get(), 1, nowMs) != SQLITE_OK ||
      sqlite3_bind_int64(stmt.get(), 2, nowMs) != SQLITE_OK ||
      sqlite3_bind_text(stmt.get(), 3, providerId.data(),
                        static_cast<int>(providerId.size()),
                        SQLITE_TRANSIENT) != SQLITE_OK ||
      sqlite3_step(stmt.get()) != SQLITE_DONE) {
    return {.status = ir::IrOutboxMutationStatus::StorageFailure};
  }
  return mutationFromChanges(impl_->sessionDatabase);
}

ir::IrOutboxMutationOutcome
ReplayRepository::UnblockIrOutbox(std::string_view providerId,
                                  std::int64_t nowMs) {
  if (!validProviderId(providerId) || nowMs < 0) {
    return {.status = ir::IrOutboxMutationStatus::Invalid};
  }
  profile_database_activity::WriteGuard operation;
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {.status = ir::IrOutboxMutationStatus::StorageFailure};
  }
  SqliteStatementHandle stmt;
  const char *query =
      "UPDATE ir_outbox SET state=CASE WHEN remote_job_id IS NULL THEN 0 ELSE "
      "2 END,consecutive_failure_count=0,"
      "next_attempt_at_ms=?,last_error_code=NULL,last_error_message=NULL,"
      "updated_at_ms=? WHERE provider_id=? AND state=3 AND "
      "ruleset_id<>'legacy-unknown' AND validation_fingerprint<>''";
  if (prepareSqliteStatement(impl_->sessionDatabase, query, stmt) !=
          SQLITE_OK ||
      sqlite3_bind_int64(stmt.get(), 1, nowMs) != SQLITE_OK ||
      sqlite3_bind_int64(stmt.get(), 2, nowMs) != SQLITE_OK ||
      sqlite3_bind_text(stmt.get(), 3, providerId.data(),
                        static_cast<int>(providerId.size()),
                        SQLITE_TRANSIENT) != SQLITE_OK ||
      sqlite3_step(stmt.get()) != SQLITE_DONE) {
    return {.status = ir::IrOutboxMutationStatus::StorageFailure};
  }
  return mutationFromChanges(impl_->sessionDatabase);
}

ir::IrOutboxMutationOutcome
ReplayRepository::DiscardIrOutbox(std::int64_t rowId) {
  if (rowId <= 0) {
    return {.status = ir::IrOutboxMutationStatus::Invalid};
  }
  profile_database_activity::WriteGuard operation;
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {.status = ir::IrOutboxMutationStatus::StorageFailure};
  }
  SqliteStatementHandle stmt;
  if (prepareSqliteStatement(impl_->sessionDatabase,
                             "DELETE FROM ir_outbox WHERE id=?",
                             stmt) != SQLITE_OK ||
      sqlite3_bind_int64(stmt.get(), 1, rowId) != SQLITE_OK ||
      sqlite3_step(stmt.get()) != SQLITE_DONE) {
    return {.status = ir::IrOutboxMutationStatus::StorageFailure};
  }
  return mutationFromChanges(impl_->sessionDatabase);
}

ir::IrOutboxCounts
ReplayRepository::CountIrOutbox(std::string_view providerId) {
  if (!validProviderId(providerId)) {
    return {.diagnostic = "IR provider ID is invalid"};
  }
  profile_database_activity::ReadGuard operation;
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {.diagnostic = "replay storage is unavailable"};
  }
  SqliteStatementHandle stmt;
  if (prepareSqliteStatement(
          impl_->sessionDatabase,
          "SELECT state,COUNT(*) FROM ir_outbox WHERE provider_id=? AND "
          "local_result_ready=1 GROUP BY state",
          stmt) != SQLITE_OK ||
      sqlite3_bind_text(stmt.get(), 1, providerId.data(),
                        static_cast<int>(providerId.size()),
                        SQLITE_TRANSIENT) != SQLITE_OK) {
    return {.diagnostic = "could not count IR outbox rows"};
  }
  ir::IrOutboxCounts result{.storageAvailable = true};
  int rc = SQLITE_OK;
  while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
    if (!columnIs(stmt.get(), 0, SQLITE_INTEGER) ||
        !columnIs(stmt.get(), 1, SQLITE_INTEGER) ||
        sqlite3_column_int64(stmt.get(), 1) < 0 ||
        !ir::isKnownIrOutboxState(sqlite3_column_int(stmt.get(), 0))) {
      return {.diagnostic = "IR outbox count contains invalid state"};
    }
    const std::size_t count =
        static_cast<std::size_t>(sqlite3_column_int64(stmt.get(), 1));
    switch (static_cast<ir::IrOutboxState>(sqlite3_column_int(stmt.get(), 0))) {
    case ir::IrOutboxState::Pending:
      result.pending = count;
      break;
    case ir::IrOutboxState::Uploading:
      result.uploading = count;
      break;
    case ir::IrOutboxState::AwaitingRemoteResult:
      result.awaitingRemoteResult = count;
      break;
    case ir::IrOutboxState::BlockedConfiguration:
      result.blockedConfiguration = count;
      break;
    case ir::IrOutboxState::FailedPermanent:
      result.failedPermanent = count;
      break;
    case ir::IrOutboxState::Succeeded:
      result.succeeded = count;
      break;
    }
    result.total += count;
  }
  if (rc != SQLITE_DONE) {
    return {.diagnostic = "IR outbox count did not complete"};
  }
  return result;
}

ir::IrOutboxMutationOutcome
ReplayRepository::RecoverStaleIrOutbox(std::int64_t nowMs) {
  if (nowMs < 0) {
    return {.status = ir::IrOutboxMutationStatus::Invalid};
  }
  profile_database_activity::WriteGuard operation;
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {.status = ir::IrOutboxMutationStatus::StorageFailure};
  }
  std::string transactionError;
  SqliteTransactionHandle transaction(
      impl_->sessionDatabase, "BEGIN IMMEDIATE TRANSACTION", transactionError);
  if (!transaction.active()) {
    return {.status = ir::IrOutboxMutationStatus::StorageFailure};
  }
  SqliteStatementHandle deferred;
  if (prepareSqliteStatement(
          impl_->sessionDatabase,
          "UPDATE ir_outbox SET state=2,updated_at_ms=? WHERE state=1 AND "
          "remote_job_id IS NOT NULL AND remote_origin IS NOT NULL",
          deferred) != SQLITE_OK ||
      sqlite3_bind_int64(deferred.get(), 1, nowMs) != SQLITE_OK ||
      sqlite3_step(deferred.get()) != SQLITE_DONE) {
    return {.status = ir::IrOutboxMutationStatus::StorageFailure};
  }
  const int deferredChanges = sqlite3_changes(impl_->sessionDatabase);
  SqliteStatementHandle pending;
  if (prepareSqliteStatement(
          impl_->sessionDatabase,
          "UPDATE ir_outbox SET state=0,updated_at_ms=? WHERE state=1 AND "
          "remote_job_id IS NULL AND remote_origin IS NULL",
          pending) != SQLITE_OK ||
      sqlite3_bind_int64(pending.get(), 1, nowMs) != SQLITE_OK ||
      sqlite3_step(pending.get()) != SQLITE_DONE) {
    return {.status = ir::IrOutboxMutationStatus::StorageFailure};
  }
  const int pendingChanges = sqlite3_changes(impl_->sessionDatabase);
  if (!transaction.commit(transactionError)) {
    return {.status = ir::IrOutboxMutationStatus::StorageFailure};
  }
  const std::size_t changes = static_cast<std::size_t>(
      std::max(0, deferredChanges) + std::max(0, pendingChanges));
  return {.status = changes > 0 ? ir::IrOutboxMutationStatus::Updated
                                : ir::IrOutboxMutationStatus::NotFound,
          .affectedRows = changes};
}

ir::IrOutboxMutationOutcome
ReplayRepository::PurgeSucceededIrOutbox(std::int64_t olderThanMs) {
  if (olderThanMs < 0) {
    return {.status = ir::IrOutboxMutationStatus::Invalid};
  }
  profile_database_activity::WriteGuard operation;
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {.status = ir::IrOutboxMutationStatus::StorageFailure};
  }
  SqliteStatementHandle stmt;
  if (prepareSqliteStatement(
          impl_->sessionDatabase,
          "DELETE FROM ir_outbox WHERE state=5 AND completed_at_ms<?",
          stmt) != SQLITE_OK ||
      sqlite3_bind_int64(stmt.get(), 1, olderThanMs) != SQLITE_OK ||
      sqlite3_step(stmt.get()) != SQLITE_DONE) {
    return {.status = ir::IrOutboxMutationStatus::StorageFailure};
  }
  return mutationFromChanges(impl_->sessionDatabase);
}

bool ReplayRepository::ClearIrOutbox(std::string &errorMessage) {
  profile_database_activity::WriteGuard operation;
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    errorMessage = "replay storage is unavailable";
    return false;
  }
  if (sqlite3_exec(impl_->sessionDatabase, "DELETE FROM ir_outbox", nullptr,
                   nullptr, nullptr) != SQLITE_OK) {
    errorMessage = "could not clear IR outbox";
    return false;
  }
  errorMessage.clear();
  return true;
}

bool ReplayRepository::ClearIrOutboxSnapshot(
    const std::filesystem::path &snapshotDatabasePath,
    std::string &errorMessage) {
  if (snapshotDatabasePath.empty()) {
    errorMessage = "IR outbox snapshot path is empty";
    return false;
  }
  std::error_code filesystemError;
  const auto status =
      std::filesystem::symlink_status(snapshotDatabasePath, filesystemError);
  if (filesystemError || !std::filesystem::is_regular_file(status) ||
      std::filesystem::is_symlink(status)) {
    errorMessage = "IR outbox snapshot is not a regular database file";
    return false;
  }
  ReplayRepository snapshot(snapshotDatabasePath);
  if (!snapshot.EnsureSchema()) {
    errorMessage = "IR outbox snapshot schema is unavailable";
    return false;
  }

  profile_database_activity::WriteGuard operation;
  std::lock_guard lock(snapshot.impl_->sessionMutex);
  if (!snapshot.EnsureSessionDatabaseLocked()) {
    errorMessage = "IR outbox snapshot storage is unavailable";
    return false;
  }
  std::string transactionError;
  SqliteTransactionHandle transaction(snapshot.impl_->sessionDatabase,
                                      "BEGIN IMMEDIATE TRANSACTION",
                                      transactionError);
  if (!transaction.active()) {
    errorMessage = "could not start IR outbox snapshot cleanup";
    return false;
  }
  SqliteStatementHandle clear;
  if (prepareSqliteStatement(snapshot.impl_->sessionDatabase,
                             "DELETE FROM ir_outbox", clear) != SQLITE_OK ||
      sqlite3_step(clear.get()) != SQLITE_DONE) {
    errorMessage = "could not clear IR outbox snapshot";
    return false;
  }
  SqliteStatementHandle verify;
  if (prepareSqliteStatement(snapshot.impl_->sessionDatabase,
                             "SELECT COUNT(*) FROM ir_outbox",
                             verify) != SQLITE_OK ||
      sqlite3_step(verify.get()) != SQLITE_ROW ||
      sqlite3_column_type(verify.get(), 0) != SQLITE_INTEGER ||
      sqlite3_column_int64(verify.get(), 0) != 0 ||
      sqlite3_step(verify.get()) != SQLITE_DONE) {
    errorMessage = "IR outbox snapshot cleanup could not be verified";
    return false;
  }
  if (!transaction.commit(transactionError)) {
    errorMessage = "could not commit IR outbox snapshot cleanup";
    return false;
  }
  clear.reset();
  verify.reset();
  int walFrames = 0;
  int checkpointedFrames = 0;
  const int checkpointResult = sqlite3_wal_checkpoint_v2(
      snapshot.impl_->sessionDatabase, "main", SQLITE_CHECKPOINT_TRUNCATE,
      &walFrames, &checkpointedFrames);
  if (checkpointResult != SQLITE_OK ||
      (walFrames >= 0 && checkpointedFrames != walFrames)) {
    errorMessage = "could not checkpoint the cleaned IR outbox snapshot";
    return false;
  }
  errorMessage.clear();
  return true;
}
