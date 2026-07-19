#include "ReplayRepository.h"
#include "ReplayRepositoryInternal.h"

#include "../ProfileDatabaseActivity.h"
#include "../ir/IrProfileSettings.h"
#include <nlohmann/json.hpp>
#include "SqliteRAII.h"

#include <SDL2/SDL.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using Json = nlohmann::json;

constexpr std::size_t kMaximumGaugeHistoryJsonBytes = 4 * 1024 * 1024;

constexpr const char *kIrRemoteScoreColumns =
    "remote_user_id,game,remote_score_id,remote_chart_id,chart_md5,"
    "chart_sha256,title,artist,difficulty,level,level_number,note_count,score,"
    "lamp_rank,service,time_achieved_ms,time_added_ms,pgreat,great,good,bad,"
    "poor,early_pgreat,late_pgreat,early_great,late_great,early_good,"
    "late_good,early_bad,late_bad,early_poor,late_poor,fast,slow,max_combo,"
    "bad_points,final_gauge,gauge_history_json,random_mode,gauge_mode,"
    "input_device,client,sync_generation";

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

bool validOriginIdentity(std::string_view providerId,
                         std::string_view serverOrigin) {
  const auto normalizedOrigin = ir::normalizeServerOrigin(serverOrigin);
  return validProviderId(providerId) && normalizedOrigin &&
         *normalizedOrigin == serverOrigin;
}

bool columnIs(sqlite3_stmt *stmt, int column, int type) {
  return sqlite3_column_type(stmt, column) == type;
}

bool nullableInteger(sqlite3_stmt *stmt, int column) {
  const int type = sqlite3_column_type(stmt, column);
  return type == SQLITE_NULL || type == SQLITE_INTEGER;
}

bool nullableNumber(sqlite3_stmt *stmt, int column) {
  const int type = sqlite3_column_type(stmt, column);
  return type == SQLITE_NULL || type == SQLITE_INTEGER || type == SQLITE_FLOAT;
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

bool optionalInt(sqlite3_stmt *stmt, int column, std::optional<int> &value) {
  if (sqlite3_column_type(stmt, column) == SQLITE_NULL) {
    value.reset();
    return true;
  }
  const sqlite3_int64 stored = sqlite3_column_int64(stmt, column);
  if (stored < std::numeric_limits<int>::min() ||
      stored > std::numeric_limits<int>::max()) {
    return false;
  }
  value = static_cast<int>(stored);
  return true;
}

std::optional<std::string> optionalText(sqlite3_stmt *stmt, int column) {
  if (sqlite3_column_type(stmt, column) == SQLITE_NULL) {
    return std::nullopt;
  }
  return sqliteColumnString(stmt, column);
}

bool bindOptionalInteger(sqlite3_stmt *stmt, int column,
                         const std::optional<std::int64_t> &value) {
  return value ? sqlite3_bind_int64(stmt, column, *value) == SQLITE_OK
               : sqlite3_bind_null(stmt, column) == SQLITE_OK;
}

bool bindOptionalInt(sqlite3_stmt *stmt, int column,
                     const std::optional<int> &value) {
  return value ? sqlite3_bind_int(stmt, column, *value) == SQLITE_OK
               : sqlite3_bind_null(stmt, column) == SQLITE_OK;
}

bool bindOptionalFloat(sqlite3_stmt *stmt, int column,
                       const std::optional<float> &value) {
  return value ? sqlite3_bind_double(stmt, column, *value) == SQLITE_OK
               : sqlite3_bind_null(stmt, column) == SQLITE_OK;
}

bool bindOptionalDouble(sqlite3_stmt *stmt, int column,
                        const std::optional<double> &value) {
  return value ? sqlite3_bind_double(stmt, column, *value) == SQLITE_OK
               : sqlite3_bind_null(stmt, column) == SQLITE_OK;
}

bool bindOptionalText(sqlite3_stmt *stmt, int column,
                      const std::optional<std::string> &value) {
  return value ? bindSqliteText(stmt, column, *value)
               : sqlite3_bind_null(stmt, column) == SQLITE_OK;
}

struct SerializedGaugeHistory {
  bool valid = false;
  std::optional<std::string> json;
};

SerializedGaugeHistory
serializeGaugeHistory(const std::vector<std::optional<float>> &history) {
  try {
    if (history.empty()) {
      return {.valid = true};
    }
    Json document = Json::array();
    for (const auto &point : history) {
      if (point) {
        document.push_back(*point);
      } else {
        document.push_back(nullptr);
      }
    }
    std::string encoded = document.dump();
    if (encoded.size() > kMaximumGaugeHistoryJsonBytes) {
      return {};
    }
    return {.valid = true, .json = std::move(encoded)};
  } catch (...) {
    return {};
  }
}

bool decodeGaugeHistory(std::string_view encoded,
                        std::vector<std::optional<float>> &history) {
  try {
    if (encoded.empty() || encoded.size() > kMaximumGaugeHistoryJsonBytes) {
      return false;
    }
    Json document = Json::parse(encoded, nullptr, false);
    if (document.is_discarded() || !document.is_array() || document.empty() ||
        document.size() > ir::kMaximumIrRemoteGaugeHistoryEntries) {
      return false;
    }
    std::vector<std::optional<float>> decoded;
    decoded.reserve(document.size());
    for (const Json &point : document) {
      if (point.is_null()) {
        decoded.push_back(std::nullopt);
        continue;
      }
      if (!point.is_number()) {
        return false;
      }
      const double numeric = point.get<double>();
      const float value = static_cast<float>(numeric);
      if (!std::isfinite(numeric) || !std::isfinite(value) || value < 0.0f ||
          value > 100.0f) {
        return false;
      }
      decoded.push_back(value);
    }
    const auto canonical = serializeGaugeHistory(decoded);
    if (!canonical.valid || !canonical.json || *canonical.json != encoded) {
      return false;
    }
    history = std::move(decoded);
    return true;
  } catch (...) {
    return false;
  }
}

bool decodeRemoteScoreRow(sqlite3_stmt *stmt, ir::IrRemoteScore &score,
                          std::int64_t &generation, std::string &diagnostic) {
  if (!columnIs(stmt, 0, SQLITE_INTEGER) || !columnIs(stmt, 1, SQLITE_TEXT) ||
      !columnIs(stmt, 2, SQLITE_TEXT) || !columnIs(stmt, 3, SQLITE_TEXT) ||
      !columnIs(stmt, 4, SQLITE_TEXT) || !columnIs(stmt, 5, SQLITE_TEXT) ||
      !columnIs(stmt, 6, SQLITE_TEXT) || !columnIs(stmt, 7, SQLITE_TEXT) ||
      !nullableText(stmt, 8) || !nullableText(stmt, 9) ||
      !nullableNumber(stmt, 10) || !columnIs(stmt, 11, SQLITE_INTEGER) ||
      !columnIs(stmt, 12, SQLITE_INTEGER) ||
      !columnIs(stmt, 13, SQLITE_INTEGER) || !columnIs(stmt, 14, SQLITE_TEXT) ||
      !nullableInteger(stmt, 15) || !columnIs(stmt, 16, SQLITE_INTEGER)) {
    diagnostic = "IR remote score row has unexpected SQLite value types";
    return false;
  }
  for (int column = 17; column <= 35; ++column) {
    if (!nullableInteger(stmt, column)) {
      diagnostic = "IR remote score row has unexpected metric value types";
      return false;
    }
  }
  if (!nullableNumber(stmt, 36) || !nullableText(stmt, 37) ||
      !nullableText(stmt, 38) || !nullableText(stmt, 39) ||
      !nullableText(stmt, 40) || !nullableText(stmt, 41) ||
      !columnIs(stmt, 42, SQLITE_INTEGER)) {
    diagnostic = "IR remote score row has unexpected optional value types";
    return false;
  }

  const sqlite3_int64 noteCount = sqlite3_column_int64(stmt, 11);
  const sqlite3_int64 exScore = sqlite3_column_int64(stmt, 12);
  const sqlite3_int64 lampRank = sqlite3_column_int64(stmt, 13);
  if (noteCount < std::numeric_limits<int>::min() ||
      noteCount > std::numeric_limits<int>::max() ||
      exScore < std::numeric_limits<int>::min() ||
      exScore > std::numeric_limits<int>::max() ||
      lampRank < std::numeric_limits<int>::min() ||
      lampRank > std::numeric_limits<int>::max()) {
    diagnostic = "IR remote score row contains an out-of-range integer";
    return false;
  }

  score = {
      .remoteUserId = sqlite3_column_int64(stmt, 0),
      .game = sqliteColumnString(stmt, 1),
      .remoteScoreId = sqliteColumnString(stmt, 2),
      .remoteChartId = sqliteColumnString(stmt, 3),
      .chartMd5 = sqliteColumnString(stmt, 4),
      .chartSha256 = sqliteColumnString(stmt, 5),
      .title = sqliteColumnString(stmt, 6),
      .artist = sqliteColumnString(stmt, 7),
      .service = sqliteColumnString(stmt, 14),
      .difficulty = optionalText(stmt, 8),
      .level = optionalText(stmt, 9),
      .levelNumber = sqlite3_column_type(stmt, 10) == SQLITE_NULL
                         ? std::optional<double>{}
                         : sqlite3_column_double(stmt, 10),
      .noteCount = static_cast<int>(noteCount),
      .score = static_cast<int>(exScore),
      .lampRank = static_cast<int>(lampRank),
      .timeAchievedUnixMillis = optionalInteger(stmt, 15),
      .timeAddedUnixMillis = sqlite3_column_int64(stmt, 16),
  };
  if (!optionalInt(stmt, 17, score.judgements.pGreat) ||
      !optionalInt(stmt, 18, score.judgements.great) ||
      !optionalInt(stmt, 19, score.judgements.good) ||
      !optionalInt(stmt, 20, score.judgements.bad) ||
      !optionalInt(stmt, 21, score.judgements.poor) ||
      !optionalInt(stmt, 22, score.timing.earlyPGreat) ||
      !optionalInt(stmt, 23, score.timing.latePGreat) ||
      !optionalInt(stmt, 24, score.timing.earlyGreat) ||
      !optionalInt(stmt, 25, score.timing.lateGreat) ||
      !optionalInt(stmt, 26, score.timing.earlyGood) ||
      !optionalInt(stmt, 27, score.timing.lateGood) ||
      !optionalInt(stmt, 28, score.timing.earlyBad) ||
      !optionalInt(stmt, 29, score.timing.lateBad) ||
      !optionalInt(stmt, 30, score.timing.earlyPoor) ||
      !optionalInt(stmt, 31, score.timing.latePoor) ||
      !optionalInt(stmt, 32, score.fast) ||
      !optionalInt(stmt, 33, score.slow) ||
      !optionalInt(stmt, 34, score.maxCombo) ||
      !optionalInt(stmt, 35, score.badPoints)) {
    diagnostic = "IR remote score row contains an out-of-range metric";
    return false;
  }
  if (sqlite3_column_type(stmt, 36) != SQLITE_NULL) {
    score.finalGauge = static_cast<float>(sqlite3_column_double(stmt, 36));
  }
  if (sqlite3_column_type(stmt, 37) != SQLITE_NULL &&
      !decodeGaugeHistory(sqliteColumnTextView(stmt, 37), score.gaugeHistory)) {
    diagnostic = "IR remote score gauge history is malformed";
    return false;
  }
  score.random = optionalText(stmt, 38);
  score.gauge = optionalText(stmt, 39);
  score.inputDevice = optionalText(stmt, 40);
  score.client = optionalText(stmt, 41);
  generation = sqlite3_column_int64(stmt, 42);

  if (!ir::validateIrRemoteScore(score, diagnostic)) {
    return false;
  }
  if (score.chartMd5.empty() || score.chartSha256.empty()) {
    diagnostic = "IR remote score row is missing a primary chart hash";
    return false;
  }
  if (generation <= 0) {
    diagnostic = "IR remote score row has an invalid generation";
    return false;
  }
  return true;
}

bool validateIdVector(const std::vector<std::int64_t> &values,
                      std::unordered_set<std::int64_t> &unique) {
  if (values.size() > ir::kMaximumIrRemoteScoreSnapshotEntries) {
    return false;
  }
  for (const std::int64_t value : values) {
    if (value <= 0 || !unique.emplace(value).second) {
      return false;
    }
  }
  return true;
}

struct ValidatedMutation {
  bool valid = false;
  std::vector<std::optional<std::string>> gaugeHistoryJson;
  std::string diagnostic;
};

ValidatedMutation
validateMutation(const ir::IrRemoteSnapshotMutation &mutation) {
  try {
    if (!validOriginIdentity(mutation.providerId, mutation.serverOrigin) ||
        mutation.synchronizedAtUnixMillis <= 0) {
      return {.diagnostic = "IR remote snapshot identity is invalid"};
    }
    std::string diagnostic;
    if (mutation.scores.size() > ir::kMaximumIrRemoteScoreSnapshotEntries) {
      return {.diagnostic = "IR remote score snapshot is oversized"};
    }
    ValidatedMutation result;
    result.gaugeHistoryJson.reserve(mutation.scores.size());
    std::unordered_set<std::string_view> remoteScoreIds;
    remoteScoreIds.reserve(mutation.scores.size());
    for (const auto &score : mutation.scores) {
      if (!ir::validateIrRemoteScore(score, diagnostic)) {
        return {.diagnostic = std::move(diagnostic)};
      }
      if (!remoteScoreIds.emplace(score.remoteScoreId).second) {
        return {.diagnostic =
                    "IR remote score snapshot has duplicate identity"};
      }
      if (score.chartMd5.empty() || score.chartSha256.empty()) {
        return {.diagnostic =
                    "IR remote score persistence requires both chart hashes"};
      }
      auto serialized = serializeGaugeHistory(score.gaugeHistory);
      if (!serialized.valid) {
        return {.diagnostic = "IR remote score gauge history is oversized"};
      }
      result.gaugeHistoryJson.push_back(std::move(serialized.json));
    }

    if (mutation.upsertedReceipts.size() >
        ir::kMaximumIrRemoteScoreSnapshotEntries) {
      return {.diagnostic = "IR remote receipt mutation is oversized"};
    }
    std::unordered_set<int> receiptReplayIds;
    std::unordered_set<std::int64_t> upsertedReceiptIds;
    for (const auto &receipt : mutation.upsertedReceipts) {
      ir::IrSubmissionReceipt validated = receipt;
      if (validated.id < 0) {
        return {.diagnostic = "IR remote receipt row ID is invalid"};
      }
      if (validated.id == 0) {
        validated.id = 1;
      } else if (!upsertedReceiptIds.emplace(validated.id).second) {
        return {.diagnostic = "IR remote receipt mutation has duplicate IDs"};
      }
      if (receipt.providerId != mutation.providerId ||
          receipt.serverOrigin != mutation.serverOrigin ||
          !receiptReplayIds.emplace(receipt.replayId).second ||
          !ir::validateIrSubmissionReceipt(validated, diagnostic)) {
        return {.diagnostic = diagnostic.empty()
                                  ? "IR remote receipt mutation is invalid"
                                  : std::move(diagnostic)};
      }
    }

    std::unordered_set<std::int64_t> deletedReceiptIds;
    std::unordered_set<std::int64_t> settledOutboxIds;
    std::unordered_set<std::int64_t> purgedOutboxIds;
    if (!validateIdVector(mutation.deletedReceiptIds, deletedReceiptIds) ||
        !validateIdVector(mutation.settledOutboxRowIds, settledOutboxIds) ||
        !validateIdVector(mutation.purgedSucceededOutboxRowIds,
                          purgedOutboxIds)) {
      return {.diagnostic = "IR remote snapshot row IDs are invalid"};
    }
    if (std::ranges::any_of(
            upsertedReceiptIds,
            [&](std::int64_t id) { return deletedReceiptIds.contains(id); }) ||
        std::ranges::any_of(settledOutboxIds, [&](std::int64_t id) {
          return purgedOutboxIds.contains(id);
        })) {
      return {.diagnostic = "IR remote snapshot row mutations conflict"};
    }
    result.valid = true;
    return result;
  } catch (...) {
    return {.diagnostic = "IR remote snapshot validation failed"};
  }
}

bool bindRemoteScore(sqlite3_stmt *stmt,
                     const ir::IrRemoteSnapshotMutation &mutation,
                     const ir::IrRemoteScore &score,
                     const std::optional<std::string> &gaugeHistoryJson,
                     std::int64_t generation) {
  int column = 1;
  return bindSqliteText(stmt, column++, mutation.providerId) &&
         bindSqliteText(stmt, column++, mutation.serverOrigin) &&
         bindSqliteText(stmt, column++, score.remoteScoreId) &&
         sqlite3_bind_int64(stmt, column++, score.remoteUserId) == SQLITE_OK &&
         bindSqliteText(stmt, column++, score.game) &&
         bindSqliteText(stmt, column++, score.remoteChartId) &&
         bindSqliteText(stmt, column++, score.chartMd5) &&
         bindSqliteText(stmt, column++, score.chartSha256) &&
         bindSqliteText(stmt, column++, score.title) &&
         bindSqliteText(stmt, column++, score.artist) &&
         bindOptionalText(stmt, column++, score.difficulty) &&
         bindOptionalText(stmt, column++, score.level) &&
         bindOptionalDouble(stmt, column++, score.levelNumber) &&
         sqlite3_bind_int(stmt, column++, score.noteCount) == SQLITE_OK &&
         sqlite3_bind_int(stmt, column++, score.score) == SQLITE_OK &&
         sqlite3_bind_int(stmt, column++, score.lampRank) == SQLITE_OK &&
         bindSqliteText(stmt, column++, score.service) &&
         bindOptionalInteger(stmt, column++, score.timeAchievedUnixMillis) &&
         sqlite3_bind_int64(stmt, column++, score.timeAddedUnixMillis) ==
             SQLITE_OK &&
         bindOptionalInt(stmt, column++, score.judgements.pGreat) &&
         bindOptionalInt(stmt, column++, score.judgements.great) &&
         bindOptionalInt(stmt, column++, score.judgements.good) &&
         bindOptionalInt(stmt, column++, score.judgements.bad) &&
         bindOptionalInt(stmt, column++, score.judgements.poor) &&
         bindOptionalInt(stmt, column++, score.timing.earlyPGreat) &&
         bindOptionalInt(stmt, column++, score.timing.latePGreat) &&
         bindOptionalInt(stmt, column++, score.timing.earlyGreat) &&
         bindOptionalInt(stmt, column++, score.timing.lateGreat) &&
         bindOptionalInt(stmt, column++, score.timing.earlyGood) &&
         bindOptionalInt(stmt, column++, score.timing.lateGood) &&
         bindOptionalInt(stmt, column++, score.timing.earlyBad) &&
         bindOptionalInt(stmt, column++, score.timing.lateBad) &&
         bindOptionalInt(stmt, column++, score.timing.earlyPoor) &&
         bindOptionalInt(stmt, column++, score.timing.latePoor) &&
         bindOptionalInt(stmt, column++, score.fast) &&
         bindOptionalInt(stmt, column++, score.slow) &&
         bindOptionalInt(stmt, column++, score.maxCombo) &&
         bindOptionalInt(stmt, column++, score.badPoints) &&
         bindOptionalFloat(stmt, column++, score.finalGauge) &&
         bindOptionalText(stmt, column++, gaugeHistoryJson) &&
         bindOptionalText(stmt, column++, score.random) &&
         bindOptionalText(stmt, column++, score.gauge) &&
         bindOptionalText(stmt, column++, score.inputDevice) &&
         bindOptionalText(stmt, column++, score.client) &&
         sqlite3_bind_int64(stmt, column, generation) == SQLITE_OK;
}

bool loadRemoteIdentitySet(sqlite3 *db, std::string_view providerId,
                           std::string_view serverOrigin,
                           std::unordered_set<std::string> &identities) {
  SqliteStatementHandle statement;
  if (prepareSqliteStatement(
          db,
          "SELECT remote_score_id FROM ir_remote_scores WHERE provider_id=? "
          "AND server_origin=?",
          statement) != SQLITE_OK ||
      sqlite3_bind_text(statement.get(), 1, providerId.data(),
                        static_cast<int>(providerId.size()),
                        SQLITE_TRANSIENT) != SQLITE_OK ||
      sqlite3_bind_text(statement.get(), 2, serverOrigin.data(),
                        static_cast<int>(serverOrigin.size()),
                        SQLITE_TRANSIENT) != SQLITE_OK) {
    return false;
  }
  int rc = SQLITE_OK;
  while ((rc = sqlite3_step(statement.get())) == SQLITE_ROW) {
    if (!columnIs(statement.get(), 0, SQLITE_TEXT) ||
        !identities.emplace(sqliteColumnString(statement.get(), 0)).second) {
      return false;
    }
  }
  return rc == SQLITE_DONE;
}

std::optional<std::int64_t>
nextGeneration(sqlite3 *db, const ir::IrRemoteSnapshotMutation &mutation) {
  SqliteStatementHandle statement;
  if (prepareSqliteStatement(
          db,
          "SELECT COALESCE(MAX(sync_generation),0) FROM ir_remote_scores "
          "WHERE provider_id=? AND server_origin=?",
          statement) != SQLITE_OK ||
      bindSqliteText(statement.get(), 1, mutation.providerId) == false ||
      bindSqliteText(statement.get(), 2, mutation.serverOrigin) == false ||
      sqlite3_step(statement.get()) != SQLITE_ROW ||
      !columnIs(statement.get(), 0, SQLITE_INTEGER)) {
    return std::nullopt;
  }
  const std::int64_t previous = sqlite3_column_int64(statement.get(), 0);
  if (previous < 0 || previous == std::numeric_limits<std::int64_t>::max() ||
      sqlite3_step(statement.get()) != SQLITE_DONE) {
    return std::nullopt;
  }
  return std::max(mutation.synchronizedAtUnixMillis, previous + 1);
}

bool insertRemoteScores(sqlite3 *db,
                        const ir::IrRemoteSnapshotMutation &mutation,
                        const ValidatedMutation &validated,
                        std::int64_t generation) {
  constexpr const char *query =
      "INSERT OR REPLACE INTO ir_remote_scores("
      "provider_id,server_origin,remote_score_id,remote_user_id,game,"
      "remote_chart_id,chart_md5,chart_sha256,title,artist,difficulty,level,"
      "level_number,note_count,score,lamp_rank,service,time_achieved_ms,"
      "time_added_ms,pgreat,great,good,bad,poor,early_pgreat,late_pgreat,"
      "early_great,late_great,early_good,late_good,early_bad,late_bad,"
      "early_poor,late_poor,fast,slow,max_combo,bad_points,final_gauge,"
      "gauge_history_json,random_mode,gauge_mode,input_device,client,"
      "sync_generation) VALUES("
      "?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,"
      "?18,?19,?20,?21,?22,?23,?24,?25,?26,?27,?28,?29,?30,?31,?32,"
      "?33,?34,?35,?36,?37,?38,?39,?40,?41,?42,?43,?44,?45)";
  SqliteStatementHandle statement;
  if (prepareSqliteStatement(db, query, statement) != SQLITE_OK) {
    return false;
  }
  for (std::size_t index = 0; index < mutation.scores.size(); ++index) {
    if (sqlite3_reset(statement.get()) != SQLITE_OK ||
        sqlite3_clear_bindings(statement.get()) != SQLITE_OK ||
        !bindRemoteScore(statement.get(), mutation, mutation.scores[index],
                         validated.gaugeHistoryJson[index], generation) ||
        sqlite3_step(statement.get()) != SQLITE_DONE ||
        sqlite3_changes(db) != 1) {
      return false;
    }
  }
  return true;
}

bool deleteOlderRemoteGeneration(sqlite3 *db,
                                 const ir::IrRemoteSnapshotMutation &mutation,
                                 std::int64_t generation) {
  SqliteStatementHandle statement;
  return prepareSqliteStatement(
             db,
             "DELETE FROM ir_remote_scores WHERE provider_id=? AND "
             "server_origin=? AND sync_generation<>?",
             statement) == SQLITE_OK &&
         bindSqliteText(statement.get(), 1, mutation.providerId) &&
         bindSqliteText(statement.get(), 2, mutation.serverOrigin) &&
         sqlite3_bind_int64(statement.get(), 3, generation) == SQLITE_OK &&
         sqlite3_step(statement.get()) == SQLITE_DONE;
}

bool insertReceipt(sqlite3 *db, const ir::IrSubmissionReceipt &receipt) {
  constexpr const char *query =
      "INSERT INTO ir_submission_receipts("
      "provider_id,server_origin,replay_id,attempt_id,chart_md5,chart_sha256,"
      "remote_user_id,remote_chart_id,remote_score_id,confirmation_source,"
      "observed_in_snapshot,confirmed_at_ms) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)";
  SqliteStatementHandle statement;
  return prepareSqliteStatement(db, query, statement) == SQLITE_OK &&
         bindSqliteText(statement.get(), 1, receipt.providerId) &&
         bindSqliteText(statement.get(), 2, receipt.serverOrigin) &&
         sqlite3_bind_int(statement.get(), 3, receipt.replayId) == SQLITE_OK &&
         bindSqliteText(statement.get(), 4, receipt.attemptId) &&
         (receipt.chartMd5.empty()
              ? sqlite3_bind_null(statement.get(), 5) == SQLITE_OK
              : bindSqliteText(statement.get(), 5, receipt.chartMd5)) &&
         bindSqliteText(statement.get(), 6, receipt.chartSha256) &&
         bindOptionalInteger(statement.get(), 7, receipt.remoteUserId) &&
         (receipt.remoteChartId.empty()
              ? sqlite3_bind_null(statement.get(), 8) == SQLITE_OK
              : bindSqliteText(statement.get(), 8, receipt.remoteChartId)) &&
         (receipt.remoteScoreId.empty()
              ? sqlite3_bind_null(statement.get(), 9) == SQLITE_OK
              : bindSqliteText(statement.get(), 9, receipt.remoteScoreId)) &&
         sqlite3_bind_int(statement.get(), 10,
                          static_cast<int>(receipt.source)) == SQLITE_OK &&
         sqlite3_bind_int(statement.get(), 11,
                          receipt.observedInSnapshot ? 1 : 0) == SQLITE_OK &&
         sqlite3_bind_int64(statement.get(), 12,
                            receipt.confirmedAtUnixMillis) == SQLITE_OK &&
         sqlite3_step(statement.get()) == SQLITE_DONE &&
         sqlite3_changes(db) == 1;
}

bool updateReceipt(sqlite3 *db, const ir::IrSubmissionReceipt &receipt) {
  constexpr const char *query =
      "UPDATE ir_submission_receipts SET replay_id=?,attempt_id=?,"
      "chart_md5=?,chart_sha256=?,remote_user_id=?,remote_chart_id=?,"
      "remote_score_id=?,confirmation_source=?,observed_in_snapshot=?,"
      "confirmed_at_ms=? WHERE id=? AND provider_id=? AND server_origin=?";
  SqliteStatementHandle statement;
  return prepareSqliteStatement(db, query, statement) == SQLITE_OK &&
         sqlite3_bind_int(statement.get(), 1, receipt.replayId) == SQLITE_OK &&
         bindSqliteText(statement.get(), 2, receipt.attemptId) &&
         (receipt.chartMd5.empty()
              ? sqlite3_bind_null(statement.get(), 3) == SQLITE_OK
              : bindSqliteText(statement.get(), 3, receipt.chartMd5)) &&
         bindSqliteText(statement.get(), 4, receipt.chartSha256) &&
         bindOptionalInteger(statement.get(), 5, receipt.remoteUserId) &&
         (receipt.remoteChartId.empty()
              ? sqlite3_bind_null(statement.get(), 6) == SQLITE_OK
              : bindSqliteText(statement.get(), 6, receipt.remoteChartId)) &&
         (receipt.remoteScoreId.empty()
              ? sqlite3_bind_null(statement.get(), 7) == SQLITE_OK
              : bindSqliteText(statement.get(), 7, receipt.remoteScoreId)) &&
         sqlite3_bind_int(statement.get(), 8,
                          static_cast<int>(receipt.source)) == SQLITE_OK &&
         sqlite3_bind_int(statement.get(), 9,
                          receipt.observedInSnapshot ? 1 : 0) == SQLITE_OK &&
         sqlite3_bind_int64(statement.get(), 10,
                            receipt.confirmedAtUnixMillis) == SQLITE_OK &&
         sqlite3_bind_int64(statement.get(), 11, receipt.id) == SQLITE_OK &&
         bindSqliteText(statement.get(), 12, receipt.providerId) &&
         bindSqliteText(statement.get(), 13, receipt.serverOrigin) &&
         sqlite3_step(statement.get()) == SQLITE_DONE &&
         sqlite3_changes(db) == 1;
}

bool applyReceiptUpserts(sqlite3 *db,
                         const ir::IrRemoteSnapshotMutation &mutation) {
  return std::ranges::all_of(mutation.upsertedReceipts, [&](const auto &row) {
    return row.id == 0 ? insertReceipt(db, row) : updateReceipt(db, row);
  });
}

bool deleteReceiptIds(sqlite3 *db, const ir::IrRemoteSnapshotMutation &mutation,
                      int &deletedCount) {
  SqliteStatementHandle statement;
  if (prepareSqliteStatement(
          db,
          "DELETE FROM ir_submission_receipts WHERE id=? AND provider_id=? "
          "AND server_origin=?",
          statement) != SQLITE_OK) {
    return false;
  }
  for (const std::int64_t id : mutation.deletedReceiptIds) {
    if (sqlite3_reset(statement.get()) != SQLITE_OK ||
        sqlite3_clear_bindings(statement.get()) != SQLITE_OK ||
        sqlite3_bind_int64(statement.get(), 1, id) != SQLITE_OK ||
        !bindSqliteText(statement.get(), 2, mutation.providerId) ||
        !bindSqliteText(statement.get(), 3, mutation.serverOrigin) ||
        sqlite3_step(statement.get()) != SQLITE_DONE ||
        sqlite3_changes(db) != 1) {
      return false;
    }
    ++deletedCount;
  }
  return true;
}

bool deleteOutboxIds(sqlite3 *db, const ir::IrRemoteSnapshotMutation &mutation,
                     const std::vector<std::int64_t> &rowIds,
                     bool succeededOnly, int &deletedCount) {
  const char *query =
      succeededOnly
          ? "DELETE FROM ir_outbox WHERE id=? AND provider_id=? AND state=5 "
            "AND EXISTS(SELECT 1 FROM ir_submission_receipts receipt WHERE "
            "receipt.provider_id=? AND receipt.server_origin=? AND "
            "receipt.attempt_id=ir_outbox.attempt_id AND "
            "receipt.confirmation_source=0 AND EXISTS(SELECT 1 FROM replays "
            "replay WHERE replay.id=receipt.replay_id AND "
            "replay.attempt_id=ir_outbox.attempt_id))"
          : "DELETE FROM ir_outbox WHERE id=? AND provider_id=? AND state IN "
            "(0,3,4) AND EXISTS(SELECT 1 FROM ir_submission_receipts receipt "
            "WHERE receipt.provider_id=? AND receipt.server_origin=? AND "
            "receipt.attempt_id=ir_outbox.attempt_id)";
  SqliteStatementHandle statement;
  if (prepareSqliteStatement(db, query, statement) != SQLITE_OK) {
    return false;
  }
  for (const std::int64_t id : rowIds) {
    if (sqlite3_reset(statement.get()) != SQLITE_OK ||
        sqlite3_clear_bindings(statement.get()) != SQLITE_OK ||
        sqlite3_bind_int64(statement.get(), 1, id) != SQLITE_OK ||
        !bindSqliteText(statement.get(), 2, mutation.providerId) ||
        !bindSqliteText(statement.get(), 3, mutation.providerId) ||
        !bindSqliteText(statement.get(), 4, mutation.serverOrigin) ||
        sqlite3_step(statement.get()) != SQLITE_DONE ||
        sqlite3_changes(db) != 1) {
      return false;
    }
    ++deletedCount;
  }
  return true;
}

std::optional<int>
countAmbiguousReceipts(sqlite3 *db,
                       const ir::IrRemoteSnapshotMutation &mutation) {
  SqliteStatementHandle statement;
  if (prepareSqliteStatement(
          db,
          "SELECT COUNT(*) FROM ir_submission_receipts WHERE provider_id=? "
          "AND server_origin=? AND observed_in_snapshot=0",
          statement) != SQLITE_OK ||
      !bindSqliteText(statement.get(), 1, mutation.providerId) ||
      !bindSqliteText(statement.get(), 2, mutation.serverOrigin) ||
      sqlite3_step(statement.get()) != SQLITE_ROW ||
      !columnIs(statement.get(), 0, SQLITE_INTEGER)) {
    return std::nullopt;
  }
  const sqlite3_int64 count = sqlite3_column_int64(statement.get(), 0);
  if (count < 0 || count > std::numeric_limits<int>::max() ||
      sqlite3_step(statement.get()) != SQLITE_DONE) {
    return std::nullopt;
  }
  return static_cast<int>(count);
}

ir::IrOutboxMutationOutcome invalidClearIdentity() {
  return {.status = ir::IrOutboxMutationStatus::Invalid,
          .diagnostic = "IR account evidence identity is invalid"};
}

} // namespace

ir::IrRemoteSnapshotApplyOutcome ReplayRepository::ApplyIrRemoteSnapshot(
    const ir::IrRemoteSnapshotMutation &mutation) {
  const ValidatedMutation validated = validateMutation(mutation);
  if (!validated.valid) {
    return {.status = ir::IrRemoteSnapshotApplyOutcome::Status::Invalid,
            .diagnostic = ir::sanitizeDiagnostic(validated.diagnostic)};
  }

  profile_database_activity::WriteGuard operation;
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {.status = ir::IrRemoteSnapshotApplyOutcome::Status::StorageFailure,
            .diagnostic = "replay storage is unavailable"};
  }
  sqlite3 *database = impl_->sessionDatabase;
  std::string transactionError;
  SqliteTransactionHandle transaction(database, "BEGIN IMMEDIATE TRANSACTION",
                                      transactionError);
  if (!transaction.active()) {
    return {.status = ir::IrRemoteSnapshotApplyOutcome::Status::StorageFailure,
            .diagnostic = "could not start IR remote snapshot transaction"};
  }

  std::unordered_set<std::string> previousIds;
  if (!loadRemoteIdentitySet(database, mutation.providerId,
                             mutation.serverOrigin, previousIds)) {
    return {.status = ir::IrRemoteSnapshotApplyOutcome::Status::StorageFailure,
            .diagnostic = "could not inspect the previous IR remote mirror"};
  }
  const auto generation = nextGeneration(database, mutation);
  if (!generation) {
    return {.status = ir::IrRemoteSnapshotApplyOutcome::Status::StorageFailure,
            .diagnostic = "could not select an IR remote mirror generation"};
  }

  std::unordered_set<std::string_view> currentIds;
  currentIds.reserve(mutation.scores.size());
  int added = 0;
  for (const auto &score : mutation.scores) {
    currentIds.emplace(score.remoteScoreId);
    if (!previousIds.contains(score.remoteScoreId)) {
      ++added;
    }
  }
  const int removed = static_cast<int>(
      std::ranges::count_if(previousIds, [&](const std::string &id) {
        return !currentIds.contains(id);
      }));

  if (!insertRemoteScores(database, mutation, validated, *generation) ||
      !deleteOlderRemoteGeneration(database, mutation, *generation)) {
    return {.status = ir::IrRemoteSnapshotApplyOutcome::Status::StorageFailure,
            .diagnostic = "could not replace the IR remote score mirror"};
  }
  if (!applyReceiptUpserts(database, mutation)) {
    return {.status = ir::IrRemoteSnapshotApplyOutcome::Status::StorageFailure,
            .diagnostic = "could not upsert IR snapshot receipts"};
  }
  int receiptsDeleted = 0;
  if (!deleteReceiptIds(database, mutation, receiptsDeleted)) {
    return {.status = ir::IrRemoteSnapshotApplyOutcome::Status::StorageFailure,
            .diagnostic = "could not delete stale IR snapshot receipts"};
  }
  int outboxRowsSettled = 0;
  if (!deleteOutboxIds(database, mutation, mutation.settledOutboxRowIds, false,
                       outboxRowsSettled) ||
      !deleteOutboxIds(database, mutation, mutation.purgedSucceededOutboxRowIds,
                       true, outboxRowsSettled)) {
    return {.status = ir::IrRemoteSnapshotApplyOutcome::Status::StorageFailure,
            .diagnostic = "could not settle represented IR outbox work"};
  }
  const auto ambiguous = countAmbiguousReceipts(database, mutation);
  if (!ambiguous) {
    return {.status = ir::IrRemoteSnapshotApplyOutcome::Status::StorageFailure,
            .diagnostic = "could not count ambiguous IR receipts"};
  }
  if (!transaction.commit(transactionError)) {
    return {.status = ir::IrRemoteSnapshotApplyOutcome::Status::StorageFailure,
            .diagnostic = "could not commit the IR remote snapshot"};
  }
  return {
      .status = ir::IrRemoteSnapshotApplyOutcome::Status::Applied,
      .remoteScoreCount = static_cast<int>(mutation.scores.size()),
      .remoteScoresAdded = added,
      .remoteScoresRemoved = removed,
      .receiptsUpserted = static_cast<int>(mutation.upsertedReceipts.size()),
      .receiptsDeleted = receiptsDeleted,
      .outboxRowsSettled = outboxRowsSettled,
      .ambiguousReceiptsPreserved = *ambiguous,
  };
}

ir::IrRemoteScoreReadOutcome
ReplayRepository::ListIrRemoteScores(std::string_view providerId,
                                     std::string_view serverOrigin) {
  if (!validOriginIdentity(providerId, serverOrigin)) {
    return {.status = ir::IrRemoteScoreReadOutcome::Status::Invalid,
            .diagnostic = "IR remote score identity is invalid"};
  }
  profile_database_activity::ReadGuard operation;
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {.status = ir::IrRemoteScoreReadOutcome::Status::StorageFailure,
            .diagnostic = "replay storage is unavailable"};
  }
  const std::string query =
      std::string("SELECT ") + kIrRemoteScoreColumns +
      " FROM ir_remote_scores WHERE provider_id=? AND server_origin=? "
      "ORDER BY remote_score_id";
  SqliteStatementHandle statement;
  if (prepareSqliteStatement(impl_->sessionDatabase, query, statement) !=
          SQLITE_OK ||
      sqlite3_bind_text(statement.get(), 1, providerId.data(),
                        static_cast<int>(providerId.size()),
                        SQLITE_TRANSIENT) != SQLITE_OK ||
      sqlite3_bind_text(statement.get(), 2, serverOrigin.data(),
                        static_cast<int>(serverOrigin.size()),
                        SQLITE_TRANSIENT) != SQLITE_OK) {
    return {.status = ir::IrRemoteScoreReadOutcome::Status::StorageFailure,
            .diagnostic = "could not prepare the IR remote score read"};
  }

  std::vector<ir::IrRemoteScore> result;
  std::optional<std::int64_t> expectedGeneration;
  int rc = SQLITE_OK;
  while ((rc = sqlite3_step(statement.get())) == SQLITE_ROW) {
    if (result.size() >= ir::kMaximumIrRemoteScoreSnapshotEntries) {
      SDL_Log("IR remote score mirror exceeds its model bound");
      return {.status = ir::IrRemoteScoreReadOutcome::Status::Invalid,
              .diagnostic = "IR remote score mirror exceeds its model bound"};
    }
    ir::IrRemoteScore score;
    std::int64_t generation = 0;
    std::string diagnostic;
    if (!decodeRemoteScoreRow(statement.get(), score, generation, diagnostic)) {
      SDL_Log("Rejecting malformed IR remote score mirror row: %s",
              diagnostic.c_str());
      return {.status = ir::IrRemoteScoreReadOutcome::Status::Invalid,
              .diagnostic = ir::sanitizeDiagnostic(diagnostic)};
    }
    if (expectedGeneration && *expectedGeneration != generation) {
      SDL_Log("Rejecting IR remote score mirror with mixed generations");
      return {.status = ir::IrRemoteScoreReadOutcome::Status::Invalid,
              .diagnostic =
                  "IR remote score mirror has mixed generations"};
    }
    expectedGeneration = generation;
    result.push_back(std::move(score));
  }
  if (rc != SQLITE_DONE) {
    return {.status = ir::IrRemoteScoreReadOutcome::Status::StorageFailure,
            .diagnostic = "could not read the IR remote score mirror"};
  }
  return {.status = ir::IrRemoteScoreReadOutcome::Status::Loaded,
          .scores = std::move(result)};
}

ir::IrOutboxMutationOutcome
ReplayRepository::ClearIrRemoteScores(std::string_view providerId,
                                      std::string_view serverOrigin) {
  if (!validOriginIdentity(providerId, serverOrigin)) {
    return invalidClearIdentity();
  }
  profile_database_activity::WriteGuard operation;
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {.status = ir::IrOutboxMutationStatus::StorageFailure,
            .diagnostic = "replay storage is unavailable"};
  }
  std::string transactionError;
  SqliteTransactionHandle transaction(
      impl_->sessionDatabase, "BEGIN IMMEDIATE TRANSACTION", transactionError);
  if (!transaction.active()) {
    return {.status = ir::IrOutboxMutationStatus::StorageFailure,
            .diagnostic = "could not start IR remote score clear"};
  }
  SqliteStatementHandle statement;
  if (prepareSqliteStatement(
          impl_->sessionDatabase,
          "DELETE FROM ir_remote_scores WHERE provider_id=? AND "
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
            .diagnostic = "could not clear IR remote scores"};
  }
  const std::size_t affectedRows = static_cast<std::size_t>(
      std::max(0, sqlite3_changes(impl_->sessionDatabase)));
  if (!transaction.commit(transactionError)) {
    return {.status = ir::IrOutboxMutationStatus::StorageFailure,
            .diagnostic = "could not commit IR remote score clear"};
  }
  return {.status = affectedRows == 0 ? ir::IrOutboxMutationStatus::NotFound
                                      : ir::IrOutboxMutationStatus::Updated,
          .affectedRows = affectedRows};
}

ir::IrOutboxMutationOutcome
ReplayRepository::ClearIrAccountEvidence(std::string_view providerId,
                                         std::string_view serverOrigin) {
  if (!validOriginIdentity(providerId, serverOrigin)) {
    return invalidClearIdentity();
  }
  profile_database_activity::WriteGuard operation;
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {.status = ir::IrOutboxMutationStatus::StorageFailure,
            .diagnostic = "replay storage is unavailable"};
  }
  sqlite3 *database = impl_->sessionDatabase;
  std::string transactionError;
  SqliteTransactionHandle transaction(database, "BEGIN IMMEDIATE TRANSACTION",
                                      transactionError);
  if (!transaction.active()) {
    return {.status = ir::IrOutboxMutationStatus::StorageFailure,
            .diagnostic = "could not start IR account evidence clear"};
  }

  std::size_t affectedRows = 0;
  SqliteStatementHandle outboxStatement;
  constexpr const char *outboxQuery =
      "DELETE FROM ir_outbox WHERE provider_id=? AND state=5 AND EXISTS("
      "SELECT 1 FROM ir_submission_receipts receipt WHERE "
      "receipt.provider_id=? AND receipt.server_origin=? AND "
      "receipt.attempt_id=ir_outbox.attempt_id AND "
      "receipt.confirmation_source=0 AND EXISTS(SELECT 1 FROM replays replay "
      "WHERE replay.id=receipt.replay_id AND "
      "replay.attempt_id=ir_outbox.attempt_id))";
  if (prepareSqliteStatement(database, outboxQuery, outboxStatement) !=
          SQLITE_OK ||
      sqlite3_bind_text(outboxStatement.get(), 1, providerId.data(),
                        static_cast<int>(providerId.size()),
                        SQLITE_TRANSIENT) != SQLITE_OK ||
      sqlite3_bind_text(outboxStatement.get(), 2, providerId.data(),
                        static_cast<int>(providerId.size()),
                        SQLITE_TRANSIENT) != SQLITE_OK ||
      sqlite3_bind_text(outboxStatement.get(), 3, serverOrigin.data(),
                        static_cast<int>(serverOrigin.size()),
                        SQLITE_TRANSIENT) != SQLITE_OK ||
      sqlite3_step(outboxStatement.get()) != SQLITE_DONE) {
    return {.status = ir::IrOutboxMutationStatus::StorageFailure,
            .diagnostic = "could not clear receipt-backed IR outbox rows"};
  }
  affectedRows +=
      static_cast<std::size_t>(std::max(0, sqlite3_changes(database)));

  SqliteStatementHandle receiptStatement;
  if (prepareSqliteStatement(
          database,
          "DELETE FROM ir_submission_receipts WHERE provider_id=? AND "
          "server_origin=?",
          receiptStatement) != SQLITE_OK ||
      sqlite3_bind_text(receiptStatement.get(), 1, providerId.data(),
                        static_cast<int>(providerId.size()),
                        SQLITE_TRANSIENT) != SQLITE_OK ||
      sqlite3_bind_text(receiptStatement.get(), 2, serverOrigin.data(),
                        static_cast<int>(serverOrigin.size()),
                        SQLITE_TRANSIENT) != SQLITE_OK ||
      sqlite3_step(receiptStatement.get()) != SQLITE_DONE) {
    return {.status = ir::IrOutboxMutationStatus::StorageFailure,
            .diagnostic = "could not clear IR submission receipts"};
  }
  affectedRows +=
      static_cast<std::size_t>(std::max(0, sqlite3_changes(database)));

  SqliteStatementHandle remoteStatement;
  if (prepareSqliteStatement(
          database,
          "DELETE FROM ir_remote_scores WHERE provider_id=? AND "
          "server_origin=?",
          remoteStatement) != SQLITE_OK ||
      sqlite3_bind_text(remoteStatement.get(), 1, providerId.data(),
                        static_cast<int>(providerId.size()),
                        SQLITE_TRANSIENT) != SQLITE_OK ||
      sqlite3_bind_text(remoteStatement.get(), 2, serverOrigin.data(),
                        static_cast<int>(serverOrigin.size()),
                        SQLITE_TRANSIENT) != SQLITE_OK ||
      sqlite3_step(remoteStatement.get()) != SQLITE_DONE) {
    return {.status = ir::IrOutboxMutationStatus::StorageFailure,
            .diagnostic = "could not clear IR remote scores"};
  }
  affectedRows +=
      static_cast<std::size_t>(std::max(0, sqlite3_changes(database)));

  if (!transaction.commit(transactionError)) {
    return {.status = ir::IrOutboxMutationStatus::StorageFailure,
            .diagnostic = "could not commit IR account evidence clear"};
  }
  return {.status = affectedRows == 0 ? ir::IrOutboxMutationStatus::NotFound
                                      : ir::IrOutboxMutationStatus::Updated,
          .affectedRows = affectedRows};
}
