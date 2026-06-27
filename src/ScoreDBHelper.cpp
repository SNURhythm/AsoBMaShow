#include "ScoreDBHelper.h"

#include "ChartDBHelper.h"
#include "CoursePlaySession.h"
#include "SqliteRAII.h"
#include "Utils.h"
#include "path.h"
#include "targets.h"

#include <SDL2/SDL.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace {
std::atomic<std::uint64_t> gScoreRevision{1};
constexpr int kScoreDatabaseSchemaVersion = 1;
constexpr const char *kScoreMigrationChartSchema = "score_migration_chart";
constexpr const char *kMaxSqlIntegerText = "9223372036854775807";

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

bool sqliteMessageContains(const char *message, const char *needle) {
  if (message == nullptr || needle == nullptr) {
    return false;
  }
  std::string lowerMessage = lowerCopy(message);
  std::string lowerNeedle = lowerCopy(needle);
  return lowerMessage.find(lowerNeedle) != std::string::npos;
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

bool execSqlAllowDuplicateColumn(sqlite3 *db, const char *query,
                                 const char *context) {
  SqliteErrorMessageHandle errMsg;
  const int rc = sqlite3_exec(db, query, nullptr, nullptr, errMsg.out());
  if (rc != SQLITE_OK &&
      !sqliteMessageContains(errMsg.get(), "duplicate column name")) {
    SDL_Log("SQL error while %s: %s", context,
            errMsg.get() != nullptr ? errMsg.get() : sqlite3_errmsg(db));
    return false;
  }
  return true;
}

int databaseUserVersion(sqlite3 *db) {
  SqliteStatementHandle stmt;
  const int rc = prepareSqliteStatement(db, "PRAGMA user_version", stmt);
  if (rc != SQLITE_OK) {
    SDL_Log("SQL error while reading score database version: %s",
            sqlite3_errmsg(db));
    return 0;
  }
  if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
    return 0;
  }
  return sqlite3_column_int(stmt.get(), 0);
}

bool setDatabaseUserVersion(sqlite3 *db, int version) {
  const std::string query =
      "PRAGMA user_version = " + std::to_string(std::max(0, version));
  return execSql(db, query.c_str(), "updating score database version");
}

bool sqliteTableExists(sqlite3 *db, const char *tableName, bool &exists,
                       const char *context) {
  exists = false;
  SqliteStatementHandle stmt;
  const int rc = prepareSqliteStatement(
      db,
      "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ? LIMIT 1",
      stmt);
  if (rc != SQLITE_OK) {
    SDL_Log("SQL error while %s: %s", context, sqlite3_errmsg(db));
    return false;
  }
  sqlite3_bind_text(stmt.get(), 1, tableName, -1, SQLITE_STATIC);
  const int stepRc = sqlite3_step(stmt.get());
  if (stepRc == SQLITE_ROW) {
    exists = true;
    return true;
  }
  if (stepRc == SQLITE_DONE) {
    return true;
  }
  SDL_Log("SQL error while %s: %s", context, sqlite3_errmsg(db));
  return false;
}

bool bindText(sqlite3_stmt *stmt, int idx, const std::string &value) {
  return sqlite3_bind_text(stmt, idx, value.c_str(), -1, SQLITE_TRANSIENT) ==
         SQLITE_OK;
}

int selectScalarInt(sqlite3 *db, const std::string &query, int fallback = 0) {
  SqliteStatementHandle stmt;
  const int rc = prepareSqliteStatement(db, query, stmt);
  if (rc != SQLITE_OK) {
    SDL_Log("SQL error while reading score migration value: %s",
            sqlite3_errmsg(db));
    return fallback;
  }
  if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
    return fallback;
  }
  return sqlite3_column_int(stmt.get(), 0);
}

int judgeCount(const RhythmState &state, Judgement judgement) {
  const auto it = state.judgeCount.find(judgement);
  return it == state.judgeCount.end() ? 0 : it->second;
}

std::string stableChartKey(const bms_parser::ChartMeta &chartMeta) {
  const std::string sha256 = normalizedHash(chartMeta.SHA256);
  if (!sha256.empty()) {
    return "sha256:" + sha256;
  }
  const std::string md5 = normalizedHash(chartMeta.MD5);
  if (!md5.empty()) {
    return "md5:" + md5;
  }
  return "path:" +
         path_t_to_utf8(fspath_to_path_t(toStoredChartPath(chartMeta.BmsPath)));
}

std::string courseKeyForSession(const CoursePlaySession &session) {
  std::ostringstream key;
  key << "course:" << session.courseName << "\n";
  key << "constraint:" << session.constraintJson << "\n";
  for (const auto &entry : session.entries) {
    key << stableChartKey(entry.meta) << "\n";
  }
  return key.str();
}

int normalizedScoreLongNoteMode(int lnMode) {
  return lnMode >= 1 && lnMode <= 3 ? lnMode : 0;
}

int scoreLongNoteModeForClearLampValues(int chartLongNoteMode,
                                        int totalLongNotes,
                                        int totalBackSpinNotes,
                                        int selectedLongNoteMode) {
  if (std::max(0, totalLongNotes) + std::max(0, totalBackSpinNotes) <= 0) {
    return 0;
  }
  const int forcedLongNoteMode = normalizedScoreLongNoteMode(chartLongNoteMode);
  if (forcedLongNoteMode > 0) {
    return forcedLongNoteMode;
  }
  return normalizedScoreLongNoteMode(selectedLongNoteMode);
}

void storeBestRank(ScoreRankMap &ranks, const std::string &key, int lnMode,
                   int rank) {
  if (key.empty()) {
    return;
  }
  auto it = ranks.find(key);
  if (it == ranks.end()) {
    it = ranks.emplace(key, ScoreRankByLongNoteMode{}).first;
  }
  const int mode = normalizedScoreLongNoteMode(lnMode);
  if (rank > it->second.ranks[static_cast<size_t>(mode)]) {
    it->second.ranks[static_cast<size_t>(mode)] = rank;
  }
}

void storeBestCourseRank(CourseScoreRankMap &ranks, const std::string &key,
                         int rank) {
  if (key.empty()) {
    return;
  }
  auto it = ranks.find(key);
  if (it == ranks.end() || rank > it->second) {
    ranks[key] = rank;
  }
}

void loadBestRanksForColumn(sqlite3 *db, const char *columnName,
                            ScoreRankMap &ranks, bool hashColumn) {
  const std::string clearMarkExpr =
      "MAX(CASE WHEN combo_break = 0 AND clear_type >= " +
      std::to_string(kClearTypeAssistedEasyClearRank) + " THEN " +
      std::to_string(kClearTypeFullComboRank) +
      " ELSE clear_type END)";
  const std::string query =
      std::string("SELECT ") + columnName + ", ln_mode, " + clearMarkExpr +
      " FROM scores WHERE " + columnName + " IS NOT NULL AND " + columnName +
      " != '' GROUP BY " + columnName + ", ln_mode";
  SqliteStatementHandle stmt;
  const int rc = prepareSqliteStatement(db, query, stmt);
  if (rc != SQLITE_OK) {
    SDL_Log("SQL error while loading score clear ranks: %s", sqlite3_errmsg(db));
    return;
  }

  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    const auto *text =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt.get(), 0));
    if (text == nullptr) {
      continue;
    }
    const std::string key =
        hashColumn ? normalizedHash(text) : normalizedPath(text);
    storeBestRank(ranks, key, sqlite3_column_int(stmt.get(), 1),
                  sqlite3_column_int(stmt.get(), 2));
  }
}

void loadBestCourseRanks(sqlite3 *db, CourseScoreRankMap &ranks) {
  const std::string clearMarkExpr =
      "MAX(CASE WHEN combo_break = 0 AND clear_type >= " +
      std::to_string(kClearTypeAssistedEasyClearRank) + " THEN " +
      std::to_string(kClearTypeFullComboRank) +
      " ELSE clear_type END)";
  const std::string query =
      "SELECT course_id, " + clearMarkExpr +
      " FROM course_scores WHERE course_id IS NOT NULL AND course_id > 0 "
      "GROUP BY course_id";
  SqliteStatementHandle stmt;
  const int rc = prepareSqliteStatement(db, query, stmt);
  if (rc != SQLITE_OK) {
    SDL_Log("SQL error while loading course score clear ranks: %s",
            sqlite3_errmsg(db));
    return;
  }

  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    const int courseId = sqlite3_column_int(stmt.get(), 0);
    const int rank = sqlite3_column_int(stmt.get(), 1);
    storeBestCourseRank(ranks, std::to_string(courseId), rank);
  }
}

bool ensureChartDatabaseReadyForScoreMigration() {
  ChartDBHelper &chartDbHelper = ChartDBHelper::GetInstance();
  sqlite3 *chartDb = chartDbHelper.Connect();
  if (chartDb == nullptr) {
    return false;
  }
  SqliteConnectionHandle chartConnection(chartDb);
  return chartDbHelper.CreateChartMetaTable(chartConnection.get());
}

bool attachChartDatabaseForScoreMigration(sqlite3 *db) {
  const std::filesystem::path chartPath =
      Utils::GetDocumentsPath("db") / "chart.db";
  char *query = sqlite3_mprintf("ATTACH DATABASE %Q AS %s",
                                chartPath.string().c_str(),
                                kScoreMigrationChartSchema);
  if (query == nullptr) {
    SDL_Log("SQL error while preparing chart database attachment for score "
            "migration");
    return false;
  }
  SqliteErrorMessageHandle errMsg;
  const int rc = sqlite3_exec(db, query, nullptr, nullptr, errMsg.out());
  sqlite3_free(query);
  if (rc != SQLITE_OK) {
    SDL_Log("SQL error while attaching chart database for score migration: %s",
            errMsg.get() != nullptr ? errMsg.get() : sqlite3_errmsg(db));
    return false;
  }
  return true;
}

void detachChartDatabaseForScoreMigration(sqlite3 *db) {
  const std::string query =
      std::string("DETACH DATABASE ") + kScoreMigrationChartSchema;
  execSql(db, query.c_str(), "detaching chart database after score migration");
}

bool chartDatabaseHasRowsForScoreMigration(sqlite3 *db) {
  const std::string query =
      std::string("SELECT COUNT(*) FROM ") + kScoreMigrationChartSchema +
      ".chart_meta";
  return selectScalarInt(db, query, 0) > 0;
}

bool chartDatabaseRebuildRequiredForScoreMigration(sqlite3 *db) {
  const std::string tableExistsQuery =
      std::string("SELECT 1 FROM ") + kScoreMigrationChartSchema +
      ".sqlite_master WHERE type = 'table' AND "
      "name = 'chart_meta_rebuild_state' LIMIT 1";
  if (selectScalarInt(db, tableExistsQuery, 0) <= 0) {
    return false;
  }
  const std::string requiredQuery =
      std::string("SELECT COALESCE(MAX(required), 0) FROM ") +
      kScoreMigrationChartSchema + ".chart_meta_rebuild_state";
  return selectScalarInt(db, requiredQuery, 0) > 0;
}

bool migrateLegacyScoreLongNoteModes(sqlite3 *db, bool &completed) {
  completed = false;
  if (!ensureChartDatabaseReadyForScoreMigration()) {
    return false;
  }
  if (!attachChartDatabaseForScoreMigration(db)) {
    return false;
  }

  const int scoreCount = selectScalarInt(db, "SELECT COUNT(*) FROM scores", 0);
  if (scoreCount > 0 && chartDatabaseRebuildRequiredForScoreMigration(db)) {
    detachChartDatabaseForScoreMigration(db);
    SDL_Log("Deferred score ln_mode migration because chart metadata is "
            "scheduled for rebuild");
    return true;
  }
  if (scoreCount > 0 && !chartDatabaseHasRowsForScoreMigration(db)) {
    detachChartDatabaseForScoreMigration(db);
    SDL_Log("Deferred score ln_mode migration because chart metadata is empty");
    return true;
  }

  const std::string chartTable =
      std::string(kScoreMigrationChartSchema) + ".chart_meta";
  const std::string effectiveModeExpr =
      "CASE WHEN COALESCE(cm.total_long_notes, 0) + "
      "COALESCE(cm.total_backspin_notes, 0) <= 0 THEN 0 "
      "WHEN COALESCE(cm.ln_mode, 0) BETWEEN 1 AND 3 THEN cm.ln_mode "
      "ELSE 1 END";
  const std::string matchPredicate =
      "((scores.chart_sha256 IS NOT NULL AND trim(scores.chart_sha256) != '' "
      "AND lower(trim(cm.sha256)) = lower(trim(scores.chart_sha256))) OR "
      "(scores.chart_md5 IS NOT NULL AND trim(scores.chart_md5) != '' "
      "AND lower(trim(cm.md5)) = lower(trim(scores.chart_md5))) OR "
      "(scores.chart_path IS NOT NULL AND scores.chart_path != '' "
      "AND cm.path = scores.chart_path))";
  const std::string matchOrder =
      "CASE WHEN scores.chart_sha256 IS NOT NULL AND "
      "trim(scores.chart_sha256) != '' AND "
      "lower(trim(cm.sha256)) = lower(trim(scores.chart_sha256)) THEN 0 "
      "WHEN scores.chart_md5 IS NOT NULL AND trim(scores.chart_md5) != '' AND "
      "lower(trim(cm.md5)) = lower(trim(scores.chart_md5)) THEN 1 "
      "WHEN scores.chart_path IS NOT NULL AND scores.chart_path != '' AND "
      "cm.path = scores.chart_path THEN 2 ELSE 3 END, "
      "COALESCE(cm.source_priority, 3), COALESCE(cm.source_archive_size, " +
      std::string(kMaxSqlIntegerText) + "), cm.path";
  const std::string updateQuery =
      "UPDATE scores SET ln_mode = COALESCE((SELECT " + effectiveModeExpr +
      " FROM " + chartTable + " cm WHERE " + matchPredicate + " ORDER BY " +
      matchOrder + " LIMIT 1), ln_mode) WHERE ln_mode = 0";

  bool ok = execSql(db, "SAVEPOINT score_lnmode_migration",
                    "starting score ln_mode migration");
  int changedRows = 0;
  if (ok) {
    ok = execSql(db, updateQuery.c_str(), "migrating score long note modes");
    if (ok) {
      changedRows = sqlite3_changes(db);
    }
  }
  if (ok) {
    ok = execSql(db, "RELEASE score_lnmode_migration",
                 "committing score ln_mode migration");
  } else {
    execSql(db, "ROLLBACK TO score_lnmode_migration",
            "rolling back score ln_mode migration");
    execSql(db, "RELEASE score_lnmode_migration",
            "releasing score ln_mode migration");
  }

  detachChartDatabaseForScoreMigration(db);
  if (ok && changedRows > 0) {
    gScoreRevision.fetch_add(1, std::memory_order_relaxed);
  }
  completed = ok;
  return ok;
}

class ScoreDatabaseMigrationPass {
public:
  using RunFunction = bool (*)(sqlite3 *, bool &completed);

  constexpr ScoreDatabaseMigrationPass(int targetVersion, const char *name,
                                       RunFunction run)
      : targetVersion_(targetVersion), name_(name), run_(run) {}

  int targetVersion() const { return targetVersion_; }
  const char *name() const { return name_; }

  bool run(sqlite3 *db, bool &completed) const { return run_(db, completed); }

private:
  int targetVersion_;
  const char *name_;
  RunFunction run_;
};

bool migrateScoreDatabaseToVersion1(sqlite3 *db, bool &completed) {
  completed = false;
  if (!execSqlAllowDuplicateColumn(
          db, "ALTER TABLE scores ADD COLUMN ln_mode INTEGER NOT NULL DEFAULT 0",
          "migrating score long note mode")) {
    return false;
  }
  return migrateLegacyScoreLongNoteModes(db, completed);
}

bool runScoreDatabaseMigrationPasses(
    sqlite3 *db, const ScoreDatabaseMigrationPass *passes,
    std::size_t passCount, int latestVersion) {
  int currentVersion = databaseUserVersion(db);
  if (currentVersion >= latestVersion) {
    return true;
  }

  for (std::size_t i = 0; i < passCount; ++i) {
    const ScoreDatabaseMigrationPass &pass = passes[i];
    if (currentVersion >= pass.targetVersion()) {
      continue;
    }

    bool completed = false;
    if (!pass.run(db, completed)) {
      SDL_Log("Score database migration failed for version %d (%s)",
              pass.targetVersion(), pass.name());
      return false;
    }
    if (!completed) {
      return true;
    }
    if (!setDatabaseUserVersion(db, pass.targetVersion())) {
      return false;
    }
    currentVersion = pass.targetVersion();
  }

  if (currentVersion < latestVersion) {
    SDL_Log("No score database migration pass reached version %d",
            latestVersion);
    return false;
  }
  return true;
}

bool migrateScoreDatabaseSchema(sqlite3 *db) {
  static constexpr ScoreDatabaseMigrationPass kMigrationPasses[] = {
      {1, "score long note modes", migrateScoreDatabaseToVersion1},
  };
  return runScoreDatabaseMigrationPasses(
      db, kMigrationPasses,
      sizeof(kMigrationPasses) / sizeof(kMigrationPasses[0]),
      kScoreDatabaseSchemaVersion);
}
} // namespace

std::size_t TransparentStringHash::operator()(std::string_view value) const
    noexcept {
  return std::hash<std::string_view>{}(value);
}

int ScoreRankByLongNoteMode::bestRankForMode(
    int lnMode, bool legacyLongNoteModeFallback) const {
  const int mode = normalizedScoreLongNoteMode(lnMode);
  const int rank = ranks[static_cast<size_t>(mode)];
  if (rank != kNoClearTypeRank || mode == 0 || !legacyLongNoteModeFallback) {
    return rank;
  }
  return ranks[0];
}

int scoreLongNoteModeForClearLamp(const bms_parser::ChartMeta &chartMeta,
                                  int selectedLongNoteMode) {
  return scoreLongNoteModeForClearLampValues(
      chartMeta.LnMode, chartMeta.TotalLongNotes, chartMeta.TotalBackSpinNotes,
      selectedLongNoteMode);
}

int scoreLongNoteModeForClearLamp(int chartLongNoteMode, int totalLongNotes,
                                  int totalBackSpinNotes,
                                  int selectedLongNoteMode) {
  return scoreLongNoteModeForClearLampValues(
      chartLongNoteMode, totalLongNotes, totalBackSpinNotes,
      selectedLongNoteMode);
}

int ScoreClearRankCache::bestRankFor(
    const bms_parser::ChartMeta &chartMeta, int selectedLongNoteMode) const {
  const auto chartPath =
      path_t_to_utf8(fspath_to_path_t(toStoredChartPath(chartMeta.BmsPath)));
  return bestRankForHashes(
      chartMeta.SHA256, chartMeta.MD5, chartPath,
      scoreLongNoteModeForClearLamp(chartMeta, selectedLongNoteMode));
}

int ScoreClearRankCache::bestRankForHashes(const std::string &sha256,
                                           const std::string &md5,
                                           const std::string &path,
                                           int longNoteMode) const {
  const std::string normalizedSha = normalizedHash(sha256);
  const auto shaIt = rankBySha256.find(normalizedSha);
  if (shaIt != rankBySha256.end()) {
    return shaIt->second.bestRankForMode(longNoteMode,
                                         legacyLongNoteModeFallback);
  }

  const std::string normalizedMd5 = normalizedHash(md5);
  const auto md5It = rankByMd5.find(normalizedMd5);
  if (md5It != rankByMd5.end()) {
    return md5It->second.bestRankForMode(longNoteMode,
                                         legacyLongNoteModeFallback);
  }

  const std::string normalizedChartPath = normalizedPath(path);
  const auto pathIt = rankByPath.find(normalizedChartPath);
  if (pathIt != rankByPath.end()) {
    return pathIt->second.bestRankForMode(longNoteMode,
                                          legacyLongNoteModeFallback);
  }

  return kNoClearTypeRank;
}

int ScoreClearRankCache::bestRankForStoredKeys(std::string_view sha256,
                                               std::string_view md5,
                                               std::string_view path,
                                               int longNoteMode) const {
  const auto shaIt = rankBySha256.find(sha256);
  if (shaIt != rankBySha256.end()) {
    return shaIt->second.bestRankForMode(longNoteMode,
                                         legacyLongNoteModeFallback);
  }

  const auto md5It = rankByMd5.find(md5);
  if (md5It != rankByMd5.end()) {
    return md5It->second.bestRankForMode(longNoteMode,
                                         legacyLongNoteModeFallback);
  }

  const auto pathIt = rankByPath.find(path);
  if (pathIt != rankByPath.end()) {
    return pathIt->second.bestRankForMode(longNoteMode,
                                          legacyLongNoteModeFallback);
  }

  return kNoClearTypeRank;
}

int ScoreClearRankCache::bestCourseRankForId(int courseId) const {
  if (courseId <= 0) {
    return kNoClearTypeRank;
  }
  const auto it = rankByCourseId.find(std::to_string(courseId));
  return it == rankByCourseId.end() ? kNoClearTypeRank : it->second;
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
  bool existingScoreTable = false;
  if (!sqliteTableExists(db, "scores", existingScoreTable,
                         "checking score table existence")) {
    return false;
  }

  const char *query =
      "CREATE TABLE IF NOT EXISTS scores ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "chart_path TEXT,"
      "chart_md5 TEXT,"
      "chart_sha256 TEXT,"
      "ln_mode INTEGER NOT NULL DEFAULT 0,"
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

  if (existingScoreTable) {
    if (!migrateScoreDatabaseSchema(db)) {
      return false;
    }
  } else if (!setDatabaseUserVersion(db, kScoreDatabaseSchemaVersion)) {
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
      "CREATE INDEX IF NOT EXISTS idx_scores_chart_sha256_ln_mode ON "
      "scores(chart_sha256, ln_mode)",
      "CREATE INDEX IF NOT EXISTS idx_scores_chart_md5_ln_mode ON "
      "scores(chart_md5, ln_mode)",
      "CREATE INDEX IF NOT EXISTS idx_scores_chart_path_ln_mode ON "
      "scores(chart_path, ln_mode)",
      "CREATE INDEX IF NOT EXISTS idx_scores_created_at ON scores(created_at)",
  };
  for (const auto *indexQuery : indexes) {
    if (!execSql(db, indexQuery, "creating score index")) {
      return false;
    }
  }
  return true;
}

bool ScoreDBHelper::CreateCourseScoreTable(sqlite3 *db) {
  const char *query =
      "CREATE TABLE IF NOT EXISTS course_scores ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "course_id INTEGER,"
      "course_key TEXT,"
      "course_name TEXT,"
      "course_group_name TEXT,"
      "constraint_json TEXT,"
      "gauge_type INTEGER NOT NULL,"
      "gauge_profile INTEGER NOT NULL,"
      "gauge_auto_shift INTEGER NOT NULL,"
      "play_option TEXT,"
      "assist_option TEXT,"
      "completed_charts INTEGER NOT NULL,"
      "total_charts INTEGER NOT NULL,"
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
  if (!execSql(db, query, "creating course score table")) {
    return false;
  }

  const char *indexes[] = {
      "CREATE INDEX IF NOT EXISTS idx_course_scores_course_id ON "
      "course_scores(course_id)",
      "CREATE INDEX IF NOT EXISTS idx_course_scores_course_key ON "
      "course_scores(course_key)",
      "CREATE INDEX IF NOT EXISTS idx_course_scores_clear_type ON "
      "course_scores(clear_type)",
      "CREATE INDEX IF NOT EXISTS idx_course_scores_created_at ON "
      "course_scores(created_at)",
  };
  for (const auto *indexQuery : indexes) {
    if (!execSql(db, indexQuery, "creating course score index")) {
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
      "chart_path, chart_md5, chart_sha256, ln_mode, chart_title, "
      "chart_artist,"
      "score, max_score, max_combo, combo_break,"
      "pgreat, great, good, bad, poor, kpoor, fast, slow, final_gauge,"
      "clear_type"
      ") VALUES ("
      "@chart_path, @chart_md5, @chart_sha256, @ln_mode, @chart_title, "
      "@chart_artist,"
      "@score, @max_score, @max_combo, @combo_break,"
      "@pgreat, @great, @good, @bad, @poor, @kpoor, @fast, @slow,"
      "@final_gauge, @clear_type"
      ")";

  SqliteStatementHandle stmt;
  int rc = prepareSqliteStatement(db, query, stmt);
  if (rc != SQLITE_OK) {
    SDL_Log("SQL error while preparing score insert: %s", sqlite3_errmsg(db));
    return false;
  }

  const auto chartPath = path_t_to_utf8(
      fspath_to_path_t(toStoredChartPath(chartMeta.BmsPath)));
  int bindIndex = 1;
  bindText(stmt.get(), bindIndex++, chartPath);
  bindText(stmt.get(), bindIndex++, chartMeta.MD5);
  bindText(stmt.get(), bindIndex++, chartMeta.SHA256);
  sqlite3_bind_int(stmt.get(), bindIndex++,
                   scoreLongNoteModeForClearLamp(chartMeta));
  bindText(stmt.get(), bindIndex++, chartMeta.Title);
  bindText(stmt.get(), bindIndex++, chartMeta.Artist);
  sqlite3_bind_int(stmt.get(), bindIndex++, state.getScore());
  sqlite3_bind_int(stmt.get(), bindIndex++, chartMeta.TotalNotes * 2);
  sqlite3_bind_int(stmt.get(), bindIndex++, state.maxCombo);
  sqlite3_bind_int(stmt.get(), bindIndex++, state.comboBreak);
  sqlite3_bind_int(stmt.get(), bindIndex++, judgeCount(state, PGreat));
  sqlite3_bind_int(stmt.get(), bindIndex++, judgeCount(state, Great));
  sqlite3_bind_int(stmt.get(), bindIndex++, judgeCount(state, Good));
  sqlite3_bind_int(stmt.get(), bindIndex++, judgeCount(state, Bad));
  sqlite3_bind_int(stmt.get(), bindIndex++, judgeCount(state, Poor));
  sqlite3_bind_int(stmt.get(), bindIndex++, judgeCount(state, Kpoor));
  sqlite3_bind_int(stmt.get(), bindIndex++, state.fastCount);
  sqlite3_bind_int(stmt.get(), bindIndex++, state.slowCount);
  sqlite3_bind_double(stmt.get(), bindIndex++, state.currentGauge);
  sqlite3_bind_int(stmt.get(), bindIndex++, state.getClearTypeRank());

  rc = sqlite3_step(stmt.get());
  if (rc != SQLITE_DONE) {
    SDL_Log("SQL error while saving score: %s", sqlite3_errmsg(db));
    return false;
  }
  return true;
}

bool ScoreDBHelper::InsertCourseScore(sqlite3 *db,
                                      const CoursePlaySession &session,
                                      const RhythmState &state,
                                      int completedCharts, int totalCharts) {
  const char *query =
      "INSERT INTO course_scores ("
      "course_id, course_key, course_name, course_group_name, constraint_json,"
      "gauge_type, gauge_profile, gauge_auto_shift, play_option, assist_option,"
      "completed_charts, total_charts,"
      "score, max_score, max_combo, combo_break,"
      "pgreat, great, good, bad, poor, kpoor, fast, slow, final_gauge,"
      "clear_type"
      ") VALUES ("
      "@course_id, @course_key, @course_name, @course_group_name,"
      "@constraint_json, @gauge_type, @gauge_profile, @gauge_auto_shift,"
      "@play_option, @assist_option, @completed_charts, @total_charts,"
      "@score, @max_score, @max_combo, @combo_break,"
      "@pgreat, @great, @good, @bad, @poor, @kpoor, @fast, @slow,"
      "@final_gauge, @clear_type"
      ")";

  SqliteStatementHandle stmt;
  int rc = prepareSqliteStatement(db, query, stmt);
  if (rc != SQLITE_OK) {
    SDL_Log("SQL error while preparing course score insert: %s",
            sqlite3_errmsg(db));
    return false;
  }

  int courseTotalNotes = 0;
  for (const auto &entry : session.entries) {
    courseTotalNotes += std::max(0, entry.meta.TotalNotes);
  }

  int bindIndex = 1;
  sqlite3_bind_int(stmt.get(), bindIndex++, session.courseId);
  bindText(stmt.get(), bindIndex++, courseKeyForSession(session));
  bindText(stmt.get(), bindIndex++, session.courseName);
  bindText(stmt.get(), bindIndex++, session.courseGroupName);
  bindText(stmt.get(), bindIndex++, session.constraintJson);
  sqlite3_bind_int(stmt.get(), bindIndex++,
                   gaugeTypeIndex(session.gaugeType));
  sqlite3_bind_int(stmt.get(), bindIndex++,
                   static_cast<int>(session.gaugeProfile));
  sqlite3_bind_int(stmt.get(), bindIndex++, session.gaugeAutoShift ? 1 : 0);
  bindText(stmt.get(), bindIndex++,
           session.playOption.value_or(session.requestedPlayOption));
  bindText(stmt.get(), bindIndex++, session.assistOption);
  sqlite3_bind_int(stmt.get(), bindIndex++, completedCharts);
  sqlite3_bind_int(stmt.get(), bindIndex++, totalCharts);
  sqlite3_bind_int(stmt.get(), bindIndex++, state.getScore());
  sqlite3_bind_int(stmt.get(), bindIndex++, courseTotalNotes * 2);
  sqlite3_bind_int(stmt.get(), bindIndex++, state.maxCombo);
  sqlite3_bind_int(stmt.get(), bindIndex++, state.comboBreak);
  sqlite3_bind_int(stmt.get(), bindIndex++, judgeCount(state, PGreat));
  sqlite3_bind_int(stmt.get(), bindIndex++, judgeCount(state, Great));
  sqlite3_bind_int(stmt.get(), bindIndex++, judgeCount(state, Good));
  sqlite3_bind_int(stmt.get(), bindIndex++, judgeCount(state, Bad));
  sqlite3_bind_int(stmt.get(), bindIndex++, judgeCount(state, Poor));
  sqlite3_bind_int(stmt.get(), bindIndex++, judgeCount(state, Kpoor));
  sqlite3_bind_int(stmt.get(), bindIndex++, state.fastCount);
  sqlite3_bind_int(stmt.get(), bindIndex++, state.slowCount);
  sqlite3_bind_double(stmt.get(), bindIndex++, state.currentGauge);
  int clearRank = state.getClearTypeRank();
  if (completedCharts == totalCharts && totalCharts > 0 &&
      state.currentGauge > 0.0f && state.comboBreak == 0 &&
      state.maxCombo >= courseTotalNotes) {
    clearRank = kClearTypeFullComboRank;
  }
  sqlite3_bind_int(stmt.get(), bindIndex++, clearRank);

  rc = sqlite3_step(stmt.get());
  if (rc != SQLITE_DONE) {
    SDL_Log("SQL error while saving course score: %s", sqlite3_errmsg(db));
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
  SqliteConnectionHandle connection(db);

  const bool result =
      CreateScoreTable(connection.get()) &&
      InsertScore(connection.get(), chartMeta, state);
  if (result) {
    gScoreRevision.fetch_add(1, std::memory_order_relaxed);
  }
  return result;
}

bool ScoreDBHelper::SaveCourseScore(const CoursePlaySession &session,
                                    const RhythmState &state,
                                    int completedCharts, int totalCharts) {
  sqlite3 *db = Connect();
  if (db == nullptr) {
    return false;
  }
  SqliteConnectionHandle connection(db);

  const bool result =
      CreateCourseScoreTable(connection.get()) &&
      InsertCourseScore(connection.get(), session, state, completedCharts,
                        totalCharts);
  if (result) {
    gScoreRevision.fetch_add(1, std::memory_order_relaxed);
  }
  return result;
}

std::optional<ScoreBestSnapshot> ScoreDBHelper::LoadBestScore(
    const bms_parser::ChartMeta &chartMeta,
    const std::optional<std::string> &beforeCreatedAt) {
  sqlite3 *db = Connect();
  if (db == nullptr) {
    return std::nullopt;
  }
  SqliteConnectionHandle connection(db);

  if (!CreateScoreTable(connection.get())) {
    return std::nullopt;
  }

  const auto chartPath = path_t_to_utf8(
      fspath_to_path_t(toStoredChartPath(chartMeta.BmsPath)));
  const std::string sha256 = normalizedHash(chartMeta.SHA256);
  const std::string md5 = normalizedHash(chartMeta.MD5);
  const std::string cutoff = beforeCreatedAt.value_or("");
  const int longNoteMode = scoreLongNoteModeForClearLamp(chartMeta);
  const bool legacyLongNoteModeFallback =
      databaseUserVersion(connection.get()) < kScoreDatabaseSchemaVersion;

  const char *query =
      "SELECT score, max_score, max_combo, combo_break, final_gauge,"
      "clear_type, created_at "
      "FROM scores "
      "WHERE ((? != '' AND lower(trim(chart_sha256)) = ?) OR "
      "(? != '' AND lower(trim(chart_md5)) = ?) OR "
      "(? != '' AND chart_path = ?)) "
      "AND (ln_mode = ? OR (? != 0 AND ln_mode = 0)) "
      "AND (? = '' OR created_at < ?) "
      "ORDER BY CASE WHEN ln_mode = ? THEN 0 ELSE 1 END, "
      "score DESC, clear_type DESC, created_at DESC, id DESC "
      "LIMIT 1";

  SqliteStatementHandle stmt;
  const int rc = prepareSqliteStatement(connection.get(), query, stmt);
  if (rc != SQLITE_OK) {
    SDL_Log("SQL error while loading best score: %s",
            sqlite3_errmsg(connection.get()));
    return std::nullopt;
  }

  int bindIndex = 1;
  bindText(stmt.get(), bindIndex++, sha256);
  bindText(stmt.get(), bindIndex++, sha256);
  bindText(stmt.get(), bindIndex++, md5);
  bindText(stmt.get(), bindIndex++, md5);
  bindText(stmt.get(), bindIndex++, chartPath);
  bindText(stmt.get(), bindIndex++, chartPath);
  sqlite3_bind_int(stmt.get(), bindIndex++, longNoteMode);
  sqlite3_bind_int(stmt.get(), bindIndex++,
                   legacyLongNoteModeFallback && longNoteMode > 0 ? 1 : 0);
  bindText(stmt.get(), bindIndex++, cutoff);
  bindText(stmt.get(), bindIndex++, cutoff);
  sqlite3_bind_int(stmt.get(), bindIndex++, longNoteMode);

  if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
    return std::nullopt;
  }

  ScoreBestSnapshot snapshot;
  snapshot.score = sqlite3_column_int(stmt.get(), 0);
  snapshot.maxScore = sqlite3_column_int(stmt.get(), 1);
  snapshot.maxCombo = sqlite3_column_int(stmt.get(), 2);
  snapshot.comboBreak = sqlite3_column_int(stmt.get(), 3);
  snapshot.finalGauge = static_cast<float>(sqlite3_column_double(stmt.get(), 4));
  snapshot.clearType = sqlite3_column_int(stmt.get(), 5);
  const auto *createdAt =
      reinterpret_cast<const char *>(sqlite3_column_text(stmt.get(), 6));
  snapshot.createdAt = createdAt != nullptr ? std::string(createdAt) : "";
  return snapshot;
}

std::optional<ScoreBestSnapshot>
ScoreDBHelper::LoadBestCourseScore(const CoursePlaySession &session) {
  sqlite3 *db = Connect();
  if (db == nullptr) {
    return std::nullopt;
  }
  SqliteConnectionHandle connection(db);

  if (!CreateCourseScoreTable(connection.get())) {
    return std::nullopt;
  }

  const std::string courseKey = courseKeyForSession(session);
  const char *query =
      "SELECT score, max_score, max_combo, combo_break, final_gauge,"
      "clear_type, created_at "
      "FROM course_scores "
      "WHERE ((? != '' AND course_key = ?) OR (? > 0 AND course_id = ?)) "
      "ORDER BY score DESC, clear_type DESC, created_at DESC, id DESC "
      "LIMIT 1";

  SqliteStatementHandle stmt;
  const int rc = prepareSqliteStatement(connection.get(), query, stmt);
  if (rc != SQLITE_OK) {
    SDL_Log("SQL error while loading best course score: %s",
            sqlite3_errmsg(connection.get()));
    return std::nullopt;
  }

  int bindIndex = 1;
  bindText(stmt.get(), bindIndex++, courseKey);
  bindText(stmt.get(), bindIndex++, courseKey);
  sqlite3_bind_int(stmt.get(), bindIndex++, session.courseId);
  sqlite3_bind_int(stmt.get(), bindIndex++, session.courseId);

  if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
    return std::nullopt;
  }

  ScoreBestSnapshot snapshot;
  snapshot.score = sqlite3_column_int(stmt.get(), 0);
  snapshot.maxScore = sqlite3_column_int(stmt.get(), 1);
  snapshot.maxCombo = sqlite3_column_int(stmt.get(), 2);
  snapshot.comboBreak = sqlite3_column_int(stmt.get(), 3);
  snapshot.finalGauge = static_cast<float>(sqlite3_column_double(stmt.get(), 4));
  snapshot.clearType = sqlite3_column_int(stmt.get(), 5);
  const auto *createdAt =
      reinterpret_cast<const char *>(sqlite3_column_text(stmt.get(), 6));
  snapshot.createdAt = createdAt != nullptr ? std::string(createdAt) : "";
  return snapshot;
}

ScoreClearRankCache ScoreDBHelper::LoadBestClearRanks() {
  ScoreClearRankCache cache;
  sqlite3 *db = Connect();
  if (db == nullptr) {
    return cache;
  }
  SqliteConnectionHandle connection(db);

  if (CreateScoreTable(connection.get())) {
    cache.legacyLongNoteModeFallback =
        databaseUserVersion(connection.get()) < kScoreDatabaseSchemaVersion;
    loadBestRanksForColumn(connection.get(), "chart_sha256",
                           cache.rankBySha256, true);
    loadBestRanksForColumn(connection.get(), "chart_md5", cache.rankByMd5,
                           true);
    loadBestRanksForColumn(connection.get(), "chart_path", cache.rankByPath,
                           false);
  }
  if (CreateCourseScoreTable(connection.get())) {
    loadBestCourseRanks(connection.get(), cache.rankByCourseId);
  }
  return cache;
}

std::uint64_t ScoreDBHelper::GetRevision() const {
  return gScoreRevision.load(std::memory_order_relaxed);
}
