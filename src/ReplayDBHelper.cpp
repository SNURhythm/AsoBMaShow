#include "ReplayDBHelper.h"

#include "SqliteRAII.h"
#include "Utils.h"
#include "path.h"
#include "targets.h"

#include <SDL2/SDL.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

namespace {
std::filesystem::path toStoredChartPath(std::filesystem::path path) {
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  static const std::filesystem::path documents =
      Utils::GetDocumentsPath("BMS/");
  const std::string documentString = documents.string();
  const std::string pathString = path.string();
  if (pathString.find(documentString) == 0) {
    path = pathString.substr(documentString.length());
  }
#endif
  return path;
}

std::string trimCopy(const std::string &value) {
  const auto begin =
      std::find_if_not(value.begin(), value.end(),
                       [](unsigned char c) { return std::isspace(c) != 0; });
  const auto end =
      std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) {
        return std::isspace(c) != 0;
      }).base();
  if (begin >= end) {
    return "";
  }
  return std::string(begin, end);
}

std::string lowerCopy(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

std::string normalizedHash(const std::string &value) {
  return lowerCopy(trimCopy(value));
}

bool execSql(sqlite3 *db, const char *query, const char *context) {
  SqliteErrorMessageHandle errMsg;
  const int rc = sqlite3_exec(db, query, nullptr, nullptr, errMsg.out());
  if (rc != SQLITE_OK) {
    SDL_Log("SQL error while %s: %s", context,
            errMsg.get() != nullptr ? errMsg.get() : sqlite3_errmsg(db));
    return false;
  }
  return true;
}

bool tableHasColumn(sqlite3 *db, const char *tableName,
                    const char *columnName) {
  const std::string query = std::string("PRAGMA table_info(") + tableName + ")";
  SqliteStatementHandle stmt;
  const int rc = prepareSqliteStatement(db, query, stmt);
  if (rc != SQLITE_OK) {
    SDL_Log("SQL error while reading replay schema: %s", sqlite3_errmsg(db));
    return false;
  }

  bool found = false;
  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    const auto *text =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt.get(), 1));
    if (text != nullptr && std::string(text) == columnName) {
      found = true;
      break;
    }
  }
  return found;
}

bool bindText(sqlite3_stmt *stmt, int idx, const std::string &value) {
  return sqlite3_bind_text(stmt, idx, value.c_str(), -1, SQLITE_TRANSIENT) ==
         SQLITE_OK;
}

void bindOptionalText(sqlite3_stmt *stmt, int idx,
                      const std::optional<std::string> &value) {
  if (value.has_value() && !value->empty()) {
    bindText(stmt, idx, *value);
  } else {
    sqlite3_bind_null(stmt, idx);
  }
}

void bindOptionalInt64(sqlite3_stmt *stmt, int idx,
                       const std::optional<long long> &value) {
  if (value.has_value()) {
    sqlite3_bind_int64(stmt, idx, static_cast<sqlite3_int64>(*value));
  } else {
    sqlite3_bind_null(stmt, idx);
  }
}

struct ReplayChartMatch {
  std::string chartPath;
  std::string sha256;
  std::string md5;
};

ReplayChartMatch replayChartMatchFor(const bms_parser::ChartMeta &chartMeta) {
  return {
      .chartPath = path_t_to_utf8(
          fspath_to_path_t(toStoredChartPath(chartMeta.BmsPath))),
      .sha256 = normalizedHash(chartMeta.SHA256),
      .md5 = normalizedHash(chartMeta.MD5),
  };
}

int bindReplayChartMatch(sqlite3_stmt *stmt, int bindIndex,
                         const ReplayChartMatch &match) {
  bindText(stmt, bindIndex++, match.sha256);
  bindText(stmt, bindIndex++, match.sha256);
  bindText(stmt, bindIndex++, match.md5);
  bindText(stmt, bindIndex++, match.md5);
  bindText(stmt, bindIndex++, match.chartPath);
  bindText(stmt, bindIndex++, match.chartPath);
  return bindIndex;
}

std::string readText(sqlite3_stmt *stmt, int idx) {
  const auto *text =
      reinterpret_cast<const char *>(sqlite3_column_text(stmt, idx));
  return text == nullptr ? "" : std::string(text);
}

std::optional<std::string> serializeRandomValues(
    const std::vector<int> &values) {
  if (values.empty()) {
    return std::nullopt;
  }
  std::ostringstream output;
  for (size_t i = 0; i < values.size(); ++i) {
    if (i > 0) {
      output << ",";
    }
    output << values[i];
  }
  return output.str();
}

std::vector<int> parseRandomValues(const std::string &value) {
  std::vector<int> values;
  std::istringstream input(value);
  std::string token;
  while (std::getline(input, token, ',')) {
    token = trimCopy(token);
    if (token.empty()) {
      continue;
    }
    char *end = nullptr;
    const long parsed = std::strtol(token.c_str(), &end, 10);
    if (end != token.c_str()) {
      values.push_back(static_cast<int>(parsed));
    }
  }
  return values;
}

ReplayEventAction actionFromInt(int value) {
  switch (value) {
  case 1:
    return ReplayEventAction::Release;
  case 2:
    return ReplayEventAction::Miss;
  case 3:
    return ReplayEventAction::Mine;
  case 0:
  default:
    return ReplayEventAction::Press;
  }
}

ReplayTouchAction touchActionFromInt(int value) {
  switch (value) {
  case 0:
    return ReplayTouchAction::Down;
  case 2:
    return ReplayTouchAction::Up;
  case 3:
    return ReplayTouchAction::Cancel;
  case 1:
  default:
    return ReplayTouchAction::Move;
  }
}

GaugeType gaugeTypeFromInt(int value) {
  if (value < 0 || value >= static_cast<int>(kGaugeTypeCount)) {
    return GaugeType::Normal;
  }
  return gaugeTypeAtIndex(value);
}

int normalizeReplayLongNoteModeValue(int lnMode) {
  return lnMode >= 1 && lnMode <= 3 ? lnMode : 0;
}

Judgement judgementFromInt(int value) {
  if (value < 0 || value >= JudgementCount) {
    return None;
  }
  return static_cast<Judgement>(value);
}

bool insertReplayEvent(sqlite3_stmt *stmt, int replayId, int eventIndex,
                       const ReplayEvent &event) {
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);

  int bindIndex = 1;
  sqlite3_bind_int(stmt, bindIndex++, replayId);
  sqlite3_bind_int(stmt, bindIndex++, eventIndex);
  sqlite3_bind_int(stmt, bindIndex++, static_cast<int>(event.action));
  sqlite3_bind_int(stmt, bindIndex++, event.lane);
  sqlite3_bind_int64(stmt, bindIndex++, event.noteTimeMicros);
  sqlite3_bind_int64(stmt, bindIndex++, event.songTimeMicros);
  sqlite3_bind_int64(stmt, bindIndex++, event.judgeTimeMicros);
  sqlite3_bind_int(stmt, bindIndex++, static_cast<int>(event.judgement));
  sqlite3_bind_int64(stmt, bindIndex++, event.diffMicros);
  sqlite3_bind_double(stmt, bindIndex++, event.gauge);
  sqlite3_bind_int(stmt, bindIndex++, gaugeTypeIndex(event.gaugeType));
  sqlite3_bind_int(stmt, bindIndex++, event.combo);
  sqlite3_bind_int(stmt, bindIndex++, event.score);

  return sqlite3_step(stmt) == SQLITE_DONE;
}

bool insertReplayTouchSample(sqlite3_stmt *stmt, int replayId, int sampleIndex,
                             const ReplayTouchSample &sample) {
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);

  int bindIndex = 1;
  sqlite3_bind_int(stmt, bindIndex++, replayId);
  sqlite3_bind_int(stmt, bindIndex++, sampleIndex);
  sqlite3_bind_int(stmt, bindIndex++, static_cast<int>(sample.action));
  sqlite3_bind_int64(stmt, bindIndex++,
                     static_cast<sqlite3_int64>(sample.fingerId));
  sqlite3_bind_int64(stmt, bindIndex++, sample.songTimeMicros);
  sqlite3_bind_double(stmt, bindIndex++, sample.x);
  sqlite3_bind_double(stmt, bindIndex++, sample.y);

  return sqlite3_step(stmt) == SQLITE_DONE;
}

bool insertReplayLaneCoverEvent(sqlite3_stmt *stmt, int replayId,
                                int eventIndex,
                                const ReplayLaneCoverEvent &event) {
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);

  int bindIndex = 1;
  sqlite3_bind_int(stmt, bindIndex++, replayId);
  sqlite3_bind_int(stmt, bindIndex++, eventIndex);
  sqlite3_bind_int64(stmt, bindIndex++, event.songTimeMicros);
  sqlite3_bind_int(stmt, bindIndex++, event.noteStartPositionPercent);
  sqlite3_bind_int(stmt, bindIndex++,
                   event.resetVisibleTimeReference ? 1 : 0);

  return sqlite3_step(stmt) == SQLITE_DONE;
}

ReplaySummary readReplaySummary(sqlite3_stmt *stmt, int eventCountColumn,
                                int touchSampleCountColumn) {
  ReplaySummary summary;
  summary.id = sqlite3_column_int(stmt, 0);
  summary.initialGaugeType = gaugeTypeFromInt(sqlite3_column_int(stmt, 6));
  summary.gaugeAutoShift = sqlite3_column_int(stmt, 7) != 0;
  summary.finalScore = sqlite3_column_int(stmt, 8);
  summary.finalGauge = static_cast<float>(sqlite3_column_double(stmt, 9));
  summary.clearType = sqlite3_column_int(stmt, 10);
  summary.createdAt = readText(stmt, 11);
  if (sqlite3_column_type(stmt, 12) != SQLITE_NULL) {
    summary.playOption = readText(stmt, 12);
  }
  if (sqlite3_column_type(stmt, 13) != SQLITE_NULL) {
    summary.playOptionSeed = sqlite3_column_int64(stmt, 13);
  }
  if (sqlite3_column_type(stmt, 14) != SQLITE_NULL) {
    summary.playOption2 = readText(stmt, 14);
  }
  if (sqlite3_column_type(stmt, 15) != SQLITE_NULL) {
    summary.playOption2Seed = sqlite3_column_int64(stmt, 15);
  }
  if (sqlite3_column_type(stmt, 16) != SQLITE_NULL) {
    summary.assistOption = assist_options::normalize(readText(stmt, 16));
  }
  summary.eventCount = sqlite3_column_int(stmt, eventCountColumn);
  summary.touchSampleCount = sqlite3_column_int(stmt, touchSampleCountColumn);
  return summary;
}
} // namespace

ReplayDBHelper &ReplayDBHelper::GetInstance() {
  sqlite3_config(SQLITE_CONFIG_SERIALIZED);
  static ReplayDBHelper instance;
  return instance;
}

sqlite3 *ReplayDBHelper::Connect() {
  const std::filesystem::path directory = Utils::GetDocumentsPath("db");
  std::filesystem::create_directories(directory);
  const std::filesystem::path path = directory / "replay.db";

  sqlite3 *db = nullptr;
  const int rc = sqlite3_open(path.string().c_str(), &db);
  if (db != nullptr) {
    sqlite3_busy_timeout(db, 1000);
  }
  if (rc != SQLITE_OK) {
    SDL_Log("Can't open replay database: %s",
            db != nullptr ? sqlite3_errmsg(db) : "unknown error");
    if (db != nullptr) {
      sqlite3_close(db);
    }
    return nullptr;
  }

  sqlite3_exec(db, "PRAGMA foreign_keys=ON", nullptr, nullptr, nullptr);
  sqlite3_exec(db, "PRAGMA journal_mode=WAL", nullptr, nullptr, nullptr);
  sqlite3_exec(db, "PRAGMA synchronous=NORMAL", nullptr, nullptr, nullptr);
  return db;
}

void ReplayDBHelper::Close(sqlite3 *db) {
  if (db != nullptr) {
    sqlite3_close(db);
  }
}

bool ReplayDBHelper::CreateReplayTables(sqlite3 *db) {
  const char *replayQuery = "CREATE TABLE IF NOT EXISTS replays ("
                            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                            "chart_path TEXT,"
                            "chart_md5 TEXT,"
                            "chart_sha256 TEXT,"
                            "chart_title TEXT,"
                            "chart_artist TEXT,"
                            "ln_mode INTEGER NOT NULL DEFAULT 0,"
                            "gauge_type INTEGER NOT NULL,"
                            "gauge_auto_shift INTEGER NOT NULL,"
                            "final_score INTEGER NOT NULL,"
                            "final_gauge REAL NOT NULL,"
                            "clear_type INTEGER NOT NULL,"
                            "random_seed INTEGER,"
                            "random_prng TEXT,"
                            "random_values TEXT,"
                            "play_option TEXT,"
                            "play_option_seed INTEGER,"
                            "play_option2 TEXT,"
                            "play_option2_seed INTEGER,"
                            "assist_option TEXT,"
                            "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP"
                            ")";
  if (!execSql(db, replayQuery, "creating replay table")) {
    return false;
  }
  if (!tableHasColumn(db, "replays", "random_seed") &&
      !execSql(db, "ALTER TABLE replays ADD COLUMN random_seed INTEGER",
               "adding replay random seed column")) {
    return false;
  }
  if (!tableHasColumn(db, "replays", "random_prng") &&
      !execSql(db, "ALTER TABLE replays ADD COLUMN random_prng TEXT",
               "adding replay random PRNG column")) {
    return false;
  }
  if (!tableHasColumn(db, "replays", "random_values") &&
      !execSql(db, "ALTER TABLE replays ADD COLUMN random_values TEXT",
               "adding replay random values column")) {
    return false;
  }
  if (!tableHasColumn(db, "replays", "play_option") &&
      !execSql(db, "ALTER TABLE replays ADD COLUMN play_option TEXT",
               "adding replay play option column")) {
    return false;
  }
  if (!tableHasColumn(db, "replays", "play_option_seed") &&
      !execSql(db, "ALTER TABLE replays ADD COLUMN play_option_seed INTEGER",
               "adding replay play option seed column")) {
    return false;
  }
  if (!tableHasColumn(db, "replays", "play_option2") &&
      !execSql(db, "ALTER TABLE replays ADD COLUMN play_option2 TEXT",
               "adding replay 2P play option column")) {
    return false;
  }
  if (!tableHasColumn(db, "replays", "play_option2_seed") &&
      !execSql(db, "ALTER TABLE replays ADD COLUMN play_option2_seed INTEGER",
               "adding replay 2P play option seed column")) {
    return false;
  }
  if (!tableHasColumn(db, "replays", "assist_option") &&
      !execSql(db, "ALTER TABLE replays ADD COLUMN assist_option TEXT",
               "adding replay assist option column")) {
    return false;
  }
  if (!tableHasColumn(db, "replays", "ln_mode") &&
      !execSql(db,
               "ALTER TABLE replays ADD COLUMN ln_mode INTEGER NOT NULL "
               "DEFAULT 0",
               "adding replay long note mode column")) {
    return false;
  }

  const char *eventQuery =
      "CREATE TABLE IF NOT EXISTS replay_events ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "replay_id INTEGER NOT NULL,"
      "event_index INTEGER NOT NULL,"
      "action INTEGER NOT NULL,"
      "lane INTEGER NOT NULL,"
      "note_time_micros INTEGER NOT NULL,"
      "song_time_micros INTEGER NOT NULL,"
      "judge_time_micros INTEGER NOT NULL,"
      "judgement INTEGER NOT NULL,"
      "diff_micros INTEGER NOT NULL,"
      "gauge REAL NOT NULL,"
      "gauge_type INTEGER NOT NULL,"
      "combo INTEGER NOT NULL,"
      "score INTEGER NOT NULL,"
      "FOREIGN KEY(replay_id) REFERENCES replays(id) ON DELETE CASCADE"
      ")";
  if (!execSql(db, eventQuery, "creating replay event table")) {
    return false;
  }

  const char *touchSampleQuery =
      "CREATE TABLE IF NOT EXISTS replay_touch_samples ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "replay_id INTEGER NOT NULL,"
      "sample_index INTEGER NOT NULL,"
      "action INTEGER NOT NULL,"
      "finger_id INTEGER NOT NULL,"
      "song_time_micros INTEGER NOT NULL,"
      "x REAL NOT NULL,"
      "y REAL NOT NULL,"
      "FOREIGN KEY(replay_id) REFERENCES replays(id) ON DELETE CASCADE"
      ")";
  if (!execSql(db, touchSampleQuery, "creating replay touch sample table")) {
    return false;
  }

  const char *laneCoverEventQuery =
      "CREATE TABLE IF NOT EXISTS replay_lane_cover_events ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "replay_id INTEGER NOT NULL,"
      "event_index INTEGER NOT NULL,"
      "song_time_micros INTEGER NOT NULL,"
      "note_start_position_percent INTEGER NOT NULL,"
      "reset_visible_time_reference INTEGER NOT NULL,"
      "FOREIGN KEY(replay_id) REFERENCES replays(id) ON DELETE CASCADE"
      ")";
  if (!execSql(db, laneCoverEventQuery,
               "creating replay lane cover event table")) {
    return false;
  }

  const char *indexes[] = {
      "CREATE INDEX IF NOT EXISTS idx_replays_chart_sha256 ON "
      "replays(chart_sha256, id)",
      "CREATE INDEX IF NOT EXISTS idx_replays_chart_md5 ON "
      "replays(chart_md5, id)",
      "CREATE INDEX IF NOT EXISTS idx_replays_chart_path ON "
      "replays(chart_path, id)",
      "CREATE INDEX IF NOT EXISTS idx_replay_events_replay_order ON "
      "replay_events(replay_id, event_index)",
      "CREATE INDEX IF NOT EXISTS idx_replay_touch_samples_replay_order ON "
      "replay_touch_samples(replay_id, sample_index)",
      "CREATE INDEX IF NOT EXISTS idx_replay_lane_cover_events_replay_order ON "
      "replay_lane_cover_events(replay_id, event_index)",
  };
  for (const auto *indexQuery : indexes) {
    if (!execSql(db, indexQuery, "creating replay index")) {
      return false;
    }
  }
  return true;
}

std::optional<int> ReplayDBHelper::SaveReplay(const ReplayData &replay) {
  SqliteConnectionHandle dbHandle(Connect());
  sqlite3 *db = dbHandle.get();
  if (db == nullptr) {
    return std::nullopt;
  }

  if (!CreateReplayTables(db) ||
      !execSql(db, "BEGIN IMMEDIATE TRANSACTION", "starting replay save")) {
    return std::nullopt;
  }

  const char *replayInsert =
      "INSERT INTO replays ("
      "chart_path, chart_md5, chart_sha256, chart_title, chart_artist,"
      "gauge_type, gauge_auto_shift, final_score, final_gauge, clear_type,"
      "random_seed, random_prng, random_values, play_option, play_option_seed,"
      "play_option2, play_option2_seed, assist_option, ln_mode"
      ") VALUES ("
      "@chart_path, @chart_md5, @chart_sha256, @chart_title, @chart_artist,"
      "@gauge_type, @gauge_auto_shift, @final_score, @final_gauge,"
      "@clear_type, @random_seed, @random_prng, @random_values,"
      "@play_option, @play_option_seed, @play_option2, @play_option2_seed,"
      "@assist_option, @ln_mode"
      ")";

  SqliteStatementHandle replayStmt;
  int rc = prepareSqliteStatement(db, replayInsert, replayStmt);
  if (rc != SQLITE_OK) {
    SDL_Log("SQL error while preparing replay insert: %s", sqlite3_errmsg(db));
    execSql(db, "ROLLBACK", "rolling back replay save");
    return std::nullopt;
  }

  const auto chartPath = path_t_to_utf8(
      fspath_to_path_t(toStoredChartPath(replay.chartMeta.BmsPath)));
  int bindIndex = 1;
  bindText(replayStmt.get(), bindIndex++, chartPath);
  bindText(replayStmt.get(), bindIndex++, replay.chartMeta.MD5);
  bindText(replayStmt.get(), bindIndex++, replay.chartMeta.SHA256);
  bindText(replayStmt.get(), bindIndex++, replay.chartMeta.Title);
  bindText(replayStmt.get(), bindIndex++, replay.chartMeta.Artist);
  sqlite3_bind_int(replayStmt.get(), bindIndex++,
                   gaugeTypeIndex(replay.initialGaugeType));
  sqlite3_bind_int(replayStmt.get(), bindIndex++,
                   replay.gaugeAutoShift ? 1 : 0);
  sqlite3_bind_int(replayStmt.get(), bindIndex++, replay.finalScore);
  sqlite3_bind_double(replayStmt.get(), bindIndex++, replay.finalGauge);
  sqlite3_bind_int(replayStmt.get(), bindIndex++, replay.clearType);
  if (replay.randomSeed.has_value()) {
    sqlite3_bind_int64(replayStmt.get(), bindIndex++,
                       static_cast<sqlite3_int64>(*replay.randomSeed));
  } else {
    sqlite3_bind_null(replayStmt.get(), bindIndex++);
  }
  if (replay.randomPrng.has_value()) {
    bindText(replayStmt.get(), bindIndex++, *replay.randomPrng);
  } else if (replay.randomSeed.has_value()) {
    bindText(replayStmt.get(), bindIndex++,
             bms_parser::Parser::RandomPrngId);
  } else {
    sqlite3_bind_null(replayStmt.get(), bindIndex++);
  }
  bindOptionalText(replayStmt.get(), bindIndex++,
                   serializeRandomValues(replay.randomValues));
  bindOptionalText(replayStmt.get(), bindIndex++, replay.playOption);
  bindOptionalInt64(replayStmt.get(), bindIndex++, replay.playOptionSeed);
  bindOptionalText(replayStmt.get(), bindIndex++, replay.playOption2);
  bindOptionalInt64(replayStmt.get(), bindIndex++, replay.playOption2Seed);
  bindText(replayStmt.get(), bindIndex++,
           assist_options::normalize(replay.assistOption));
  sqlite3_bind_int(replayStmt.get(), bindIndex++,
                   normalizeReplayLongNoteModeValue(replay.chartMeta.LnMode));

  rc = sqlite3_step(replayStmt.get());
  replayStmt.reset();
  if (rc != SQLITE_DONE) {
    SDL_Log("SQL error while saving replay: %s", sqlite3_errmsg(db));
    execSql(db, "ROLLBACK", "rolling back replay save");
    return std::nullopt;
  }

  const int replayId = static_cast<int>(sqlite3_last_insert_rowid(db));
  const char *eventInsert =
      "INSERT INTO replay_events ("
      "replay_id, event_index, action, lane, note_time_micros,"
      "song_time_micros, judge_time_micros, judgement, diff_micros,"
      "gauge, gauge_type, combo, score"
      ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

  SqliteStatementHandle eventStmt;
  rc = prepareSqliteStatement(db, eventInsert, eventStmt);
  if (rc != SQLITE_OK) {
    SDL_Log("SQL error while preparing replay event insert: %s",
            sqlite3_errmsg(db));
    execSql(db, "ROLLBACK", "rolling back replay save");
    return std::nullopt;
  }

  bool eventOk = true;
  for (size_t i = 0; i < replay.events.size(); ++i) {
    if (!insertReplayEvent(eventStmt.get(), replayId, static_cast<int>(i),
                           replay.events[i])) {
      SDL_Log("SQL error while saving replay event: %s", sqlite3_errmsg(db));
      eventOk = false;
      break;
    }
  }
  eventStmt.reset();

  const char *touchSampleInsert =
      "INSERT INTO replay_touch_samples ("
      "replay_id, sample_index, action, finger_id, song_time_micros, x, y"
      ") VALUES (?, ?, ?, ?, ?, ?, ?)";

  SqliteStatementHandle touchSampleStmt;
  rc = prepareSqliteStatement(db, touchSampleInsert, touchSampleStmt);
  if (rc != SQLITE_OK) {
    SDL_Log("SQL error while preparing replay touch sample insert: %s",
            sqlite3_errmsg(db));
    execSql(db, "ROLLBACK", "rolling back replay save");
    return std::nullopt;
  }

  bool touchSampleOk = true;
  for (size_t i = 0; i < replay.touchSamples.size(); ++i) {
    if (!insertReplayTouchSample(touchSampleStmt.get(), replayId,
                                 static_cast<int>(i),
                                 replay.touchSamples[i])) {
      SDL_Log("SQL error while saving replay touch sample: %s",
              sqlite3_errmsg(db));
      touchSampleOk = false;
      break;
    }
  }
  touchSampleStmt.reset();

  const char *laneCoverEventInsert =
      "INSERT INTO replay_lane_cover_events ("
      "replay_id, event_index, song_time_micros,"
      "note_start_position_percent, reset_visible_time_reference"
      ") VALUES (?, ?, ?, ?, ?)";

  SqliteStatementHandle laneCoverEventStmt;
  rc = prepareSqliteStatement(db, laneCoverEventInsert, laneCoverEventStmt);
  if (rc != SQLITE_OK) {
    SDL_Log("SQL error while preparing replay lane cover event insert: %s",
            sqlite3_errmsg(db));
    execSql(db, "ROLLBACK", "rolling back replay save");
    return std::nullopt;
  }

  bool laneCoverEventOk = true;
  for (size_t i = 0; i < replay.laneCoverEvents.size(); ++i) {
    if (!insertReplayLaneCoverEvent(laneCoverEventStmt.get(), replayId,
                                    static_cast<int>(i),
                                    replay.laneCoverEvents[i])) {
      SDL_Log("SQL error while saving replay lane cover event: %s",
              sqlite3_errmsg(db));
      laneCoverEventOk = false;
      break;
    }
  }
  laneCoverEventStmt.reset();

  if (!eventOk || !touchSampleOk || !laneCoverEventOk ||
      !execSql(db, "COMMIT", "committing replay save")) {
    execSql(db, "ROLLBACK", "rolling back replay save");
    return std::nullopt;
  }

  return replayId;
}

std::vector<ReplaySummary>
ReplayDBHelper::ListReplays(const bms_parser::ChartMeta &chartMeta, int limit) {
  std::vector<ReplaySummary> replays;
  SqliteConnectionHandle dbHandle(Connect());
  sqlite3 *db = dbHandle.get();
  if (db == nullptr) {
    return replays;
  }

  if (!CreateReplayTables(db)) {
    return replays;
  }

  const auto match = replayChartMatchFor(chartMeta);
  limit = std::max(1, limit);

  const char *query =
      "SELECT r.id, r.chart_path, r.chart_md5, r.chart_sha256,"
      "r.chart_title, r.chart_artist, r.gauge_type, r.gauge_auto_shift,"
      "r.final_score, r.final_gauge, r.clear_type, r.created_at,"
      "r.play_option, r.play_option_seed, r.play_option2,"
      "r.play_option2_seed, r.assist_option,"
      "(SELECT COUNT(*) FROM replay_events e WHERE e.replay_id = r.id),"
      "(SELECT COUNT(*) FROM replay_touch_samples t WHERE t.replay_id = r.id) "
      "FROM replays r "
      "WHERE ((? != '' AND r.chart_sha256 = ?) OR "
      "(? != '' AND r.chart_md5 = ?) OR "
      "(? != '' AND r.chart_path = ?)) "
      "ORDER BY r.id DESC LIMIT ?";

  SqliteStatementHandle stmt;
  int rc = prepareSqliteStatement(db, query, stmt);
  if (rc != SQLITE_OK) {
    SDL_Log("SQL error while preparing replay list: %s", sqlite3_errmsg(db));
    return replays;
  }

  int bindIndex = bindReplayChartMatch(stmt.get(), 1, match);
  sqlite3_bind_int(stmt.get(), bindIndex++, limit);

  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    replays.push_back(readReplaySummary(stmt.get(), 17, 18));
  }
  return replays;
}

std::optional<ReplayData>
ReplayDBHelper::LoadReplay(int replayId,
                           const bms_parser::ChartMeta &chartMeta) {
  SqliteConnectionHandle dbHandle(Connect());
  sqlite3 *db = dbHandle.get();
  if (db == nullptr) {
    return std::nullopt;
  }

  if (!CreateReplayTables(db)) {
    return std::nullopt;
  }

  const auto match = replayChartMatchFor(chartMeta);
  const char *query =
      "SELECT id, chart_path, chart_md5, chart_sha256, chart_title,"
      "chart_artist, gauge_type, gauge_auto_shift, final_score, final_gauge,"
      "clear_type, created_at, random_seed, random_prng, random_values,"
      "play_option,"
      "play_option_seed, play_option2, play_option2_seed, assist_option, "
      "ln_mode "
      "FROM replays WHERE id = ? AND "
      "((? != '' AND chart_sha256 = ?) OR "
      "(? != '' AND chart_md5 = ?) OR "
      "(? != '' AND chart_path = ?))";

  SqliteStatementHandle stmt;
  int rc = prepareSqliteStatement(db, query, stmt);
  if (rc != SQLITE_OK) {
    SDL_Log("SQL error while preparing replay load: %s", sqlite3_errmsg(db));
    return std::nullopt;
  }

  sqlite3_bind_int(stmt.get(), 1, replayId);
  bindReplayChartMatch(stmt.get(), 2, match);
  std::optional<ReplayData> replay;
  if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    ReplayData loaded;
    loaded.id = sqlite3_column_int(stmt.get(), 0);
    loaded.chartMeta = chartMeta;
    loaded.chartMeta.BmsPath = readText(stmt.get(), 1);
    loaded.chartMeta.MD5 = readText(stmt.get(), 2);
    loaded.chartMeta.SHA256 = readText(stmt.get(), 3);
    loaded.chartMeta.Title = readText(stmt.get(), 4);
    loaded.chartMeta.Artist = readText(stmt.get(), 5);
    loaded.initialGaugeType =
        gaugeTypeFromInt(sqlite3_column_int(stmt.get(), 6));
    loaded.gaugeAutoShift = sqlite3_column_int(stmt.get(), 7) != 0;
    loaded.finalScore = sqlite3_column_int(stmt.get(), 8);
    loaded.finalGauge =
        static_cast<float>(sqlite3_column_double(stmt.get(), 9));
    loaded.clearType = sqlite3_column_int(stmt.get(), 10);
    loaded.createdAt = readText(stmt.get(), 11);
    if (sqlite3_column_type(stmt.get(), 12) != SQLITE_NULL) {
      loaded.randomSeed =
          static_cast<unsigned int>(sqlite3_column_int64(stmt.get(), 12));
      loaded.chartMeta.RandomSeed = loaded.randomSeed;
    }
    if (sqlite3_column_type(stmt.get(), 13) != SQLITE_NULL) {
      loaded.randomPrng = readText(stmt.get(), 13);
    } else if (loaded.randomSeed.has_value()) {
      loaded.randomPrng = bms_parser::Parser::RandomPrngId;
    }
    loaded.chartMeta.RandomPrng = loaded.randomPrng;
    if (sqlite3_column_type(stmt.get(), 14) != SQLITE_NULL) {
      loaded.randomValues = parseRandomValues(readText(stmt.get(), 14));
    }
    loaded.chartMeta.RandomValues = loaded.randomValues;
    if (sqlite3_column_type(stmt.get(), 15) != SQLITE_NULL) {
      loaded.playOption = readText(stmt.get(), 15);
    }
    if (sqlite3_column_type(stmt.get(), 16) != SQLITE_NULL) {
      loaded.playOptionSeed = sqlite3_column_int64(stmt.get(), 16);
    }
    if (sqlite3_column_type(stmt.get(), 17) != SQLITE_NULL) {
      loaded.playOption2 = readText(stmt.get(), 17);
    }
    if (sqlite3_column_type(stmt.get(), 18) != SQLITE_NULL) {
      loaded.playOption2Seed = sqlite3_column_int64(stmt.get(), 18);
    }
    if (sqlite3_column_type(stmt.get(), 19) != SQLITE_NULL) {
      loaded.assistOption = assist_options::normalize(readText(stmt.get(), 19));
    }
    const int replayLongNoteMode =
        normalizeReplayLongNoteModeValue(sqlite3_column_int(stmt.get(), 20));
    if (replayLongNoteMode > 0) {
      loaded.chartMeta.LnMode = replayLongNoteMode;
    }
    replay = std::move(loaded);
  }
  stmt.reset();

  if (!replay.has_value()) {
    return std::nullopt;
  }

  const char *eventQuery =
      "SELECT action, lane, note_time_micros, song_time_micros,"
      "judge_time_micros, judgement, diff_micros, gauge, gauge_type, combo,"
      "score FROM replay_events WHERE replay_id = ? ORDER BY event_index";
  SqliteStatementHandle eventStmt;
  rc = prepareSqliteStatement(db, eventQuery, eventStmt);
  if (rc != SQLITE_OK) {
    SDL_Log("SQL error while preparing replay event load: %s",
            sqlite3_errmsg(db));
    return std::nullopt;
  }
  sqlite3_bind_int(eventStmt.get(), 1, replay->id);

  while (sqlite3_step(eventStmt.get()) == SQLITE_ROW) {
    ReplayEvent event;
    event.action = actionFromInt(sqlite3_column_int(eventStmt.get(), 0));
    event.lane = sqlite3_column_int(eventStmt.get(), 1);
    event.noteTimeMicros = sqlite3_column_int64(eventStmt.get(), 2);
    event.songTimeMicros = sqlite3_column_int64(eventStmt.get(), 3);
    event.judgeTimeMicros = sqlite3_column_int64(eventStmt.get(), 4);
    event.judgement = judgementFromInt(sqlite3_column_int(eventStmt.get(), 5));
    event.diffMicros = sqlite3_column_int64(eventStmt.get(), 6);
    event.gauge =
        static_cast<float>(sqlite3_column_double(eventStmt.get(), 7));
    event.gaugeType = gaugeTypeFromInt(sqlite3_column_int(eventStmt.get(), 8));
    event.combo = sqlite3_column_int(eventStmt.get(), 9);
    event.score = sqlite3_column_int(eventStmt.get(), 10);
    replay->events.push_back(event);
  }
  eventStmt.reset();

  const char *touchSampleQuery =
      "SELECT action, finger_id, song_time_micros, x, y "
      "FROM replay_touch_samples WHERE replay_id = ? ORDER BY sample_index";
  SqliteStatementHandle touchSampleStmt;
  rc = prepareSqliteStatement(db, touchSampleQuery, touchSampleStmt);
  if (rc != SQLITE_OK) {
    SDL_Log("SQL error while preparing replay touch sample load: %s",
            sqlite3_errmsg(db));
    return std::nullopt;
  }
  sqlite3_bind_int(touchSampleStmt.get(), 1, replay->id);

  while (sqlite3_step(touchSampleStmt.get()) == SQLITE_ROW) {
    ReplayTouchSample sample;
    sample.action =
        touchActionFromInt(sqlite3_column_int(touchSampleStmt.get(), 0));
    sample.fingerId = sqlite3_column_int64(touchSampleStmt.get(), 1);
    sample.songTimeMicros = sqlite3_column_int64(touchSampleStmt.get(), 2);
    sample.x =
        static_cast<float>(sqlite3_column_double(touchSampleStmt.get(), 3));
    sample.y =
        static_cast<float>(sqlite3_column_double(touchSampleStmt.get(), 4));
    replay->touchSamples.push_back(sample);
  }
  touchSampleStmt.reset();

  const char *laneCoverEventQuery =
      "SELECT song_time_micros, note_start_position_percent,"
      "reset_visible_time_reference "
      "FROM replay_lane_cover_events WHERE replay_id = ? "
      "ORDER BY event_index";
  SqliteStatementHandle laneCoverEventStmt;
  rc = prepareSqliteStatement(db, laneCoverEventQuery, laneCoverEventStmt);
  if (rc != SQLITE_OK) {
    SDL_Log("SQL error while preparing replay lane cover event load: %s",
            sqlite3_errmsg(db));
    return std::nullopt;
  }
  sqlite3_bind_int(laneCoverEventStmt.get(), 1, replay->id);

  while (sqlite3_step(laneCoverEventStmt.get()) == SQLITE_ROW) {
    ReplayLaneCoverEvent event;
    event.songTimeMicros = sqlite3_column_int64(laneCoverEventStmt.get(), 0);
    event.noteStartPositionPercent =
        sqlite3_column_int(laneCoverEventStmt.get(), 1);
    event.resetVisibleTimeReference =
        sqlite3_column_int(laneCoverEventStmt.get(), 2) != 0;
    replay->laneCoverEvents.push_back(event);
  }

  return replay;
}

std::optional<ReplayData>
ReplayDBHelper::LoadLatestReplay(const bms_parser::ChartMeta &chartMeta) {
  const auto replays = ListReplays(chartMeta, 1);
  if (replays.empty()) {
    return std::nullopt;
  }
  return LoadReplay(replays.front().id, chartMeta);
}
