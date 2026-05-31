#include "ReplayDBHelper.h"

#include "Utils.h"
#include "path.h"
#include "targets.h"

#include <SDL2/SDL.h>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>

namespace {
std::filesystem::path toStoredChartPath(std::filesystem::path path) {
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  static const std::filesystem::path documents = Utils::GetDocumentsPath("BMS/");
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
  char *errMsg = nullptr;
  const int rc = sqlite3_exec(db, query, nullptr, nullptr, &errMsg);
  if (rc != SQLITE_OK) {
    SDL_Log("SQL error while %s: %s", context,
            errMsg != nullptr ? errMsg : sqlite3_errmsg(db));
    sqlite3_free(errMsg);
    return false;
  }
  return true;
}

bool bindText(sqlite3_stmt *stmt, int idx, const std::string &value) {
  return sqlite3_bind_text(stmt, idx, value.c_str(), -1, SQLITE_TRANSIENT) ==
         SQLITE_OK;
}

std::string readText(sqlite3_stmt *stmt, int idx) {
  const auto *text =
      reinterpret_cast<const char *>(sqlite3_column_text(stmt, idx));
  return text == nullptr ? "" : std::string(text);
}

ReplayEventAction actionFromInt(int value) {
  switch (value) {
  case 1:
    return ReplayEventAction::Release;
  case 2:
    return ReplayEventAction::Miss;
  case 0:
  default:
    return ReplayEventAction::Press;
  }
}

GaugeType gaugeTypeFromInt(int value) {
  if (value < 0 || value >= static_cast<int>(kGaugeTypeCount)) {
    return GaugeType::Normal;
  }
  return gaugeTypeAtIndex(value);
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
  const char *replayQuery =
      "CREATE TABLE IF NOT EXISTS replays ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "chart_path TEXT,"
      "chart_md5 TEXT,"
      "chart_sha256 TEXT,"
      "chart_title TEXT,"
      "chart_artist TEXT,"
      "gauge_type INTEGER NOT NULL,"
      "gauge_auto_shift INTEGER NOT NULL,"
      "final_score INTEGER NOT NULL,"
      "final_gauge REAL NOT NULL,"
      "clear_type INTEGER NOT NULL,"
      "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP"
      ")";
  if (!execSql(db, replayQuery, "creating replay table")) {
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

  const char *indexes[] = {
      "CREATE INDEX IF NOT EXISTS idx_replays_chart_sha256 ON "
      "replays(chart_sha256, id)",
      "CREATE INDEX IF NOT EXISTS idx_replays_chart_md5 ON "
      "replays(chart_md5, id)",
      "CREATE INDEX IF NOT EXISTS idx_replays_chart_path ON "
      "replays(chart_path, id)",
      "CREATE INDEX IF NOT EXISTS idx_replay_events_replay_order ON "
      "replay_events(replay_id, event_index)",
  };
  for (const auto *indexQuery : indexes) {
    if (!execSql(db, indexQuery, "creating replay index")) {
      return false;
    }
  }
  return true;
}

std::optional<int> ReplayDBHelper::SaveReplay(const ReplayData &replay) {
  sqlite3 *db = Connect();
  if (db == nullptr) {
    return std::nullopt;
  }

  if (!CreateReplayTables(db) ||
      !execSql(db, "BEGIN IMMEDIATE TRANSACTION", "starting replay save")) {
    Close(db);
    return std::nullopt;
  }

  const char *replayInsert =
      "INSERT INTO replays ("
      "chart_path, chart_md5, chart_sha256, chart_title, chart_artist,"
      "gauge_type, gauge_auto_shift, final_score, final_gauge, clear_type"
      ") VALUES ("
      "@chart_path, @chart_md5, @chart_sha256, @chart_title, @chart_artist,"
      "@gauge_type, @gauge_auto_shift, @final_score, @final_gauge, @clear_type"
      ")";

  sqlite3_stmt *replayStmt = nullptr;
  int rc = sqlite3_prepare_v2(db, replayInsert, -1, &replayStmt, nullptr);
  if (rc != SQLITE_OK) {
    SDL_Log("SQL error while preparing replay insert: %s", sqlite3_errmsg(db));
    execSql(db, "ROLLBACK", "rolling back replay save");
    Close(db);
    return std::nullopt;
  }

  const auto chartPath = path_t_to_utf8(
      fspath_to_path_t(toStoredChartPath(replay.chartMeta.BmsPath)));
  int bindIndex = 1;
  bindText(replayStmt, bindIndex++, chartPath);
  bindText(replayStmt, bindIndex++, replay.chartMeta.MD5);
  bindText(replayStmt, bindIndex++, replay.chartMeta.SHA256);
  bindText(replayStmt, bindIndex++, replay.chartMeta.Title);
  bindText(replayStmt, bindIndex++, replay.chartMeta.Artist);
  sqlite3_bind_int(replayStmt, bindIndex++,
                   gaugeTypeIndex(replay.initialGaugeType));
  sqlite3_bind_int(replayStmt, bindIndex++, replay.gaugeAutoShift ? 1 : 0);
  sqlite3_bind_int(replayStmt, bindIndex++, replay.finalScore);
  sqlite3_bind_double(replayStmt, bindIndex++, replay.finalGauge);
  sqlite3_bind_int(replayStmt, bindIndex++, replay.clearType);

  rc = sqlite3_step(replayStmt);
  sqlite3_finalize(replayStmt);
  if (rc != SQLITE_DONE) {
    SDL_Log("SQL error while saving replay: %s", sqlite3_errmsg(db));
    execSql(db, "ROLLBACK", "rolling back replay save");
    Close(db);
    return std::nullopt;
  }

  const int replayId = static_cast<int>(sqlite3_last_insert_rowid(db));
  const char *eventInsert =
      "INSERT INTO replay_events ("
      "replay_id, event_index, action, lane, note_time_micros,"
      "song_time_micros, judge_time_micros, judgement, diff_micros,"
      "gauge, gauge_type, combo, score"
      ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

  sqlite3_stmt *eventStmt = nullptr;
  rc = sqlite3_prepare_v2(db, eventInsert, -1, &eventStmt, nullptr);
  if (rc != SQLITE_OK) {
    SDL_Log("SQL error while preparing replay event insert: %s",
            sqlite3_errmsg(db));
    execSql(db, "ROLLBACK", "rolling back replay save");
    Close(db);
    return std::nullopt;
  }

  bool eventOk = true;
  for (size_t i = 0; i < replay.events.size(); ++i) {
    if (!insertReplayEvent(eventStmt, replayId, static_cast<int>(i),
                           replay.events[i])) {
      SDL_Log("SQL error while saving replay event: %s", sqlite3_errmsg(db));
      eventOk = false;
      break;
    }
  }
  sqlite3_finalize(eventStmt);

  if (!eventOk || !execSql(db, "COMMIT", "committing replay save")) {
    execSql(db, "ROLLBACK", "rolling back replay save");
    Close(db);
    return std::nullopt;
  }

  Close(db);
  return replayId;
}

std::optional<ReplayData>
ReplayDBHelper::LoadLatestReplay(const bms_parser::ChartMeta &chartMeta) {
  sqlite3 *db = Connect();
  if (db == nullptr) {
    return std::nullopt;
  }

  if (!CreateReplayTables(db)) {
    Close(db);
    return std::nullopt;
  }

  const std::string chartPath = path_t_to_utf8(
      fspath_to_path_t(toStoredChartPath(chartMeta.BmsPath)));
  const std::string sha256 = normalizedHash(chartMeta.SHA256);
  const std::string md5 = normalizedHash(chartMeta.MD5);

  const char *query =
      "SELECT id, chart_path, chart_md5, chart_sha256, chart_title,"
      "chart_artist, gauge_type, gauge_auto_shift, final_score, final_gauge,"
      "clear_type, created_at "
      "FROM replays WHERE "
      "((? != '' AND chart_sha256 = ?) OR "
      "(? != '' AND chart_md5 = ?) OR "
      "(? != '' AND chart_path = ?)) "
      "ORDER BY id DESC LIMIT 1";

  sqlite3_stmt *stmt = nullptr;
  int rc = sqlite3_prepare_v2(db, query, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    SDL_Log("SQL error while preparing replay load: %s", sqlite3_errmsg(db));
    Close(db);
    return std::nullopt;
  }

  int bindIndex = 1;
  bindText(stmt, bindIndex++, sha256);
  bindText(stmt, bindIndex++, sha256);
  bindText(stmt, bindIndex++, md5);
  bindText(stmt, bindIndex++, md5);
  bindText(stmt, bindIndex++, chartPath);
  bindText(stmt, bindIndex++, chartPath);

  std::optional<ReplayData> replay;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    ReplayData loaded;
    loaded.id = sqlite3_column_int(stmt, 0);
    loaded.chartMeta = chartMeta;
    loaded.chartMeta.BmsPath = readText(stmt, 1);
    loaded.chartMeta.MD5 = readText(stmt, 2);
    loaded.chartMeta.SHA256 = readText(stmt, 3);
    loaded.chartMeta.Title = readText(stmt, 4);
    loaded.chartMeta.Artist = readText(stmt, 5);
    loaded.initialGaugeType = gaugeTypeFromInt(sqlite3_column_int(stmt, 6));
    loaded.gaugeAutoShift = sqlite3_column_int(stmt, 7) != 0;
    loaded.finalScore = sqlite3_column_int(stmt, 8);
    loaded.finalGauge = static_cast<float>(sqlite3_column_double(stmt, 9));
    loaded.clearType = sqlite3_column_int(stmt, 10);
    loaded.createdAt = readText(stmt, 11);
    replay = std::move(loaded);
  }
  sqlite3_finalize(stmt);

  if (!replay.has_value()) {
    Close(db);
    return std::nullopt;
  }

  const char *eventQuery =
      "SELECT action, lane, note_time_micros, song_time_micros,"
      "judge_time_micros, judgement, diff_micros, gauge, gauge_type, combo,"
      "score FROM replay_events WHERE replay_id = ? ORDER BY event_index";
  sqlite3_stmt *eventStmt = nullptr;
  rc = sqlite3_prepare_v2(db, eventQuery, -1, &eventStmt, nullptr);
  if (rc != SQLITE_OK) {
    SDL_Log("SQL error while preparing replay event load: %s",
            sqlite3_errmsg(db));
    Close(db);
    return std::nullopt;
  }
  sqlite3_bind_int(eventStmt, 1, replay->id);

  while (sqlite3_step(eventStmt) == SQLITE_ROW) {
    ReplayEvent event;
    event.action = actionFromInt(sqlite3_column_int(eventStmt, 0));
    event.lane = sqlite3_column_int(eventStmt, 1);
    event.noteTimeMicros = sqlite3_column_int64(eventStmt, 2);
    event.songTimeMicros = sqlite3_column_int64(eventStmt, 3);
    event.judgeTimeMicros = sqlite3_column_int64(eventStmt, 4);
    event.judgement = judgementFromInt(sqlite3_column_int(eventStmt, 5));
    event.diffMicros = sqlite3_column_int64(eventStmt, 6);
    event.gauge = static_cast<float>(sqlite3_column_double(eventStmt, 7));
    event.gaugeType = gaugeTypeFromInt(sqlite3_column_int(eventStmt, 8));
    event.combo = sqlite3_column_int(eventStmt, 9);
    event.score = sqlite3_column_int(eventStmt, 10);
    replay->events.push_back(event);
  }
  sqlite3_finalize(eventStmt);

  Close(db);
  return replay;
}
