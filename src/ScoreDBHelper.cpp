#include "ScoreDBHelper.h"

#include "Utils.h"
#include "path.h"
#include "targets.h"

#include <SDL2/SDL.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <string>

namespace {
std::atomic<std::uint64_t> gScoreRevision{1};

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

std::string normalizedPath(const std::string &value) {
  return trimCopy(value);
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

int judgeCount(const RhythmState &state, Judgement judgement) {
  const auto it = state.judgeCount.find(judgement);
  return it == state.judgeCount.end() ? 0 : it->second;
}

void storeBestRank(std::unordered_map<std::string, int> &ranks,
                   const std::string &key, int rank) {
  if (key.empty()) {
    return;
  }
  auto it = ranks.find(key);
  if (it == ranks.end() || rank > it->second) {
    ranks[key] = rank;
  }
}

void loadBestRanksForColumn(sqlite3 *db, const char *columnName,
                            std::unordered_map<std::string, int> &ranks,
                            bool hashColumn) {
  const std::string query =
      std::string("SELECT ") + columnName + ", MAX(clear_type) FROM scores "
      "WHERE " + columnName + " IS NOT NULL AND " + columnName +
      " != '' GROUP BY " + columnName;
  sqlite3_stmt *stmt = nullptr;
  const int rc = sqlite3_prepare_v2(db, query.c_str(), -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    SDL_Log("SQL error while loading score clear ranks: %s", sqlite3_errmsg(db));
    return;
  }

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const auto *text =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
    if (text == nullptr) {
      continue;
    }
    const std::string key =
        hashColumn ? normalizedHash(text) : normalizedPath(text);
    storeBestRank(ranks, key, sqlite3_column_int(stmt, 1));
  }
  sqlite3_finalize(stmt);
}
} // namespace

int ScoreClearRankCache::bestRankFor(
    const bms_parser::ChartMeta &chartMeta) const {
  const auto chartPath =
      path_t_to_utf8(fspath_to_path_t(toStoredChartPath(chartMeta.BmsPath)));
  return bestRankForHashes(chartMeta.SHA256, chartMeta.MD5, chartPath);
}

int ScoreClearRankCache::bestRankForHashes(const std::string &sha256,
                                           const std::string &md5,
                                           const std::string &path) const {
  const auto shaIt = rankBySha256.find(normalizedHash(sha256));
  if (shaIt != rankBySha256.end()) {
    return shaIt->second;
  }

  const auto md5It = rankByMd5.find(normalizedHash(md5));
  if (md5It != rankByMd5.end()) {
    return md5It->second;
  }

  const auto pathIt = rankByPath.find(normalizedPath(path));
  if (pathIt != rankByPath.end()) {
    return pathIt->second;
  }

  return kNoClearTypeRank;
}

ScoreDBHelper &ScoreDBHelper::GetInstance() {
  sqlite3_config(SQLITE_CONFIG_SERIALIZED);
  static ScoreDBHelper instance;
  return instance;
}

sqlite3 *ScoreDBHelper::Connect() {
  const std::filesystem::path directory = Utils::GetDocumentsPath("db");
  std::filesystem::create_directories(directory);
  const std::filesystem::path path = directory / "score.db";

  sqlite3 *db = nullptr;
  const int rc = sqlite3_open(path.string().c_str(), &db);
  if (db != nullptr) {
    sqlite3_busy_timeout(db, 1000);
  }
  if (rc != SQLITE_OK) {
    SDL_Log("Can't open score database: %s",
            db != nullptr ? sqlite3_errmsg(db) : "unknown error");
    if (db != nullptr) {
      sqlite3_close(db);
    }
    return nullptr;
  }

  sqlite3_exec(db, "PRAGMA journal_mode=WAL", nullptr, nullptr, nullptr);
  sqlite3_exec(db, "PRAGMA synchronous=NORMAL", nullptr, nullptr, nullptr);
  return db;
}

void ScoreDBHelper::Close(sqlite3 *db) {
  if (db != nullptr) {
    sqlite3_close(db);
  }
}

bool ScoreDBHelper::CreateScoreTable(sqlite3 *db) {
  const char *query =
      "CREATE TABLE IF NOT EXISTS scores ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "chart_path TEXT,"
      "chart_md5 TEXT,"
      "chart_sha256 TEXT,"
      "chart_title TEXT,"
      "chart_artist TEXT,"
      "score INTEGER NOT NULL,"
      "max_score INTEGER NOT NULL,"
      "max_combo INTEGER NOT NULL,"
      "combo_break INTEGER NOT NULL,"
      "pgreat INTEGER NOT NULL,"
      "great INTEGER NOT NULL,"
      "good INTEGER NOT NULL,"
      "bad INTEGER NOT NULL,"
      "poor INTEGER NOT NULL,"
      "kpoor INTEGER NOT NULL,"
      "fast INTEGER NOT NULL,"
      "slow INTEGER NOT NULL,"
      "final_gauge REAL NOT NULL,"
      "clear_type INTEGER NOT NULL,"
      "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP"
      ")";
  if (!execSql(db, query, "creating score table")) {
    return false;
  }

  const char *indexes[] = {
      "CREATE INDEX IF NOT EXISTS idx_scores_chart_sha256 ON scores(chart_sha256)",
      "CREATE INDEX IF NOT EXISTS idx_scores_chart_md5 ON scores(chart_md5)",
      "CREATE INDEX IF NOT EXISTS idx_scores_chart_path ON scores(chart_path)",
      "CREATE INDEX IF NOT EXISTS idx_scores_chart_sha256_clear_type ON "
      "scores(chart_sha256, clear_type)",
      "CREATE INDEX IF NOT EXISTS idx_scores_chart_md5_clear_type ON "
      "scores(chart_md5, clear_type)",
      "CREATE INDEX IF NOT EXISTS idx_scores_chart_path_clear_type ON "
      "scores(chart_path, clear_type)",
      "CREATE INDEX IF NOT EXISTS idx_scores_created_at ON scores(created_at)",
  };
  for (const auto *indexQuery : indexes) {
    if (!execSql(db, indexQuery, "creating score index")) {
      return false;
    }
  }
  return true;
}

bool ScoreDBHelper::InsertScore(sqlite3 *db,
                                const bms_parser::ChartMeta &chartMeta,
                                const RhythmState &state) {
  const char *query =
      "INSERT INTO scores ("
      "chart_path, chart_md5, chart_sha256, chart_title, chart_artist,"
      "score, max_score, max_combo, combo_break,"
      "pgreat, great, good, bad, poor, kpoor, fast, slow, final_gauge,"
      "clear_type"
      ") VALUES ("
      "@chart_path, @chart_md5, @chart_sha256, @chart_title, @chart_artist,"
      "@score, @max_score, @max_combo, @combo_break,"
      "@pgreat, @great, @good, @bad, @poor, @kpoor, @fast, @slow,"
      "@final_gauge, @clear_type"
      ")";

  sqlite3_stmt *stmt = nullptr;
  int rc = sqlite3_prepare_v2(db, query, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    SDL_Log("SQL error while preparing score insert: %s", sqlite3_errmsg(db));
    return false;
  }

  const auto chartPath = path_t_to_utf8(
      fspath_to_path_t(toStoredChartPath(chartMeta.BmsPath)));
  int bindIndex = 1;
  bindText(stmt, bindIndex++, chartPath);
  bindText(stmt, bindIndex++, chartMeta.MD5);
  bindText(stmt, bindIndex++, chartMeta.SHA256);
  bindText(stmt, bindIndex++, chartMeta.Title);
  bindText(stmt, bindIndex++, chartMeta.Artist);
  sqlite3_bind_int(stmt, bindIndex++, state.getScore());
  sqlite3_bind_int(stmt, bindIndex++, chartMeta.TotalNotes * 2);
  sqlite3_bind_int(stmt, bindIndex++, state.maxCombo);
  sqlite3_bind_int(stmt, bindIndex++, state.comboBreak);
  sqlite3_bind_int(stmt, bindIndex++, judgeCount(state, PGreat));
  sqlite3_bind_int(stmt, bindIndex++, judgeCount(state, Great));
  sqlite3_bind_int(stmt, bindIndex++, judgeCount(state, Good));
  sqlite3_bind_int(stmt, bindIndex++, judgeCount(state, Bad));
  sqlite3_bind_int(stmt, bindIndex++, judgeCount(state, Poor));
  sqlite3_bind_int(stmt, bindIndex++, judgeCount(state, Kpoor));
  sqlite3_bind_int(stmt, bindIndex++, state.fastCount);
  sqlite3_bind_int(stmt, bindIndex++, state.slowCount);
  sqlite3_bind_double(stmt, bindIndex++, state.currentGauge);
  sqlite3_bind_int(stmt, bindIndex++, state.getClearTypeRank());

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    SDL_Log("SQL error while saving score: %s", sqlite3_errmsg(db));
    return false;
  }
  return true;
}

bool ScoreDBHelper::SaveScore(const bms_parser::ChartMeta &chartMeta,
                              const RhythmState &state) {
  sqlite3 *db = Connect();
  if (db == nullptr) {
    return false;
  }

  const bool result = CreateScoreTable(db) && InsertScore(db, chartMeta, state);
  Close(db);
  if (result) {
    gScoreRevision.fetch_add(1, std::memory_order_relaxed);
  }
  return result;
}

ScoreClearRankCache ScoreDBHelper::LoadBestClearRanks() {
  ScoreClearRankCache cache;
  sqlite3 *db = Connect();
  if (db == nullptr) {
    return cache;
  }

  if (CreateScoreTable(db)) {
    loadBestRanksForColumn(db, "chart_sha256", cache.rankBySha256, true);
    loadBestRanksForColumn(db, "chart_md5", cache.rankByMd5, true);
    loadBestRanksForColumn(db, "chart_path", cache.rankByPath, false);
  }
  Close(db);
  return cache;
}

std::uint64_t ScoreDBHelper::GetRevision() const {
  return gScoreRevision.load(std::memory_order_relaxed);
}
