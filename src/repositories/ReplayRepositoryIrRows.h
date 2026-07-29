#pragma once

#include "../ir/IrOutboxModels.h"
#include "SqliteRAII.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>

namespace replay_repository_detail {

inline constexpr const char *kIrOutboxColumns =
    "id,provider_id,attempt_id,chart_md5,chart_sha256,payload_json,"
    "ruleset_id,ruleset_revision,validation_fingerprint,state,"
    "local_result_ready,request_attempt_count,consecutive_failure_count,"
    "remote_poll_count,next_attempt_at_ms,next_request_user_intent,"
    "remote_job_id,remote_origin,last_error_code,last_error_message,"
    "created_at_ms,updated_at_ms,completed_at_ms";

inline constexpr const char *kIrSubmissionReceiptColumns =
    "id,provider_id,server_origin,replay_id,modern_chart_result_id,attempt_id,"
    "chart_md5,chart_sha256,remote_user_id,remote_chart_id,remote_score_id,"
    "confirmation_source,observed_in_snapshot,confirmed_at_ms";

inline bool columnIs(sqlite3_stmt *statement, int column, int type) {
  return sqlite3_column_type(statement, column) == type;
}

inline bool nullableInteger(sqlite3_stmt *statement, int column) {
  const int type = sqlite3_column_type(statement, column);
  return type == SQLITE_NULL || type == SQLITE_INTEGER;
}

inline bool nullableText(sqlite3_stmt *statement, int column) {
  const int type = sqlite3_column_type(statement, column);
  return type == SQLITE_NULL || type == SQLITE_TEXT;
}

inline std::optional<std::int64_t> optionalInteger(sqlite3_stmt *statement,
                                                   int column) {
  if (sqlite3_column_type(statement, column) == SQLITE_NULL) {
    return std::nullopt;
  }
  return sqlite3_column_int64(statement, column);
}

inline std::string optionalText(sqlite3_stmt *statement, int column) {
  return sqlite3_column_type(statement, column) == SQLITE_NULL
             ? std::string{}
             : sqliteColumnString(statement, column);
}

inline bool decodeIrOutboxRow(sqlite3_stmt *statement,
                              ir::IrOutboxEntry &entry,
                              std::string &diagnostic) {
  if (!columnIs(statement, 0, SQLITE_INTEGER) ||
      !columnIs(statement, 1, SQLITE_TEXT) ||
      !columnIs(statement, 2, SQLITE_TEXT) || !nullableText(statement, 3) ||
      !columnIs(statement, 4, SQLITE_TEXT) ||
      !columnIs(statement, 5, SQLITE_TEXT) ||
      !columnIs(statement, 6, SQLITE_TEXT) ||
      !columnIs(statement, 7, SQLITE_INTEGER) ||
      !columnIs(statement, 8, SQLITE_TEXT) ||
      !columnIs(statement, 9, SQLITE_INTEGER) ||
      !columnIs(statement, 10, SQLITE_INTEGER) ||
      !columnIs(statement, 11, SQLITE_INTEGER) ||
      !columnIs(statement, 12, SQLITE_INTEGER) ||
      !columnIs(statement, 13, SQLITE_INTEGER) ||
      !nullableInteger(statement, 14) ||
      !columnIs(statement, 15, SQLITE_INTEGER) ||
      !nullableText(statement, 16) || !nullableText(statement, 17) ||
      !nullableText(statement, 18) || !nullableText(statement, 19) ||
      !columnIs(statement, 20, SQLITE_INTEGER) ||
      !columnIs(statement, 21, SQLITE_INTEGER) ||
      !nullableInteger(statement, 22)) {
    diagnostic = "IR outbox row has unexpected SQLite value types";
    return false;
  }
  const int state = sqlite3_column_int(statement, 9);
  const int localReady = sqlite3_column_int(statement, 10);
  const int userIntent = sqlite3_column_int(statement, 15);
  if (!ir::isKnownIrOutboxState(state) ||
      (localReady != 0 && localReady != 1) ||
      (userIntent != 0 && userIntent != 1)) {
    diagnostic = "IR outbox row contains an unknown state or boolean";
    return false;
  }
  entry = {
      .id = sqlite3_column_int64(statement, 0),
      .providerId = sqliteColumnString(statement, 1),
      .attemptId = sqliteColumnString(statement, 2),
      .chartMd5 = optionalText(statement, 3),
      .chartSha256 = sqliteColumnString(statement, 4),
      .payloadJson = sqliteColumnString(statement, 5),
      .rulesetProof =
          {
              .rulesetId = sqliteColumnString(statement, 6),
              .rulesetRevision = sqlite3_column_int(statement, 7),
              .validationFingerprint = sqliteColumnString(statement, 8),
          },
      .state = static_cast<ir::IrOutboxState>(state),
      .localResultReady = localReady != 0,
      .requestAttemptCount = sqlite3_column_int(statement, 11),
      .consecutiveFailureCount = sqlite3_column_int(statement, 12),
      .remotePollCount = sqlite3_column_int(statement, 13),
      .nextAttemptAtUnixMillis = optionalInteger(statement, 14),
      .nextRequestUserIntent = userIntent != 0,
      .remoteJobId = optionalText(statement, 16),
      .remoteOrigin = optionalText(statement, 17),
      .lastErrorCode = optionalText(statement, 18),
      .lastErrorMessage = optionalText(statement, 19),
      .createdAtUnixMillis = sqlite3_column_int64(statement, 20),
      .updatedAtUnixMillis = sqlite3_column_int64(statement, 21),
      .completedAtUnixMillis = optionalInteger(statement, 22),
  };
  return ir::validateIrOutboxEntry(entry, diagnostic);
}

inline bool decodeIrSubmissionReceiptRow(
    sqlite3_stmt *statement, ir::IrSubmissionReceipt &receipt,
    std::string &diagnostic) {
  if (!columnIs(statement, 0, SQLITE_INTEGER) ||
      !columnIs(statement, 1, SQLITE_TEXT) ||
      !columnIs(statement, 2, SQLITE_TEXT) ||
      !nullableInteger(statement, 3) ||
      !nullableInteger(statement, 4) ||
      !columnIs(statement, 5, SQLITE_TEXT) || !nullableText(statement, 6) ||
      !columnIs(statement, 7, SQLITE_TEXT) ||
      !nullableInteger(statement, 8) || !nullableText(statement, 9) ||
      !nullableText(statement, 10) ||
      !columnIs(statement, 11, SQLITE_INTEGER) ||
      !columnIs(statement, 12, SQLITE_INTEGER) ||
      !columnIs(statement, 13, SQLITE_INTEGER)) {
    diagnostic = "IR receipt row has unexpected SQLite value types";
    return false;
  }
  const sqlite3_int64 replayId =
      sqlite3_column_type(statement, 3) == SQLITE_NULL
          ? 0
          : sqlite3_column_int64(statement, 3);
  const sqlite3_int64 modernResultId =
      sqlite3_column_type(statement, 4) == SQLITE_NULL
          ? 0
          : sqlite3_column_int64(statement, 4);
  if (replayId < 0 || replayId > std::numeric_limits<int>::max() ||
      modernResultId < 0 ||
      modernResultId > std::numeric_limits<int>::max()) {
    diagnostic = "IR receipt result owner ID is out of range";
    return false;
  }
  receipt = {
      .id = sqlite3_column_int64(statement, 0),
      .providerId = sqliteColumnString(statement, 1),
      .serverOrigin = sqliteColumnString(statement, 2),
      .replayId = static_cast<int>(replayId),
      .modernChartResultId = static_cast<int>(modernResultId),
      .attemptId = sqliteColumnString(statement, 5),
      .chartMd5 = optionalText(statement, 6),
      .chartSha256 = sqliteColumnString(statement, 7),
      .remoteUserId = optionalInteger(statement, 8),
      .remoteChartId = optionalText(statement, 9),
      .remoteScoreId = optionalText(statement, 10),
      .source = static_cast<ir::IrReceiptConfirmationSource>(
          sqlite3_column_int(statement, 11)),
      .observedInSnapshot = sqlite3_column_int(statement, 12) != 0,
      .confirmedAtUnixMillis = sqlite3_column_int64(statement, 13),
  };
  return ir::validateIrSubmissionReceipt(receipt, diagnostic);
}

} // namespace replay_repository_detail
