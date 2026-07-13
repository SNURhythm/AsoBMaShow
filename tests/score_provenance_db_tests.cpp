#include "../src/ChartDBHelper.h"
#include "../src/CourseIdentity.h"
#include "../src/CoursePlaySession.h"
#include "../src/ReplayDBHelper.h"
#include "../src/ScoreCacheQueries.h"
#include "../src/ScoreDBHelper.h"
#include "../src/ScoreProvenance.h"
#include "../src/SqliteRAII.h"
#include "../src/targets.h"

#include <array>
#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#if !TARGET_OS_WINDOWS
#include <sys/wait.h>
#include <unistd.h>
#endif

// ScoreDBHelper's pre-v1 migration can consult ChartDBHelper. These v4/v5
// fixtures never take that path, so the focused target supplies only the
// three legacy symbols required by the linker.
ChartDBHelper::ChartDBHelper() = default;
sqlite3 *ChartDBHelper::Connect() { return nullptr; }
bool ChartDBHelper::CreateChartMetaTable(sqlite3 *) { return false; }

namespace {

constexpr const char *kLegacyProvenanceJson =
    "{\"schemaVersion\":1,\"ruleset\":{\"version\":0},\"stages\":[],"
    "\"eligibility\":\"legacy-unverified\"}";
constexpr const char *kAssistedProvenanceJson =
    "{\"schemaVersion\":1,\"ruleset\":{\"version\":0},\"stages\":[],"
    "\"playback\":{\"percent\":75,\"mode\":\"pitch-shift\"},"
    "\"eligibility\":\"modified\"}";

constexpr std::string_view kShaA =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr std::string_view kShaB =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
constexpr std::string_view kShaC =
    "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
constexpr std::string_view kShaD =
    "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";
constexpr std::string_view kMd5A = "11111111111111111111111111111111";
constexpr std::string_view kMd5B = "22222222222222222222222222222222";

void execOrAbort(sqlite3 *db, const std::string &sql) {
  char *error = nullptr;
  if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
    std::cerr << "exec failed: " << (error != nullptr ? error : "") << "\n"
              << sql << std::endl;
    sqlite3_free(error);
    std::abort();
  }
}

SqliteConnectionHandle openDatabase(const std::filesystem::path &path) {
  std::filesystem::create_directories(path.parent_path());
  sqlite3 *db = nullptr;
  assert(sqlite3_open(path.string().c_str(), &db) == SQLITE_OK);
  return SqliteConnectionHandle(db);
}

int queryInt(sqlite3 *db, const std::string &sql) {
  SqliteStatementHandle stmt;
  assert(prepareSqliteStatement(db, sql, stmt) == SQLITE_OK);
  assert(sqlite3_step(stmt.get()) == SQLITE_ROW);
  return sqlite3_column_int(stmt.get(), 0);
}

std::string queryText(sqlite3 *db, const std::string &sql) {
  SqliteStatementHandle stmt;
  assert(prepareSqliteStatement(db, sql, stmt) == SQLITE_OK);
  assert(sqlite3_step(stmt.get()) == SQLITE_ROW);
  const auto *text = sqlite3_column_text(stmt.get(), 0);
  return text == nullptr ? std::string{}
                         : std::string(reinterpret_cast<const char *>(text));
}

bool tableExists(sqlite3 *db, const std::string &table) {
  SqliteStatementHandle stmt;
  assert(prepareSqliteStatement(
             db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?",
             stmt) == SQLITE_OK);
  sqlite3_bind_text(stmt.get(), 1, table.c_str(), -1, SQLITE_TRANSIENT);
  return sqlite3_step(stmt.get()) == SQLITE_ROW;
}

bool columnExists(sqlite3 *db, const std::string &table,
                  const std::string &column) {
  SqliteStatementHandle stmt;
  const std::string sql = "PRAGMA table_info(\"" + table + "\")";
  assert(prepareSqliteStatement(db, sql, stmt) == SQLITE_OK);
  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    const auto *name = sqlite3_column_text(stmt.get(), 1);
    if (name != nullptr && column == reinterpret_cast<const char *>(name)) {
      return true;
    }
  }
  return false;
}

bool indexExists(sqlite3 *db, const std::string &index) {
  SqliteStatementHandle stmt;
  assert(prepareSqliteStatement(
             db, "SELECT 1 FROM sqlite_master WHERE type='index' AND name=?",
             stmt) == SQLITE_OK);
  sqlite3_bind_text(stmt.get(), 1, index.c_str(), -1, SQLITE_TRANSIENT);
  return sqlite3_step(stmt.get()) == SQLITE_ROW;
}

bool indexIsUniqueAndPartial(sqlite3 *db, const std::string &table,
                             const std::string &index) {
  SqliteStatementHandle stmt;
  const std::string sql = "PRAGMA index_list(\"" + table + "\")";
  assert(prepareSqliteStatement(db, sql, stmt) == SQLITE_OK);
  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    const auto *name = sqlite3_column_text(stmt.get(), 1);
    if (name != nullptr && index == reinterpret_cast<const char *>(name)) {
      return sqlite3_column_type(stmt.get(), 2) == SQLITE_INTEGER &&
             sqlite3_column_int(stmt.get(), 2) == 1 &&
             sqlite3_column_type(stmt.get(), 4) == SQLITE_INTEGER &&
             sqlite3_column_int(stmt.get(), 4) == 1;
    }
  }
  return false;
}

std::string
legacyCourseKey(std::string_view name, std::string_view constraintJson,
                std::span<const course_identity::ChartIdentity> charts) {
  std::string key = "course:" + std::string(name) +
                    "\nconstraint:" + std::string(constraintJson) + "\n";
  for (const auto &chart : charts) {
    if (!chart.sha256.empty()) {
      key += "sha256:" + chart.sha256 + "\n";
    } else {
      key += "md5:" + chart.md5 + "\n";
    }
  }
  return key;
}

void insertCourseScoreRow(sqlite3 *db, int courseId, std::string_view courseKey,
                          std::string_view courseName,
                          std::string_view groupName,
                          std::string_view constraintJson, int totalCharts,
                          int score, int comboBreak, int clearType,
                          std::optional<int> lnMode = std::nullopt) {
  const std::string lnColumn = lnMode.has_value() ? ", ln_mode" : "";
  const std::string lnValue = lnMode.has_value() ? ", ?10" : "";
  const std::string sql =
      "INSERT INTO course_scores ("
      "course_id, course_key, course_name, course_group_name, constraint_json,"
      "gauge_type, gauge_profile, gauge_auto_shift, completed_charts,"
      "total_charts, score, max_score, max_combo, combo_break, pgreat, great,"
      "good, bad, poor, kpoor, fast, slow, final_gauge, clear_type,"
      "ruleset_version, eligibility, provenance_json" +
      lnColumn +
      ") VALUES (?1, ?2, ?3, ?4, ?5, 0, 0, 0, ?6, ?6, ?7, ?8, 0, ?9,"
      "0, 0, 0, 0, 0, 0, 0, 0, 50.0, " +
      std::to_string(clearType) + ", 0, 2, '" + kLegacyProvenanceJson + "'" +
      lnValue + ")";
  SqliteStatementHandle stmt;
  assert(prepareSqliteStatement(db, sql, stmt) == SQLITE_OK);
  sqlite3_bind_int(stmt.get(), 1, courseId);
  sqlite3_bind_text(stmt.get(), 2, courseKey.data(),
                    static_cast<int>(courseKey.size()), SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt.get(), 3, courseName.data(),
                    static_cast<int>(courseName.size()), SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt.get(), 4, groupName.data(),
                    static_cast<int>(groupName.size()), SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt.get(), 5, constraintJson.data(),
                    static_cast<int>(constraintJson.size()), SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt.get(), 6, totalCharts);
  sqlite3_bind_int(stmt.get(), 7, score);
  sqlite3_bind_int(stmt.get(), 8, score * 2);
  sqlite3_bind_int(stmt.get(), 9, comboBreak);
  if (lnMode.has_value()) {
    sqlite3_bind_int(stmt.get(), 10, *lnMode);
  }
  assert(sqlite3_step(stmt.get()) == SQLITE_DONE);
}

std::string schemaSnapshot(sqlite3 *db) {
  SqliteStatementHandle stmt;
  assert(prepareSqliteStatement(
             db,
             "SELECT type || ':' || name || ':' || COALESCE(sql, '') "
             "FROM sqlite_master ORDER BY type, name",
             stmt) == SQLITE_OK);
  std::string result;
  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    const auto *text = sqlite3_column_text(stmt.get(), 0);
    if (text != nullptr) {
      result += reinterpret_cast<const char *>(text);
    }
    result.push_back('\n');
  }
  return result;
}

std::string readFileBytes(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

struct PersistentDatabaseSnapshot {
  int userVersion = 0;
  std::string journalMode;
  std::string schema;
  std::string sentinel;
  std::string bytes;
  bool journalSidecar = false;
  bool walSidecar = false;
  bool shmSidecar = false;

  bool operator==(const PersistentDatabaseSnapshot &) const = default;
};

PersistentDatabaseSnapshot
persistentDatabaseSnapshot(const std::filesystem::path &path) {
  auto db = openDatabase(path);
  PersistentDatabaseSnapshot result;
  result.userVersion = queryInt(db.get(), "PRAGMA user_version");
  result.journalMode = queryText(db.get(), "PRAGMA journal_mode");
  result.schema = schemaSnapshot(db.get());
  result.sentinel = queryText(db.get(), "SELECT value FROM sentinel");
  db.reset();
  result.bytes = readFileBytes(path);
  result.journalSidecar = std::filesystem::exists(path.string() + "-journal");
  result.walSidecar = std::filesystem::exists(path.string() + "-wal");
  result.shmSidecar = std::filesystem::exists(path.string() + "-shm");
  return result;
}

void createFutureSentinelDatabase(const std::filesystem::path &path) {
  auto db = openDatabase(path);
  execOrAbort(db.get(), "CREATE TABLE sentinel(value TEXT)");
  execOrAbort(db.get(), "INSERT INTO sentinel VALUES ('unchanged')");
  execOrAbort(db.get(), "PRAGMA user_version = 99");
}

struct RawDatabaseFamilySnapshot {
  std::array<std::optional<std::string>, 4> files;

  bool operator==(const RawDatabaseFamilySnapshot &) const = default;
};

RawDatabaseFamilySnapshot
rawDatabaseFamilySnapshot(const std::filesystem::path &path) {
  constexpr std::array<const char *, 4> suffixes = {"", "-journal", "-wal",
                                                    "-shm"};
  RawDatabaseFamilySnapshot snapshot;
  for (std::size_t i = 0; i < suffixes.size(); ++i) {
    const std::filesystem::path familyPath = path.string() + suffixes[i];
    std::error_code existsError;
    const bool exists = std::filesystem::exists(familyPath, existsError);
    assert(!existsError);
    if (exists) {
      snapshot.files[i] = readFileBytes(familyPath);
    }
  }
  return snapshot;
}

bool sameDatabaseFamilyIgnoringWalSharedMemoryBytes(
    const RawDatabaseFamilySnapshot &left,
    const RawDatabaseFamilySnapshot &right) {
  return left.files[0] == right.files[0] && left.files[1] == right.files[1] &&
         left.files[2] == right.files[2] &&
         left.files[3].has_value() == right.files[3].has_value();
}

enum class FutureDatabaseState { Delete, WalAfterExit, HotJournal };

const char *futureDatabaseStateName(FutureDatabaseState state) {
  switch (state) {
  case FutureDatabaseState::Delete:
    return "delete";
  case FutureDatabaseState::WalAfterExit:
    return "wal-after-exit";
  case FutureDatabaseState::HotJournal:
    return "hot-journal";
  }
  return "unknown";
}

#if !TARGET_OS_WINDOWS
[[noreturn]] void executeChildSqlAndExit(const std::filesystem::path &path,
                                         const char *sql,
                                         bool flushCache = false) {
  sqlite3 *db = nullptr;
  if (sqlite3_open(path.string().c_str(), &db) != SQLITE_OK || db == nullptr) {
    _exit(10);
  }
  char *error = nullptr;
  if (sqlite3_exec(db, sql, nullptr, nullptr, &error) != SQLITE_OK) {
    sqlite3_free(error);
    _exit(11);
  }
  if (flushCache && sqlite3_db_cacheflush(db) != SQLITE_OK) {
    _exit(12);
  }
  _exit(0);
}

void waitForSuccessfulChild(pid_t child) {
  int status = 0;
  assert(waitpid(child, &status, 0) == child);
  assert(WIFEXITED(status));
  assert(WEXITSTATUS(status) == 0);
}

void createFutureDatabaseState(const std::filesystem::path &path,
                               FutureDatabaseState state) {
  std::filesystem::create_directories(path.parent_path());
  if (state == FutureDatabaseState::Delete) {
    createFutureSentinelDatabase(path);
    return;
  }
  if (state == FutureDatabaseState::HotJournal) {
    createFutureSentinelDatabase(path);
  }

  const pid_t child = fork();
  assert(child >= 0);
  if (child == 0) {
    if (state == FutureDatabaseState::WalAfterExit) {
      executeChildSqlAndExit(
          path, "PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0;"
                "CREATE TABLE sentinel(value TEXT);"
                "INSERT INTO sentinel VALUES ('unchanged');"
                "PRAGMA user_version=99;");
    }
    executeChildSqlAndExit(
        path,
        "PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL;"
        "PRAGMA cache_size=1; BEGIN IMMEDIATE; PRAGMA user_version=98;"
        "CREATE TABLE uncommitted(payload BLOB);"
        "WITH RECURSIVE counter(value) AS (VALUES(1) UNION ALL "
        "SELECT value + 1 FROM counter WHERE value < 200) "
        "INSERT INTO uncommitted SELECT randomblob(4096) FROM counter;",
        true);
  }
  waitForSuccessfulChild(child);

  if (state == FutureDatabaseState::WalAfterExit) {
    assert(std::filesystem::exists(path.string() + "-wal"));
    assert(std::filesystem::exists(path.string() + "-shm"));
  } else {
    assert(std::filesystem::exists(path.string() + "-journal"));
    assert(std::filesystem::file_size(path.string() + "-journal") > 0);
  }
}

void createWalDatabaseWithVersion(const std::filesystem::path &path,
                                  int userVersion) {
  std::filesystem::create_directories(path.parent_path());
  const pid_t child = fork();
  assert(child >= 0);
  if (child == 0) {
    const std::string sql =
        "PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0;"
        "CREATE TABLE sentinel(value TEXT);"
        "INSERT INTO sentinel VALUES('unchanged');"
        "PRAGMA user_version=" +
        std::to_string(userVersion);
    executeChildSqlAndExit(path, sql.c_str());
  }
  waitForSuccessfulChild(child);
  assert(std::filesystem::exists(path.string() + "-wal"));
  assert(std::filesystem::exists(path.string() + "-shm"));
}

void createLargeFutureWalDatabase(const std::filesystem::path &path) {
  {
    auto db = openDatabase(path);
    execOrAbort(db.get(), "CREATE TABLE padding(payload BLOB)");
    execOrAbort(db.get(),
                "WITH RECURSIVE counter(value) AS (VALUES(1) UNION ALL "
                "SELECT value + 1 FROM counter WHERE value < 512) "
                "INSERT INTO padding SELECT randomblob(4096) FROM counter");
  }
  assert(std::filesystem::file_size(path) > 2 * 1024 * 1024);

  const pid_t child = fork();
  assert(child >= 0);
  if (child == 0) {
    executeChildSqlAndExit(
        path, "PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0;"
              "PRAGMA user_version=99;");
  }
  waitForSuccessfulChild(child);
  assert(std::filesystem::exists(path.string() + "-wal"));
}

template <typename Operation>
void withLiveWalWriter(const std::filesystem::path &path, int userVersion,
                       const Operation &operation) {
  std::filesystem::create_directories(path.parent_path());
  int readyPipe[2] = {-1, -1};
  int releasePipe[2] = {-1, -1};
  assert(pipe(readyPipe) == 0);
  assert(pipe(releasePipe) == 0);
  const pid_t child = fork();
  assert(child >= 0);
  if (child == 0) {
    close(readyPipe[0]);
    close(releasePipe[1]);
    sqlite3 *db = nullptr;
    if (sqlite3_open(path.string().c_str(), &db) != SQLITE_OK ||
        db == nullptr) {
      _exit(20);
    }
    const std::string setup =
        "PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0;"
        "CREATE TABLE sentinel(value TEXT);"
        "INSERT INTO sentinel VALUES ('unchanged');"
        "PRAGMA user_version=" +
        std::to_string(userVersion) +
        "; BEGIN IMMEDIATE; UPDATE sentinel SET value='uncommitted';";
    char *error = nullptr;
    if (sqlite3_exec(db, setup.c_str(), nullptr, nullptr, &error) !=
        SQLITE_OK) {
      sqlite3_free(error);
      _exit(21);
    }
    const char ready = 'R';
    if (write(readyPipe[1], &ready, 1) != 1) {
      _exit(22);
    }
    char release = 0;
    if (read(releasePipe[0], &release, 1) != 1) {
      _exit(23);
    }
    _exit(0);
  }

  close(readyPipe[1]);
  close(releasePipe[0]);
  char ready = 0;
  assert(read(readyPipe[0], &ready, 1) == 1);
  assert(ready == 'R');
  close(readyPipe[0]);
  operation();
  const char release = 'X';
  assert(write(releasePipe[1], &release, 1) == 1);
  close(releasePipe[1]);
  waitForSuccessfulChild(child);
}
#endif

void createVersion4ScoreFixture(const std::filesystem::path &path) {
  auto db = openDatabase(path);
  execOrAbort(
      db.get(),
      "CREATE TABLE scores ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "chart_path TEXT, chart_md5 TEXT, chart_sha256 TEXT NOT NULL,"
      "ln_mode INTEGER NOT NULL DEFAULT 0, chart_title TEXT,"
      "chart_artist TEXT, score INTEGER NOT NULL, max_score INTEGER NOT NULL,"
      "max_combo INTEGER NOT NULL, combo_break INTEGER NOT NULL,"
      "pgreat INTEGER NOT NULL, great INTEGER NOT NULL, good INTEGER NOT NULL,"
      "bad INTEGER NOT NULL, poor INTEGER NOT NULL, kpoor INTEGER NOT NULL,"
      "fast INTEGER NOT NULL, slow INTEGER NOT NULL, final_gauge REAL NOT NULL,"
      "clear_type INTEGER NOT NULL,"
      "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP)");
  execOrAbort(
      db.get(),
      "CREATE TABLE course_scores ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT, course_id INTEGER,"
      "course_key TEXT, course_name TEXT, course_group_name TEXT,"
      "constraint_json TEXT, gauge_type INTEGER NOT NULL,"
      "gauge_profile INTEGER NOT NULL, gauge_auto_shift INTEGER NOT NULL,"
      "play_option TEXT, assist_option TEXT, completed_charts INTEGER NOT NULL,"
      "total_charts INTEGER NOT NULL, score INTEGER NOT NULL,"
      "max_score INTEGER NOT NULL, max_combo INTEGER NOT NULL,"
      "combo_break INTEGER NOT NULL, pgreat INTEGER NOT NULL,"
      "great INTEGER NOT NULL, good INTEGER NOT NULL, bad INTEGER NOT NULL,"
      "poor INTEGER NOT NULL, kpoor INTEGER NOT NULL, fast INTEGER NOT NULL,"
      "slow INTEGER NOT NULL, final_gauge REAL NOT NULL,"
      "clear_type INTEGER NOT NULL,"
      "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP)");
  execOrAbort(
      db.get(),
      "INSERT INTO scores (chart_path, chart_md5, chart_sha256, ln_mode,"
      "chart_title, chart_artist, score, max_score, max_combo, combo_break,"
      "pgreat, great, good, bad, poor, kpoor, fast, slow, final_gauge,"
      "clear_type, created_at) VALUES "
      "('BMS/legacy.bms','legacy-md5','legacy-sha',2,'Legacy','Artist',"
      "1234,2000,456,7,500,100,20,3,4,5,6,7,73.5,300,"
      "'2026-01-02 03:04:05')");
  execOrAbort(
      db.get(),
      "INSERT INTO course_scores (course_id, course_key, course_name,"
      "course_group_name, constraint_json, gauge_type, gauge_profile,"
      "gauge_auto_shift, play_option, assist_option, completed_charts,"
      "total_charts, score, max_score, max_combo, combo_break, pgreat, great,"
      "good, bad, poor, kpoor, fast, slow, final_gauge, clear_type, created_at)"
      " VALUES (17,'course-key','Legacy Course','Group','{}',2,1,1,'MIRROR',"
      "'OFF',2,3,2345,6000,321,8,900,200,30,4,5,6,7,8,61.25,200,"
      "'2026-02-03 04:05:06')");
  execOrAbort(db.get(), "PRAGMA user_version = 4");
}

void createVersion5ScoreFixture(const std::filesystem::path &path) {
  createVersion4ScoreFixture(path);
  auto db = openDatabase(path);
  for (const std::string table : {"scores", "course_scores"}) {
    execOrAbort(db.get(), "ALTER TABLE " + table +
                              " ADD COLUMN ruleset_version INTEGER NOT NULL "
                              "DEFAULT 0");
    execOrAbort(db.get(), "ALTER TABLE " + table +
                              " ADD COLUMN eligibility INTEGER NOT NULL "
                              "DEFAULT 2");
    execOrAbort(db.get(), "ALTER TABLE " + table +
                              " ADD COLUMN provenance_json TEXT NOT NULL "
                              "DEFAULT '" +
                              kLegacyProvenanceJson + "'");
  }
  execOrAbort(db.get(), "PRAGMA user_version = 5");
}

void createVersion5CourseMigrationFixture(const std::filesystem::path &path) {
  createVersion5ScoreFixture(path);
  auto db = openDatabase(path);
  execOrAbort(db.get(), "DELETE FROM course_scores");

  const std::vector<course_identity::ChartIdentity> md5Charts = {
      {.md5 = std::string(kMd5A)}, {.md5 = std::string(kMd5B)}};
  const std::string raw = legacyCourseKey("Legacy Course", "[]", md5Charts);
  const std::string converted = course_identity::makeCourseKey(md5Charts, "[]");
  const std::string canonical = course_identity::makeCourseKey(
      std::vector<course_identity::ChartIdentity>{
          {.sha256 = std::string(kShaC), .md5 = std::string(kMd5A)}},
      "[]");

  insertCourseScoreRow(db.get(), 17, raw, "Legacy Course", "Legacy Group", "[]",
                       2, 2345, 8, kClearTypeHardClearRank);
  insertCourseScoreRow(db.get(), 18, canonical, "Canonical", "Group", "[]", 1,
                       1234, 4, kClearTypeEasyClearRank);
  insertCourseScoreRow(db.get(), 19, "not-a-course-key", "Malformed", "Group",
                       "[]", 1, 1000, 5, kClearTypeNormalClearRank);
  insertCourseScoreRow(db.get(), 20, "", "Blank", "Group", "[]", 1, 900, 6,
                       kClearTypeAssistedEasyClearRank);
  assert(!converted.empty());
}

void createVersion6StaleScoreSummaryFixture(const std::filesystem::path &path) {
  createVersion5ScoreFixture(path);
  auto db = openDatabase(path);
  execOrAbort(db.get(), "DELETE FROM scores");
  execOrAbort(
      db.get(),
      "ALTER TABLE course_scores ADD COLUMN legacy_course_key TEXT NOT NULL "
      "DEFAULT ''");
  execOrAbort(db.get(),
              "ALTER TABLE course_scores ADD COLUMN ln_mode INTEGER NOT NULL "
              "DEFAULT -1");
  execOrAbort(db.get(),
              "CREATE INDEX idx_course_scores_key_ln_mode_clear_type ON "
              "course_scores(course_key, ln_mode, clear_type)");
  const char *scoreIndexes[] = {
      "CREATE INDEX idx_scores_chart_sha256 ON scores(chart_sha256)",
      "CREATE INDEX idx_scores_chart_md5 ON scores(chart_md5)",
      "CREATE INDEX idx_scores_chart_path ON scores(chart_path)",
      "CREATE INDEX idx_scores_chart_sha256_clear_type ON "
      "scores(chart_sha256, clear_type)",
      "CREATE INDEX idx_scores_chart_md5_clear_type ON "
      "scores(chart_md5, clear_type)",
      "CREATE INDEX idx_scores_chart_path_clear_type ON "
      "scores(chart_path, clear_type)",
      "CREATE INDEX idx_scores_chart_sha256_ln_mode ON "
      "scores(chart_sha256, ln_mode)",
      "CREATE INDEX idx_scores_chart_md5_ln_mode ON "
      "scores(chart_md5, ln_mode)",
      "CREATE INDEX idx_scores_chart_path_ln_mode ON "
      "scores(chart_path, ln_mode)",
      "CREATE INDEX idx_scores_identity_ln_mode ON "
      "scores(chart_sha256, chart_md5, chart_path, ln_mode)",
      "CREATE INDEX idx_scores_created_at ON scores(created_at)",
      "CREATE INDEX idx_course_scores_course_id ON course_scores(course_id)",
      "CREATE INDEX idx_course_scores_course_id_clear_type ON "
      "course_scores(course_id, clear_type)",
      "CREATE INDEX idx_course_scores_course_key ON course_scores(course_key)",
      "CREATE INDEX idx_course_scores_clear_type ON course_scores(clear_type)",
      "CREATE INDEX idx_course_scores_created_at ON course_scores(created_at)",
  };
  for (const char *indexSql : scoreIndexes) {
    execOrAbort(db.get(), indexSql);
  }

  const std::string insertScores =
      "INSERT INTO scores (chart_path, chart_md5, chart_sha256, ln_mode, "
      "chart_title, chart_artist, score, max_score, max_combo, combo_break, "
      "pgreat, great, good, bad, poor, kpoor, fast, slow, final_gauge, "
      "clear_type, ruleset_version, eligibility, provenance_json, created_at) "
      "VALUES "
      "('assisted.bms','assisted-md5','" +
      std::string(kShaA) +
      "',2,'Assisted','Artist',180,200,100,0,90,0,0,0,0,0,0,0,100," +
      std::to_string(kClearTypeHardClearRank) + ",0,1,'" +
      kAssistedProvenanceJson +
      "','2026-03-01 00:00:00'),"
      "('neutral.bms','neutral-md5','" +
      std::string(kShaB) +
      "',2,'Neutral','Artist',190,200,100,0,95,0,0,0,0,0,0,0,100," +
      std::to_string(kClearTypeFullComboRank) + ",0,2,'" +
      kLegacyProvenanceJson + "','2026-03-02 00:00:00')";
  execOrAbort(db.get(), insertScores);

  const auto summaryError =
      score_cache_queries::ensureScoreSummarySchema(db.get());
  assert(!summaryError.has_value());
  execOrAbort(
      db.get(),
      "INSERT INTO score_sha256_clear_rank_cache(chart_sha256, ln_mode, rank) "
      "SELECT lower(chart_sha256), ln_mode, " +
          std::to_string(kClearTypeFullComboRank) + " FROM scores");
  execOrAbort(
      db.get(),
      "INSERT INTO score_sha256_best_score_cache(chart_sha256, ln_mode, "
      "score_id, score, max_score, max_combo, combo_break, final_gauge, "
      "clear_type, clear_rank, score_rank, created_at) SELECT "
      "lower(chart_sha256), ln_mode, id, score, max_score, max_combo, "
      "combo_break, final_gauge, clear_type, " +
          std::to_string(kClearTypeFullComboRank) +
          ", 'AA', created_at FROM scores");
  execOrAbort(db.get(), "PRAGMA user_version = 6");
}

std::string chartOutcome(sqlite3 *db) {
  return queryText(
      db,
      "SELECT score || '|' || max_score || '|' || max_combo || '|' || "
      "combo_break || '|' || pgreat || '|' || great || '|' || good || '|' || "
      "bad || '|' || poor || '|' || kpoor || '|' || fast || '|' || slow || "
      "'|' || final_gauge || '|' || clear_type || '|' || created_at "
      "FROM scores WHERE id=1");
}

std::string courseOutcome(sqlite3 *db) {
  return queryText(
      db,
      "SELECT completed_charts || '|' || total_charts || '|' || score || '|' "
      "|| max_score || '|' || max_combo || '|' || combo_break || '|' || "
      "pgreat || '|' || great || '|' || good || '|' || bad || '|' || poor || "
      "'|' || kpoor || '|' || fast || '|' || slow || '|' || final_gauge || "
      "'|' || clear_type || '|' || created_at FROM course_scores WHERE id=1");
}

ScoreProvenance sampleProvenance(const std::string &hash) {
  ScoreProvenance value;
  value.ruleset = RulesetDescriptor::Current();
  value.stages = {{
      .chartMd5 = "md5-" + hash,
      .chartSha256 = "sha-" + hash,
      .longNoteMode = 2,
      .judgeRankSource = JudgeRankSource::Chart,
      .sourceJudgeRank = 2,
      .effectiveJudgeWindows = {{PGreat, -10000, 10000},
                                {Great, -30000, 30000}},
  }};
  value.gaugeType = GaugeType::Hard;
  value.player1 = {.option = "RANDOM", .seed = 1234};
  value.inputDevices = {InputDeviceCategory::Keyboard};
  value.eligibility = ScoreEligibility::Verified;
  return value;
}

bms_parser::ChartMeta sampleMeta(const std::filesystem::path &root,
                                 const std::string &hash) {
  bms_parser::ChartMeta meta;
  meta.BmsPath = root / "BMS" / (hash + ".bms");
  meta.MD5 = std::string(kMd5A);
  meta.SHA256 = std::string(kShaA);
  meta.Title = "Title " + hash;
  meta.Artist = "Artist";
  meta.TotalNotes = 100;
  meta.TotalLongNotes = 2;
  meta.LnMode = 2;
  return meta;
}

RhythmState sampleState(int pgreat, int great) {
  RhythmState state(nullptr, false);
  state.judgeCount[PGreat] = pgreat;
  state.judgeCount[Great] = great;
  state.maxCombo = pgreat + great;
  state.comboBreak = 1;
  state.fastCount = 3;
  state.slowCount = 4;
  state.currentGauge = 82.5f;
  return state;
}

std::string scoreAttemptId(int suffix) {
  assert(suffix >= 0 && suffix <= 4095);
  std::string value = "00000000-0000-4000-8000-000000000000";
  constexpr std::string_view digits = "0123456789abcdef";
  value[value.size() - 3] =
      digits[static_cast<std::size_t>((suffix / 256) % 16)];
  value[value.size() - 2] =
      digits[static_cast<std::size_t>((suffix / 16) % 16)];
  value.back() = digits[static_cast<std::size_t>(suffix % 16)];
  return value;
}

result_persistence::PendingChartScoreWrite
samplePendingScore(const std::filesystem::path &root, const std::string &name,
                   int suffix, std::string createdAt, int pgreat = 20,
                   int great = 5) {
  const bms_parser::ChartMeta meta = sampleMeta(root, name);
  const RhythmState state = sampleState(pgreat, great);
  const ScoreProvenance provenance = sampleProvenance(name);
  return {
      .attemptId = scoreAttemptId(suffix),
      .replayId = suffix + 1,
      .createdAt = std::move(createdAt),
      .score = result_persistence::captureChartScoreWrite(
          meta, state, provenance, scoreLongNoteModeForClearLamp(meta)),
  };
}

void removeAttemptIdentityFromCurrentSchema(sqlite3 *db) {
  if (indexExists(db, "idx_scores_attempt_id")) {
    execOrAbort(db, "DROP INDEX idx_scores_attempt_id");
  }
  if (columnExists(db, "scores", "attempt_id")) {
    execOrAbort(db, "ALTER TABLE scores DROP COLUMN attempt_id");
  }
}

void createVersion8ScoreFixture(const std::filesystem::path &path) {
  createVersion4ScoreFixture(path);
  {
    ScoreDBHelper bootstrap(path);
    assert(bootstrap.EnsureSchema());
  }
  auto db = openDatabase(path);
  removeAttemptIdentityFromCurrentSchema(db.get());
  execOrAbort(db.get(), "PRAGMA user_version = 8");
  assert(!columnExists(db.get(), "scores", "attempt_id"));
  assert(!indexExists(db.get(), "idx_scores_attempt_id"));
}

ScoreProvenance readStoredProvenance(sqlite3 *db, const std::string &table,
                                     int id) {
  const std::string json =
      queryText(db, "SELECT provenance_json FROM " + table +
                        " WHERE id=" + std::to_string(id));
  std::string error;
  const auto value = deserializeScoreProvenance(json, error);
  assert(value.has_value());
  assert(error.empty());
  return *value;
}

void testVersion8MigrationAddsAttemptIdentity(
    const std::filesystem::path &root) {
  const auto path = root / "migration-v9-attempt-identity" / "score.db";
  createVersion8ScoreFixture(path);

  ScoreDBHelper helper(path);
  assert(helper.EnsureSchema());

  auto db = openDatabase(path);
  assert(queryInt(db.get(), "PRAGMA user_version") == 9);
  assert(columnExists(db.get(), "scores", "attempt_id"));
  assert(indexExists(db.get(), "idx_scores_attempt_id"));
  assert(indexIsUniqueAndPartial(db.get(), "scores", "idx_scores_attempt_id"));
  assert(queryText(db.get(),
                   "SELECT sql FROM sqlite_master WHERE type='index' AND "
                   "name='idx_scores_attempt_id'")
             .find("WHERE attempt_id IS NOT NULL") != std::string::npos);
  assert(queryInt(db.get(), "SELECT COUNT(*) FROM scores") == 1);
  assert(queryInt(db.get(),
                  "SELECT COUNT(*) FROM scores WHERE attempt_id IS NULL") == 1);
  const std::string migratedSchema = schemaSnapshot(db.get());
  db.reset();

  assert(helper.EnsureSchema());
  db = openDatabase(path);
  assert(schemaSnapshot(db.get()) == migratedSchema);
}

void testStaleVersionRecognizesAppliedAttemptIdentity(
    const std::filesystem::path &root) {
  const auto path = root / "stale-v8-applied-attempt-identity" / "score.db";
  {
    ScoreDBHelper bootstrap(path);
    assert(bootstrap.EnsureSchema());
  }

  auto db = openDatabase(path);
  const std::string currentSchema = schemaSnapshot(db.get());
  assert(columnExists(db.get(), "scores", "attempt_id"));
  assert(indexExists(db.get(), "idx_scores_attempt_id"));
  execOrAbort(db.get(), "PRAGMA user_version = 8");
  db.reset();

  ScoreDBHelper helper(path);
  assert(helper.EnsureSchema());
  db = openDatabase(path);
  assert(queryInt(db.get(), "PRAGMA user_version") == 9);
  assert(schemaSnapshot(db.get()) == currentSchema);
}

void testStaleVersionRejectsPartialAttemptIdentity(
    const std::filesystem::path &root) {
  const auto path = root / "stale-v8-partial-attempt-identity" / "score.db";
  {
    ScoreDBHelper bootstrap(path);
    assert(bootstrap.EnsureSchema());
  }

  auto db = openDatabase(path);
  execOrAbort(db.get(), "DROP INDEX idx_scores_attempt_id");
  execOrAbort(db.get(), "PRAGMA user_version = 8");
  const std::string partialSchema = schemaSnapshot(db.get());
  db.reset();

  ScoreDBHelper helper(path);
  assert(!helper.EnsureSchema());
  db = openDatabase(path);
  assert(queryInt(db.get(), "PRAGMA user_version") == 8);
  assert(columnExists(db.get(), "scores", "attempt_id"));
  assert(!indexExists(db.get(), "idx_scores_attempt_id"));
  assert(schemaSnapshot(db.get()) == partialSchema);
}

void testLegacyNullAttemptScoresRemainRepeatable(
    const std::filesystem::path &root) {
  const auto path = root / "legacy-null-score-attempt" / "score.db";
  ScoreDBHelper helper(path);
  const auto meta = sampleMeta(root, "legacy-null-score-attempt");
  const auto state = sampleState(12, 3);
  const auto provenance = sampleProvenance("legacy-null-score-attempt");

  assert(helper.SaveScore(meta, state, provenance));
  assert(helper.SaveScore(meta, state, provenance));

  auto db = openDatabase(path);
  assert(queryInt(db.get(), "SELECT COUNT(*) FROM scores") == 2);
  assert(queryInt(db.get(),
                  "SELECT COUNT(*) FROM scores WHERE attempt_id IS NULL") == 2);
}

void testProjectedScoreIsIdempotent(const std::filesystem::path &root) {
  const auto path = root / "projected-score-idempotent" / "score.db";
  ScoreDBHelper helper(path);
  const auto pending = samplePendingScore(root, "projected-score-idempotent", 1,
                                          "2026-07-14 01:02:03");
  const std::uint64_t revisionBefore = helper.GetRevision();

  const auto first = helper.SaveProjectedScore(pending);
  const std::uint64_t revisionAfterInsert = helper.GetRevision();
  const auto second = helper.SaveProjectedScore(pending);

  assert(first.status == result_persistence::ProjectionStatus::Inserted);
  assert(first.diagnostic.empty());
  assert(second.status == result_persistence::ProjectionStatus::AlreadyPresent);
  assert(second.diagnostic.empty());
  assert(revisionAfterInsert == revisionBefore + 1);
  assert(helper.GetRevision() == revisionAfterInsert);
  auto db = openDatabase(path);
  assert(queryInt(db.get(), "SELECT COUNT(*) FROM scores") == 1);
  assert(queryText(db.get(), "SELECT attempt_id FROM scores") ==
         pending.attemptId);
}

void testProjectedScoreConflictDoesNotMutateExistingRow(
    const std::filesystem::path &root) {
  const auto path = root / "projected-score-conflict" / "score.db";
  ScoreDBHelper helper(path);
  const auto pending = samplePendingScore(root, "projected-score-conflict", 2,
                                          "2026-07-14 02:03:04");
  assert(helper.SaveProjectedScore(pending).status ==
         result_persistence::ProjectionStatus::Inserted);
  const std::uint64_t revisionAfterInsert = helper.GetRevision();

  auto judgementConflict = pending;
  ++judgementConflict.score.pGreat;
  const auto conflict = helper.SaveProjectedScore(judgementConflict);
  assert(conflict.status ==
         result_persistence::ProjectionStatus::IntegrityConflict);
  assert(!conflict.diagnostic.empty());

  auto timestampConflict = pending;
  timestampConflict.createdAt += ".001";
  assert(helper.SaveProjectedScore(timestampConflict).status ==
         result_persistence::ProjectionStatus::IntegrityConflict);

  auto textConflict = pending;
  textConflict.score.chartTitle += " changed";
  assert(helper.SaveProjectedScore(textConflict).status ==
         result_persistence::ProjectionStatus::IntegrityConflict);

  auto floatConflict = pending;
  floatConflict.score.finalGauge =
      std::nextafter(floatConflict.score.finalGauge, 100.0f);
  assert(helper.SaveProjectedScore(floatConflict).status ==
         result_persistence::ProjectionStatus::IntegrityConflict);

  auto provenanceConflict = pending;
  provenanceConflict.score.provenance.player1.seed = 4321;
  assert(helper.SaveProjectedScore(provenanceConflict).status ==
         result_persistence::ProjectionStatus::IntegrityConflict);

  assert(helper.GetRevision() == revisionAfterInsert);
  auto db = openDatabase(path);
  assert(queryInt(db.get(), "SELECT COUNT(*) FROM scores") == 1);
  assert(queryInt(db.get(), "SELECT pgreat FROM scores") ==
         pending.score.pGreat);
  assert(queryText(db.get(), "SELECT chart_title FROM scores") ==
         pending.score.chartTitle);
  assert(queryText(db.get(), "SELECT created_at FROM scores") ==
         pending.createdAt);
  assert(readStoredProvenance(db.get(), "scores", 1) ==
         pending.score.provenance);
}

void testProjectedScoreUsesReplayTimestamp(const std::filesystem::path &root) {
  const auto path = root / "projected-score-timestamp" / "score.db";
  ScoreDBHelper helper(path);
  const auto pending = samplePendingScore(root, "projected-score-timestamp", 3,
                                          "2024-02-29 23:59:58.987654");
  assert(helper.SaveProjectedScore(pending).status ==
         result_persistence::ProjectionStatus::Inserted);

  auto db = openDatabase(path);
  assert(queryText(db.get(), "SELECT created_at FROM scores") ==
         pending.createdAt);
  db.reset();
  const auto best =
      helper.LoadBestScore(sampleMeta(root, "projected-score-timestamp"));
  assert(best.has_value());
  assert(best->createdAt == pending.createdAt);
}

void testProjectedRetryUpdatesSummaryCachesOnce(
    const std::filesystem::path &root) {
  const auto path = root / "projected-score-summary-once" / "score.db";
  ScoreDBHelper helper(path);
  assert(helper.EnsureSchema());
  {
    auto db = openDatabase(path);
    execOrAbort(db.get(),
                "CREATE TABLE projection_insert_audit(value INTEGER)");
    execOrAbort(db.get(), "CREATE TRIGGER projection_insert_audit_after_insert "
                          "AFTER INSERT ON scores BEGIN INSERT INTO "
                          "projection_insert_audit VALUES (1); END");
  }

  const auto pending = samplePendingScore(root, "projected-score-summary-once",
                                          4, "2026-07-14 04:05:06");
  const std::uint64_t revisionBefore = helper.GetRevision();
  assert(helper.SaveProjectedScore(pending).status ==
         result_persistence::ProjectionStatus::Inserted);
  const std::uint64_t revisionAfterInsert = helper.GetRevision();
  assert(helper.SaveProjectedScore(pending).status ==
         result_persistence::ProjectionStatus::AlreadyPresent);

  assert(revisionAfterInsert == revisionBefore + 1);
  assert(helper.GetRevision() == revisionAfterInsert);
  auto db = openDatabase(path);
  assert(queryInt(db.get(), "SELECT COUNT(*) FROM projection_insert_audit") ==
         1);
  assert(queryInt(db.get(),
                  "SELECT COUNT(*) FROM score_sha256_clear_rank_cache") == 1);
  assert(queryInt(db.get(),
                  "SELECT COUNT(*) FROM score_sha256_best_score_cache") == 1);
  assert(queryInt(db.get(),
                  "SELECT score_id FROM score_sha256_best_score_cache") == 1);
  assert(
      queryInt(db.get(), "SELECT score FROM score_sha256_best_score_cache") ==
      pending.score.score);
}

void testPreviousBestExcludesExactAttemptAtSameTimestamp(
    const std::filesystem::path &root) {
  const auto path = root / "previous-best-exact-attempt" / "score.db";
  ScoreDBHelper helper(path);
  const std::string sharedTimestamp = "2026-07-14 05:06:07";
  const auto previous = samplePendingScore(root, "previous-best-exact-attempt",
                                           5, sharedTimestamp, 30, 0);
  const auto current = samplePendingScore(root, "previous-best-exact-attempt",
                                          6, sharedTimestamp, 40, 0);
  assert(helper.SaveProjectedScore(previous).status ==
         result_persistence::ProjectionStatus::Inserted);
  assert(helper.SaveProjectedScore(current).status ==
         result_persistence::ProjectionStatus::Inserted);

  const auto meta = sampleMeta(root, "previous-best-exact-attempt");
  const auto best = helper.LoadBestScore(meta);
  assert(best.has_value() && best->score == current.score.score);

  const auto excludingCurrent =
      helper.LoadBestScore(meta, std::nullopt, current.attemptId);
  assert(excludingCurrent.has_value());
  assert(excludingCurrent->score == previous.score.score);
  assert(excludingCurrent->createdAt == sharedTimestamp);

  const auto withBothFilters = helper.LoadBestScore(
      meta, std::string("2026-07-14 05:06:08"), current.attemptId);
  assert(withBothFilters.has_value());
  assert(withBothFilters->score == previous.score.score);

  RhythmState legacyState = sampleState(35, 0);
  assert(helper.SaveScore(meta, legacyState,
                          sampleProvenance("previous-best-legacy-null")));
  {
    auto db = openDatabase(path);
    execOrAbort(db.get(), "UPDATE scores SET created_at='" + sharedTimestamp +
                              "' WHERE attempt_id IS NULL");
  }
  const auto legacyFallback =
      helper.LoadBestScore(meta, std::nullopt, current.attemptId);
  assert(legacyFallback.has_value());
  assert(legacyFallback->score == legacyState.getScore());
  assert(legacyFallback->createdAt == sharedTimestamp);
}

void testProjectedScoreValidatesStoredTypesAndCanonicalProvenance(
    const std::filesystem::path &root) {
  const auto assertCorruptionConflicts =
      [&](const std::string &name, int suffix, const std::string &mutation) {
        const auto path = root / name / "score.db";
        ScoreDBHelper helper(path);
        const auto pending =
            samplePendingScore(root, name, suffix, "2026-07-14 06:07:08");
        assert(helper.SaveProjectedScore(pending).status ==
               result_persistence::ProjectionStatus::Inserted);
        const std::uint64_t revisionAfterInsert = helper.GetRevision();
        helper.Shutdown();
        {
          auto db = openDatabase(path);
          execOrAbort(db.get(), mutation);
        }
        const auto retry = helper.SaveProjectedScore(pending);
        assert(retry.status ==
               result_persistence::ProjectionStatus::IntegrityConflict);
        assert(!retry.diagnostic.empty());
        assert(helper.GetRevision() == revisionAfterInsert);
      };

  assertCorruptionConflicts("projected-score-type-corruption", 7,
                            "UPDATE scores SET final_gauge='not-a-real' WHERE "
                            "attempt_id IS NOT NULL");
  assertCorruptionConflicts(
      "projected-score-float-roundtrip-corruption", 8,
      "UPDATE scores SET final_gauge=final_gauge+0.000000000001 WHERE "
      "attempt_id IS NOT NULL");
  assertCorruptionConflicts(
      "projected-score-indexed-provenance-corruption", 9,
      "UPDATE scores SET ruleset_version=ruleset_version+1 WHERE attempt_id IS "
      "NOT NULL");
  assertCorruptionConflicts(
      "projected-score-json-corruption", 10,
      "UPDATE scores SET provenance_json=' ' || provenance_json WHERE "
      "attempt_id IS NOT NULL");
}

void testUnrelatedScoreConstraintIsStorageFailure(
    const std::filesystem::path &root) {
  const auto path = root / "projected-score-unrelated-constraint" / "score.db";
  ScoreDBHelper helper(path);
  assert(helper.EnsureSchema());
  {
    auto db = openDatabase(path);
    execOrAbort(db.get(), "CREATE UNIQUE INDEX unique_score_title_for_test ON "
                          "scores(chart_title)");
  }
  const auto first = samplePendingScore(
      root, "projected-score-unrelated-constraint", 10, "2026-07-14 07:08:09");
  auto second = first;
  second.attemptId = scoreAttemptId(11);
  second.replayId = 12;
  assert(helper.SaveProjectedScore(first).status ==
         result_persistence::ProjectionStatus::Inserted);
  const std::uint64_t revisionAfterInsert = helper.GetRevision();
  const auto outcome = helper.SaveProjectedScore(second);
  assert(outcome.status ==
         result_persistence::ProjectionStatus::StorageFailure);
  assert(!outcome.diagnostic.empty());
  assert(helper.GetRevision() == revisionAfterInsert);
  auto db = openDatabase(path);
  assert(queryInt(db.get(), "SELECT COUNT(*) FROM scores") == 1);
  assert(queryInt(db.get(), "SELECT COUNT(*) FROM scores WHERE attempt_id='" +
                                second.attemptId + "'") == 0);
}

void testVersion4MigrationPreservesOutcomesAndRows(
    const std::filesystem::path &root) {
  const auto path = root / "migration" / "score.db";
  createVersion4ScoreFixture(path);
  auto before = openDatabase(path);
  const std::string chartBefore = chartOutcome(before.get());
  const std::string courseBefore = courseOutcome(before.get());
  before.reset();

  ScoreDBHelper helper(path);
  assert(helper.GetDatabasePath() == path);
  assert(helper.EnsureSchema());

  auto migrated = openDatabase(path);
  assert(queryInt(migrated.get(), "PRAGMA user_version") ==
         ScoreDBHelper::kCurrentSchemaVersion);
  assert(queryInt(migrated.get(), "SELECT COUNT(*) FROM scores") == 1);
  assert(queryInt(migrated.get(), "SELECT COUNT(*) FROM course_scores") == 1);
  assert(chartOutcome(migrated.get()) == chartBefore);
  assert(courseOutcome(migrated.get()) == courseBefore);
  for (const std::string table : {"scores", "course_scores"}) {
    assert(columnExists(migrated.get(), table, "ruleset_version"));
    assert(columnExists(migrated.get(), table, "eligibility"));
    assert(columnExists(migrated.get(), table, "provenance_json"));
    assert(queryInt(migrated.get(), "SELECT ruleset_version FROM " + table +
                                        " WHERE id=1") == 0);
    assert(queryInt(migrated.get(),
                    "SELECT eligibility FROM " + table + " WHERE id=1") ==
           static_cast<int>(ScoreEligibility::LegacyUnverified));
    assert(readStoredProvenance(migrated.get(), table, 1) ==
           ScoreProvenance::Legacy());
    assert(queryText(migrated.get(),
                     "SELECT provenance_json FROM " + table + " WHERE id=1") ==
           kLegacyProvenanceJson);
  }
  const std::string firstSchema = schemaSnapshot(migrated.get());
  migrated.reset();

  assert(helper.EnsureSchema());
  auto second = openDatabase(path);
  assert(schemaSnapshot(second.get()) == firstSchema);
  assert(queryInt(second.get(), "SELECT COUNT(*) FROM scores") == 1);
  assert(queryInt(second.get(), "SELECT COUNT(*) FROM course_scores") == 1);
}

void testVersion5MigrationPreservesRawCourseEvidence(
    const std::filesystem::path &root) {
  const auto path = root / "migration-v6" / "score.db";
  createVersion5CourseMigrationFixture(path);
  auto before = openDatabase(path);
  const std::string chartBefore = chartOutcome(before.get());
  const int rowsBefore =
      queryInt(before.get(), "SELECT COUNT(*) FROM course_scores");
  before.reset();

  const std::vector<course_identity::ChartIdentity> md5Charts = {
      {.md5 = std::string(kMd5A)}, {.md5 = std::string(kMd5B)}};
  const std::string raw = legacyCourseKey("Legacy Course", "[]", md5Charts);
  const std::string converted = course_identity::makeCourseKey(md5Charts, "[]");

  ScoreDBHelper helper(path);
  assert(helper.EnsureSchema());
  auto migrated = openDatabase(path);
  assert(queryInt(migrated.get(), "PRAGMA user_version") ==
         ScoreDBHelper::kCurrentSchemaVersion);
  assert(columnExists(migrated.get(), "course_scores", "legacy_course_key"));
  assert(columnExists(migrated.get(), "course_scores", "ln_mode"));
  assert(
      indexExists(migrated.get(), "idx_course_scores_key_ln_mode_clear_type"));
  assert(queryInt(migrated.get(), "SELECT COUNT(*) FROM course_scores") ==
         rowsBefore);
  assert(chartOutcome(migrated.get()) == chartBefore);
  assert(queryInt(migrated.get(),
                  "SELECT COUNT(*) FROM course_scores WHERE ln_mode=-1") ==
         rowsBefore);
  assert(queryText(migrated.get(),
                   "SELECT course_key FROM course_scores WHERE course_id=17") ==
         converted);
  assert(
      queryText(
          migrated.get(),
          "SELECT legacy_course_key FROM course_scores WHERE course_id=17") ==
      raw);
  assert(queryText(
             migrated.get(),
             "SELECT legacy_course_key FROM course_scores WHERE course_id=18")
             .empty());
  assert(queryText(migrated.get(),
                   "SELECT course_key FROM course_scores WHERE course_id=19") ==
         "not-a-course-key");
  assert(
      queryText(
          migrated.get(),
          "SELECT legacy_course_key FROM course_scores WHERE course_id=19") ==
      "not-a-course-key");
  assert(queryText(migrated.get(),
                   "SELECT course_key FROM course_scores WHERE course_id=20")
             .empty());
  assert(queryText(
             migrated.get(),
             "SELECT legacy_course_key FROM course_scores WHERE course_id=20")
             .empty());
  const std::string schemaAfterFirstPass = schemaSnapshot(migrated.get());
  migrated.reset();

  assert(helper.EnsureSchema());
  auto second = openDatabase(path);
  assert(schemaSnapshot(second.get()) == schemaAfterFirstPass);
  assert(
      queryText(
          second.get(),
          "SELECT legacy_course_key FROM course_scores WHERE course_id=17") ==
      raw);
}

void testVersion6MigrationIsSavepointSafeAndAtomic(
    const std::filesystem::path &root) {
  const auto commitPath = root / "migration-v6-outer-commit" / "score.db";
  createVersion5CourseMigrationFixture(commitPath);
  {
    auto db = openDatabase(commitPath);
    execOrAbort(db.get(), "BEGIN TRANSACTION");
    ScoreDBHelper helper(commitPath);
    assert(helper.InsertScore(db.get(), sampleMeta(root, "v6-commit"),
                              sampleState(1, 1)));
    assert(queryInt(db.get(), "PRAGMA user_version") ==
           ScoreDBHelper::kCurrentSchemaVersion);
    assert(columnExists(db.get(), "course_scores", "legacy_course_key"));
    assert(columnExists(db.get(), "scores", "attempt_id"));
    assert(indexExists(db.get(), "idx_scores_attempt_id"));
    execOrAbort(db.get(), "COMMIT");
  }
  auto committed = openDatabase(commitPath);
  assert(queryInt(committed.get(), "PRAGMA user_version") ==
         ScoreDBHelper::kCurrentSchemaVersion);
  assert(columnExists(committed.get(), "course_scores", "ln_mode"));
  committed.reset();

  const auto rollbackPath = root / "migration-v6-outer-rollback" / "score.db";
  createVersion5CourseMigrationFixture(rollbackPath);
  {
    auto db = openDatabase(rollbackPath);
    execOrAbort(db.get(), "BEGIN TRANSACTION");
    ScoreDBHelper helper(rollbackPath);
    assert(helper.InsertScore(db.get(), sampleMeta(root, "v6-rollback"),
                              sampleState(1, 1)));
    assert(queryInt(db.get(), "PRAGMA user_version") ==
           ScoreDBHelper::kCurrentSchemaVersion);
    assert(columnExists(db.get(), "course_scores", "legacy_course_key"));
    assert(columnExists(db.get(), "scores", "attempt_id"));
    assert(indexExists(db.get(), "idx_scores_attempt_id"));
    execOrAbort(db.get(), "ROLLBACK");
  }
  auto rolledBack = openDatabase(rollbackPath);
  assert(queryInt(rolledBack.get(), "PRAGMA user_version") == 5);
  assert(!columnExists(rolledBack.get(), "course_scores", "legacy_course_key"));
  assert(!columnExists(rolledBack.get(), "course_scores", "ln_mode"));
  assert(!indexExists(rolledBack.get(),
                      "idx_course_scores_key_ln_mode_clear_type"));
  assert(!columnExists(rolledBack.get(), "scores", "attempt_id"));
  assert(!indexExists(rolledBack.get(), "idx_scores_attempt_id"));
  assert(queryInt(rolledBack.get(), "SELECT COUNT(*) FROM scores") == 1);
  rolledBack.reset();

  const auto failurePath = root / "migration-v6-failure" / "score.db";
  createVersion5CourseMigrationFixture(failurePath);
  {
    auto db = openDatabase(failurePath);
    execOrAbort(db.get(),
                "CREATE TRIGGER fail_v6_course_update BEFORE UPDATE ON "
                "course_scores BEGIN SELECT RAISE(ABORT, 'injected'); END");
  }
  ScoreDBHelper failing(failurePath);
  assert(!failing.EnsureSchema());
  auto failed = openDatabase(failurePath);
  assert(queryInt(failed.get(), "PRAGMA user_version") == 5);
  assert(!columnExists(failed.get(), "course_scores", "legacy_course_key"));
  assert(!columnExists(failed.get(), "course_scores", "ln_mode"));
  assert(
      !indexExists(failed.get(), "idx_course_scores_key_ln_mode_clear_type"));
  assert(queryText(failed.get(),
                   "SELECT course_key FROM course_scores WHERE course_id=17")
             .starts_with("course:Legacy Course\n"));
}

void testVersion7MigrationRepairsPopulatedScoreSummariesExactlyOnce(
    const std::filesystem::path &root) {
  const auto path = root / "migration-v7-summary-semantics" / "score.db";
  createVersion6StaleScoreSummaryFixture(path);
  {
    auto stale = openDatabase(path);
    assert(queryInt(stale.get(), "PRAGMA user_version") == 6);
    assert(queryInt(stale.get(),
                    "SELECT rank FROM score_sha256_clear_rank_cache WHERE "
                    "chart_sha256='" +
                        std::string(kShaA) + "'") == kClearTypeFullComboRank);
    assert(queryInt(stale.get(),
                    "SELECT clear_rank FROM score_sha256_best_score_cache "
                    "WHERE chart_sha256='" +
                        std::string(kShaA) + "'") == kClearTypeFullComboRank);
  }

  {
    ScoreDBHelper helper(path);
    assert(helper.EnsureSchema());
  }

  std::string schemaAfterMigration;
  {
    auto migrated = openDatabase(path);
    assert(queryInt(migrated.get(), "SELECT COUNT(*) FROM scores") == 2);
    assert(queryInt(migrated.get(),
                    "SELECT COUNT(*) FROM score_sha256_clear_rank_cache WHERE "
                    "chart_sha256='" +
                        std::string(kShaA) + "'") == 0);
    assert(queryInt(migrated.get(), "PRAGMA user_version") ==
           ScoreDBHelper::kCurrentSchemaVersion);
    assert(queryInt(migrated.get(),
                    "SELECT COUNT(*) FROM score_sha256_best_score_cache "
                    "WHERE chart_sha256='" +
                        std::string(kShaA) + "'") == 0);
    assert(queryInt(migrated.get(),
                    "SELECT rank FROM score_sha256_clear_rank_cache WHERE "
                    "chart_sha256='" +
                        std::string(kShaB) + "'") == kClearTypeFullComboRank);
    assert(queryInt(migrated.get(),
                    "SELECT clear_rank FROM score_sha256_best_score_cache "
                    "WHERE chart_sha256='" +
                        std::string(kShaB) + "'") == kClearTypeFullComboRank);
    assert(queryInt(migrated.get(),
                    "SELECT clear_type FROM scores WHERE chart_sha256='" +
                        std::string(kShaA) + "'") == kClearTypeHardClearRank);
    assert(queryInt(migrated.get(),
                    "SELECT clear_type FROM scores WHERE chart_sha256='" +
                        std::string(kShaB) + "'") == kClearTypeFullComboRank);
    schemaAfterMigration = schemaSnapshot(migrated.get());
  }

  {
    ScoreDBHelper reopened(path);
    assert(reopened.EnsureSchema());
  }
  auto idempotent = openDatabase(path);
  assert(queryInt(idempotent.get(), "PRAGMA user_version") ==
         ScoreDBHelper::kCurrentSchemaVersion);
  assert(schemaSnapshot(idempotent.get()) == schemaAfterMigration);
  assert(queryInt(idempotent.get(),
                  "SELECT COUNT(*) FROM score_sha256_clear_rank_cache WHERE "
                  "chart_sha256='" +
                      std::string(kShaA) + "'") == 0);

  const auto failurePath =
      root / "migration-v7-summary-semantics-failure" / "score.db";
  createVersion6StaleScoreSummaryFixture(failurePath);
  std::string schemaBeforeFailure;
  {
    auto db = openDatabase(failurePath);
    execOrAbort(db.get(),
                "CREATE TRIGGER fail_v7_summary_rebuild BEFORE DELETE ON "
                "score_sha256_clear_rank_cache BEGIN SELECT RAISE(ABORT, "
                "'injected'); END");
    schemaBeforeFailure = schemaSnapshot(db.get());
  }
  ScoreDBHelper failing(failurePath);
  assert(!failing.EnsureSchema());
  auto failed = openDatabase(failurePath);
  assert(queryInt(failed.get(), "PRAGMA user_version") == 6);
  assert(schemaSnapshot(failed.get()) == schemaBeforeFailure);
  assert(queryInt(failed.get(),
                  "SELECT rank FROM score_sha256_clear_rank_cache WHERE "
                  "chart_sha256='" +
                      std::string(kShaA) + "'") == kClearTypeFullComboRank);
}

void testCourseWritesUseAuthoritativeKeysAndExactMode(
    const std::filesystem::path &root) {
  const auto path = root / "course-write-v6" / "score.db";
  ScoreDBHelper helper(path);
  assert(helper.EnsureSchema());

  bms_parser::ChartMeta meta = sampleMeta(root, "course-key-authoritative");
  meta.SHA256 = std::string(kShaA);
  meta.MD5 = std::string(kMd5A);
  CoursePlaySession session;
  session.courseId = 71;
  session.courseName = "Renamed course";
  session.constraintJson = "[]";
  session.entries.push_back({.meta = meta});
  session.courseKey = course_identity::makeCourseKey(
      std::vector<course_identity::ChartIdentity>{{.md5 = std::string(kMd5A)}},
      "[]");
  session.longNoteMode = long_note_mode::kHcnValue;
  const std::string recomputed = course_identity::makeCourseKey(session);
  assert(recomputed != session.courseKey);
  assert(helper.SaveCourseScore(session, sampleState(5, 1), 1, 1));

  auto db = openDatabase(path);
  assert(queryText(db.get(), "SELECT course_key FROM course_scores") ==
         session.courseKey);
  assert(queryInt(db.get(), "SELECT ln_mode FROM course_scores") ==
         long_note_mode::kHcnValue);
  db.reset();

  CoursePlaySession fallback = session;
  fallback.courseId = 72;
  fallback.courseKey.clear();
  fallback.longNoteMode = 99;
  assert(helper.SaveCourseScore(fallback, sampleState(4, 1), 1, 1));
  db = openDatabase(path);
  assert(queryText(db.get(),
                   "SELECT course_key FROM course_scores WHERE course_id=72") ==
         recomputed);
  assert(queryInt(db.get(),
                  "SELECT ln_mode FROM course_scores WHERE course_id=72") ==
         long_note_mode::kUnknownValue);

  const auto rejectedPath = root / "course-write-unkeyable" / "score.db";
  CoursePlaySession unkeyable;
  unkeyable.courseId = 73;
  unkeyable.constraintJson = "[]";
  ScoreDBHelper rejected(rejectedPath);
  assert(!rejected.SaveCourseScore(unkeyable, sampleState(1, 0), 0, 0));
  assert(!std::filesystem::exists(rejectedPath));
}

void testCourseReadsAreKeyAndModeAuthoritative(
    const std::filesystem::path &root) {
  const auto path = root / "course-read-v6" / "score.db";
  ScoreDBHelper helper(path);
  assert(helper.EnsureSchema());
  const std::string keyA = course_identity::makeCourseKey(
      std::vector<course_identity::ChartIdentity>{
          {.sha256 = std::string(kShaA)}},
      "[]");
  const std::string keyB = course_identity::makeCourseKey(
      std::vector<course_identity::ChartIdentity>{
          {.sha256 = std::string(kShaB)}},
      "[]");
  const std::string wildcardKey = course_identity::makeCourseKey(
      std::vector<course_identity::ChartIdentity>{
          {.sha256 = std::string(kShaC)}},
      "[]");
  const std::string missingKey = course_identity::makeCourseKey(
      std::vector<course_identity::ChartIdentity>{
          {.sha256 = std::string(kShaD)}},
      "[]");

  auto db = openDatabase(path);
  insertCourseScoreRow(db.get(), 11, keyA, "Old name", "Group", "[]", 1, 200, 1,
                       kClearTypeEasyClearRank, long_note_mode::kCnValue);
  insertCourseScoreRow(db.get(), 44, keyA, "Old name", "Group", "[]", 1, 150, 1,
                       kClearTypeNormalClearRank, long_note_mode::kLnValue);
  insertCourseScoreRow(db.get(), 55, keyA, "Old name", "Group", "[]", 1, 900, 1,
                       kClearTypeHardClearRank, long_note_mode::kHcnValue);
  insertCourseScoreRow(db.get(), 11, keyB, "Other content", "Group", "[]", 1,
                       999, 1, kClearTypeFullComboRank,
                       long_note_mode::kCnValue);
  insertCourseScoreRow(db.get(), 66, "", "Blank legacy", "Group", "[]", 1, 300,
                       1, kClearTypeNormalClearRank, long_note_mode::kCnValue);
  insertCourseScoreRow(db.get(), 67, "", "Other blank legacy", "Group", "[]", 1,
                       800, 1, kClearTypeHardClearRank,
                       long_note_mode::kCnValue);
  insertCourseScoreRow(db.get(), 77, "malformed", "Malformed", "Group", "[]", 1,
                       1000, 1, kClearTypeFullComboRank,
                       long_note_mode::kCnValue);
  insertCourseScoreRow(db.get(), 88, wildcardKey, "Wildcard", "Group", "[]", 1,
                       500, 1, kClearTypeNormalClearRank, -1);
  insertCourseScoreRow(db.get(), 88, wildcardKey, "Wildcard", "Group", "[]", 1,
                       400, 1, kClearTypeHardClearRank,
                       long_note_mode::kCnValue);
  db.reset();

  CoursePlaySession renamed;
  renamed.courseId = 999;
  renamed.courseKey = keyA;
  renamed.courseName = "New name";
  renamed.longNoteMode = long_note_mode::kCnValue;
  const auto byKey = helper.LoadBestCourseScore(renamed);
  assert(byKey.has_value() && byKey->score == 200);

  CoursePlaySession mismatchingKey = renamed;
  mismatchingKey.courseId = 11;
  mismatchingKey.courseKey = missingKey;
  assert(!helper.LoadBestCourseScore(mismatchingKey).has_value());

  CoursePlaySession blankFallback = renamed;
  blankFallback.courseId = 66;
  blankFallback.courseKey = missingKey;
  const auto byBlankId = helper.LoadBestCourseScore(blankFallback);
  assert(byBlankId.has_value() && byBlankId->score == 300);

  CoursePlaySession emptyRequestedKey;
  emptyRequestedKey.courseId = 66;
  emptyRequestedKey.longNoteMode = long_note_mode::kCnValue;
  const auto blankRequestById = helper.LoadBestCourseScore(emptyRequestedKey);
  assert(blankRequestById.has_value() && blankRequestById->score == 300);

  CoursePlaySession malformedDoesNotFallback = renamed;
  malformedDoesNotFallback.courseId = 77;
  malformedDoesNotFallback.courseKey = missingKey;
  assert(!helper.LoadBestCourseScore(malformedDoesNotFallback).has_value());

  CoursePlaySession wildcard = renamed;
  wildcard.courseId = 88;
  wildcard.courseKey = wildcardKey;
  for (int mode : long_note_mode::kPlayableValues) {
    wildcard.longNoteMode = mode;
    const auto score = helper.LoadBestCourseScore(wildcard);
    assert(score.has_value() && score->score == 500);
  }

  db = openDatabase(path);
  insertCourseScoreRow(db.get(), 88, wildcardKey, "Wildcard", "Group", "[]", 1,
                       600, 1, kClearTypeEasyClearRank,
                       long_note_mode::kCnValue);
  db.reset();
  wildcard.longNoteMode = long_note_mode::kCnValue;
  const auto exactHigher = helper.LoadBestCourseScore(wildcard);
  assert(exactHigher.has_value() && exactHigher->score == 600);
}

void testCourseLampCacheSeparatesKeysIdsAndModes(
    const std::filesystem::path &root) {
  const auto path = root / "course-lamp-v6" / "score.db";
  ScoreDBHelper helper(path);
  assert(helper.EnsureSchema());
  const std::string keyA = course_identity::makeCourseKey(
      std::vector<course_identity::ChartIdentity>{
          {.sha256 = std::string(kShaA)}},
      "[]");
  const std::string keyB = course_identity::makeCourseKey(
      std::vector<course_identity::ChartIdentity>{
          {.sha256 = std::string(kShaB)}},
      "[]");

  auto db = openDatabase(path);
  insertCourseScoreRow(db.get(), 1, keyA, "Old", "Group", "[]", 1, 100, 1,
                       kClearTypeEasyClearRank, long_note_mode::kLnValue);
  insertCourseScoreRow(db.get(), 2, keyA, "New", "Group", "[]", 1, 200, 1,
                       kClearTypeHardClearRank, long_note_mode::kCnValue);
  insertCourseScoreRow(db.get(), 3, keyA, "Wildcard", "Group", "[]", 1, 150, 1,
                       kClearTypeNormalClearRank, -1);
  insertCourseScoreRow(db.get(), 50, keyB, "Nonblank", "Group", "[]", 1, 300, 1,
                       kClearTypeFullComboRank, long_note_mode::kLnValue);
  insertCourseScoreRow(db.get(), 50, "", "Blank", "Group", "[]", 1, 90, 1,
                       kClearTypeAssistedEasyClearRank,
                       long_note_mode::kLnValue);
  db.reset();

  const ScoreClearRankCache cache = helper.LoadBestClearRanks();
  assert(cache.bestCourseRankFor(keyA, 999, long_note_mode::kLnValue) ==
         kClearTypeNormalClearRank);
  assert(cache.bestCourseRankFor(keyA, 999, long_note_mode::kCnValue) ==
         kClearTypeHardClearRank);
  assert(cache.bestCourseRankFor(keyA, 999, long_note_mode::kHcnValue) ==
         kClearTypeNormalClearRank);
  assert(cache.bestCourseRankFor("missing", 50, long_note_mode::kLnValue) ==
         kClearTypeAssistedEasyClearRank);
  assert(cache.bestCourseRankFor("missing", 2, long_note_mode::kCnValue) ==
         kNoClearTypeRank);
}

std::vector<course_identity::ChartIdentity> enrichedRecoveryCharts() {
  return {{.sha256 = std::string(kShaA), .md5 = std::string(kMd5A)},
          {.sha256 = std::string(kShaB), .md5 = std::string(kMd5B)}};
}

void testCourseRecoveryUsesStrongestCommonEvidenceAndOwnsResult(
    const std::filesystem::path &root) {
  const auto path = root / "course-recovery-v6" / "score.db";
  createVersion5CourseMigrationFixture(path);
  {
    auto db = openDatabase(path);
    const std::vector<course_identity::ChartIdentity> md5Charts = {
        {.md5 = std::string(kMd5A)}, {.md5 = std::string(kMd5B)}};
    insertCourseScoreRow(db.get(), 17,
                         legacyCourseKey("Legacy Course", "[]", md5Charts),
                         "Legacy Course", "Legacy Group", "[]", 2, 2000, 2,
                         kClearTypeEasyClearRank);
  }

  const auto enriched = enrichedRecoveryCharts();
  const std::string currentKey = course_identity::makeCourseKey(enriched, "[]");
  std::vector<course_identity::Definition> definitions = {
      {.courseId = 700,
       .courseKey = currentKey,
       .name = "Current renamed course",
       .groupName = "Current group",
       .constraintJson = "[]",
       .charts = enriched},
      {.courseId = 701,
       .courseKey = currentKey,
       .name = "Duplicate current definition",
       .groupName = "Other group",
       .constraintJson = "[]",
       .charts = enriched},
  };

  ScoreDBHelper helper(path);
  const CourseScoreRecoveryResult result =
      helper.RecoverCourseRecords(definitions);
  assert(result.ok());
  assert(result.evidence.size() == 1);
  assert((result.evidence.front() ==
          CourseScoreEvidence{.legacyCourseId = 17,
                              .totalCharts = 2,
                              .courseName = "Legacy Course",
                              .courseGroupName = "Legacy Group",
                              .canonicalConstraintPayload = "[]",
                              .courseKey = currentKey}));
  definitions.clear();
  assert(result.evidence.front().courseKey == currentKey);

  auto db = openDatabase(path);
  assert(queryText(db.get(),
                   "SELECT course_key FROM course_scores WHERE course_id=17 "
                   "ORDER BY id LIMIT 1") == currentKey);
  assert(queryText(db.get(),
                   "SELECT legacy_course_key FROM course_scores WHERE "
                   "course_id=17 ORDER BY id LIMIT 1")
             .starts_with("course:Legacy Course\n"));
  assert(queryText(db.get(),
                   "SELECT course_id || '|' || course_name || '|' || "
                   "course_group_name || '|' || constraint_json || '|' || "
                   "total_charts FROM course_scores WHERE course_id=17 "
                   "ORDER BY id LIMIT 1") ==
         "17|Legacy Course|Legacy Group|[]|2");
}

void testCourseRecoveryFailsClosedOnAmbiguityAndFailure(
    const std::filesystem::path &root) {
  const auto path = root / "course-recovery-ambiguous" / "score.db";
  createVersion5CourseMigrationFixture(path);
  ScoreDBHelper helper(path);
  assert(helper.EnsureSchema());

  const std::vector<course_identity::ChartIdentity> firstCharts = {
      {.sha256 = std::string(kShaA), .md5 = std::string(kMd5A)},
      {.sha256 = std::string(kShaB), .md5 = std::string(kMd5B)}};
  const std::vector<course_identity::ChartIdentity> secondCharts = {
      {.sha256 = std::string(kShaC), .md5 = std::string(kMd5A)},
      {.sha256 = std::string(kShaD), .md5 = std::string(kMd5B)}};
  const std::string firstKey =
      course_identity::makeCourseKey(firstCharts, "[]");
  const std::string secondKey =
      course_identity::makeCourseKey(secondCharts, "[]");
  const std::vector<course_identity::Definition> definitions = {
      {.courseId = 801,
       .courseKey = firstKey,
       .constraintJson = "[]",
       .charts = firstCharts},
      {.courseId = 802,
       .courseKey = secondKey,
       .constraintJson = "[]",
       .charts = secondCharts},
  };

  auto db = openDatabase(path);
  insertCourseScoreRow(db.get(), 99, firstKey, "Shared tuple", "Old group",
                       "[]", 2, 100, 1, kClearTypeEasyClearRank,
                       long_note_mode::kLnValue);
  insertCourseScoreRow(db.get(), 99, secondKey, "Shared tuple", "Old group",
                       "[]", 2, 110, 1, kClearTypeNormalClearRank,
                       long_note_mode::kLnValue);
  db.reset();

  const CourseScoreRecoveryResult ambiguous =
      helper.RecoverCourseRecords(definitions);
  assert(ambiguous.ok());
  db = openDatabase(path);
  const std::vector<course_identity::ChartIdentity> md5Charts = {
      {.md5 = std::string(kMd5A)}, {.md5 = std::string(kMd5B)}};
  assert(queryText(db.get(),
                   "SELECT course_key FROM course_scores WHERE course_id=17") ==
         course_identity::makeCourseKey(md5Charts, "[]"));
  assert(queryText(db.get(),
                   "SELECT course_key FROM course_scores WHERE course_id=19") ==
         "not-a-course-key");
  assert(queryText(db.get(),
                   "SELECT course_key FROM course_scores WHERE course_id=20")
             .empty());
  const auto sharedEvidence = std::ranges::count_if(
      ambiguous.evidence, [](const CourseScoreEvidence &item) {
        return item.legacyCourseId == 99 && item.courseName == "Shared tuple";
      });
  assert(sharedEvidence == 2);
  db.reset();

  const auto failurePath = root / "course-recovery-failure" / "score.db";
  createVersion5CourseMigrationFixture(failurePath);
  ScoreDBHelper failing(failurePath);
  assert(failing.EnsureSchema());
  {
    auto failureDb = openDatabase(failurePath);
    execOrAbort(failureDb.get(),
                "CREATE TRIGGER fail_recovery_update BEFORE UPDATE OF "
                "course_key ON course_scores BEGIN SELECT RAISE(ABORT, "
                "'injected recovery failure'); END");
  }
  const std::uint64_t revisionBefore = failing.GetRevision();
  const CourseScoreRecoveryResult failed = failing.RecoverCourseRecords(
      std::vector<course_identity::Definition>{{.courseId = 900,
                                                .courseKey = firstKey,
                                                .constraintJson = "[]",
                                                .charts = firstCharts}});
  assert(!failed.ok());
  assert(failed.evidence.empty());
  assert(failing.GetRevision() == revisionBefore);
  auto failureDb = openDatabase(failurePath);
  assert(queryText(failureDb.get(),
                   "SELECT course_key FROM course_scores WHERE course_id=17") ==
         course_identity::makeCourseKey(md5Charts, "[]"));
}

void testIgnoredRecoveryUpdateDoesNotAdvanceRevision(
    const std::filesystem::path &root) {
  const auto path = root / "course-recovery-ignored-update" / "score.db";
  createVersion5CourseMigrationFixture(path);
  ScoreDBHelper helper(path);
  assert(helper.EnsureSchema());

  auto db = openDatabase(path);
  execOrAbort(db.get(),
              "CREATE TRIGGER ignore_recovery_update BEFORE UPDATE OF "
              "course_key ON course_scores BEGIN SELECT RAISE(IGNORE); END");
  db.reset();

  const auto charts = enrichedRecoveryCharts();
  const std::string currentKey = course_identity::makeCourseKey(charts, "[]");
  const std::uint64_t revisionBefore = helper.GetRevision();
  const CourseScoreRecoveryResult result = helper.RecoverCourseRecords(
      std::vector<course_identity::Definition>{{.courseId = 901,
                                                .courseKey = currentKey,
                                                .constraintJson = "[]",
                                                .charts = charts}});
  assert(result.ok());
  assert(helper.GetRevision() == revisionBefore);

  db = openDatabase(path);
  const std::vector<course_identity::ChartIdentity> md5Charts = {
      {.md5 = std::string(kMd5A)}, {.md5 = std::string(kMd5B)}};
  assert(queryText(db.get(),
                   "SELECT course_key FROM course_scores WHERE course_id=17") ==
         course_identity::makeCourseKey(md5Charts, "[]"));
}

void testFutureVersionNinePlusOneIsRejected(const std::filesystem::path &root) {
  const auto path = root / "future-v10-recovery" / "score.db";
  createFutureSentinelDatabase(path);
  {
    auto db = openDatabase(path);
    execOrAbort(db.get(), "PRAGMA user_version=10");
  }
  const auto before = rawDatabaseFamilySnapshot(path);
  ScoreDBHelper helper(path);
  assert(!helper.EnsureSchema());
  CoursePlaySession session;
  session.courseId = 7;
  session.courseKey = course_identity::makeCourseKey(
      std::vector<course_identity::ChartIdentity>{
          {.sha256 = std::string(kShaA)}},
      "[]");
  session.entries.push_back({.meta = sampleMeta(root, "future-v10")});
  assert(!helper.SaveCourseScore(session, sampleState(1, 0), 1, 1));
  const CourseScoreRecoveryResult result = helper.RecoverCourseRecords({});
  assert(!result.ok());
  assert(result.evidence.empty());
  assert(rawDatabaseFamilySnapshot(path) == before);
}

void testChartAndCourseRoundTripAndPathIsolation(
    const std::filesystem::path &root) {
  const auto firstPath = root / "profiles" / "one" / "score.db";
  const auto secondPath = root / "profiles" / "two" / "score.db";
  ScoreDBHelper first(firstPath);
  ScoreDBHelper second(secondPath);
  assert(first.EnsureSchema());
  assert(second.EnsureSchema());

  const auto meta = sampleMeta(root, "roundtrip");
  const auto state = sampleState(20, 5);
  const auto provenance = sampleProvenance("roundtrip");
  assert(first.SaveScore(meta, state, provenance));

  CoursePlaySession session;
  session.courseId = 23;
  session.courseName = "Course";
  session.courseGroupName = "Group";
  session.constraintJson = "{}";
  session.entries.push_back({.meta = meta});
  assert(first.SaveCourseScore(session, state, 1, 1, provenance));

  auto firstDb = openDatabase(firstPath);
  assert(queryInt(firstDb.get(), "SELECT COUNT(*) FROM scores") == 1);
  assert(queryInt(firstDb.get(), "SELECT COUNT(*) FROM course_scores") == 1);
  assert(readStoredProvenance(firstDb.get(), "scores", 1) == provenance);
  assert(readStoredProvenance(firstDb.get(), "course_scores", 1) == provenance);
  for (const std::string table : {"scores", "course_scores"}) {
    assert(queryInt(firstDb.get(),
                    "SELECT ruleset_version FROM " + table + " WHERE id=1") ==
           RulesetDescriptor::kCurrentVersion);
    assert(queryInt(firstDb.get(),
                    "SELECT eligibility FROM " + table + " WHERE id=1") ==
           static_cast<int>(ScoreEligibility::Verified));
  }

  auto secondDb = openDatabase(secondPath);
  assert(queryInt(secondDb.get(), "SELECT COUNT(*) FROM scores") == 0);
  assert(queryInt(secondDb.get(), "SELECT COUNT(*) FROM course_scores") == 0);

  ScoreDBHelper retargetable;
  const std::uint64_t revisionBefore = retargetable.GetRevision();
  retargetable.SetDatabasePath(firstPath);
  assert(retargetable.GetDatabasePath() == firstPath);
  assert(retargetable.GetRevision() > revisionBefore);
}

void testModifiedPlaybackDoesNotUpdateBestScores(
    const std::filesystem::path &root) {
  const auto path = root / "assisted-clear-cap" / "score.db";
  ScoreDBHelper helper(path);
  assert(helper.EnsureSchema());

  const auto meta = sampleMeta(root, "assisted-clear-cap");
  RhythmState state = sampleState(meta.TotalNotes, 0);
  state.comboBreak = 0;
  state.maxCombo = meta.TotalNotes;
  state.currentGauge = 100.0f;
  state.setAssistClearMark(true);

  ScoreProvenance provenance = sampleProvenance("assisted-clear-cap");
  provenance.playback = {.percent = 75,
                         .mode = audio::PlaybackMode::PitchShift};
  provenance.eligibility = ScoreEligibility::Modified;
  assert(helper.SaveScore(meta, state, provenance));

  CoursePlaySession session;
  session.courseId = 91;
  session.courseName = "Assisted Course";
  session.constraintJson = "{}";
  session.longNoteMode = meta.LnMode;
  session.entries.push_back({.meta = meta});
  session.courseKey = course_identity::makeCourseKey(session);
  assert(helper.SaveCourseScore(session, state, 1, 1, provenance));

  auto db = openDatabase(path);
  assert(queryInt(db.get(), "SELECT clear_type FROM scores") ==
         kClearTypeAssistedEasyClearRank);
  assert(queryInt(db.get(), "SELECT clear_type FROM course_scores") ==
         kClearTypeAssistedEasyClearRank);
  db.reset();

  const ScoreClearRankCache cache = helper.LoadBestClearRanks();
  assert(cache.bestRankFor(meta) == kNoClearTypeRank);
  assert(cache.bestCourseRankFor(session.courseKey, session.courseId,
                                 meta.LnMode) == kNoClearTypeRank);
  assert(!helper.LoadBestScore(meta).has_value());
  assert(!helper.LoadBestCourseScore(session).has_value());
}

void testVersion8MigrationReclassifiesBeatorajaValidScores(
    const std::filesystem::path &root) {
  const auto path = root / "migration-v8-beatoraja-eligibility" / "score.db";
  ScoreDBHelper initial(path);
  assert(initial.EnsureSchema());

  const auto meta = sampleMeta(root, "migration-v8-beatoraja-eligibility");
  const auto state = sampleState(20, 5);

  ScoreProvenance chartProvenance = sampleProvenance("migration-v8-chart");
  chartProvenance.gaugeAutoShift = GaugeAutoShiftMode::SurvivalToGroove;
  chartProvenance.gaugeAutoShiftLowerBound = GaugeType::Hard;
  chartProvenance.judgeWindowScalePercent = 80;
  chartProvenance.eligibility = ScoreEligibility::Modified;
  assert(initial.SaveScore(meta, state, chartProvenance));

  CoursePlaySession session;
  session.courseId = 92;
  session.courseName = "Beatoraja-valid course";
  session.constraintJson = "{}";
  session.longNoteMode = meta.LnMode;
  session.entries.push_back({.meta = meta});
  session.courseKey = course_identity::makeCourseKey(session);

  ScoreProvenance courseProvenance = sampleProvenance("migration-v8-course");
  courseProvenance.gaugeProfile = GaugeProfile::CourseDefault;
  courseProvenance.gaugeAutoShift = GaugeAutoShiftMode::Continue;
  courseProvenance.stages.front().judgeRankSource =
      JudgeRankSource::CourseConstraint;
  courseProvenance.eligibility = ScoreEligibility::Modified;
  assert(initial.SaveCourseScore(session, state, 1, 1, courseProvenance));

  assert(!initial.LoadBestScore(meta).has_value());
  assert(!initial.LoadBestCourseScore(session).has_value());
  initial.Shutdown();

  {
    auto db = openDatabase(path);
    assert(queryInt(db.get(), "SELECT COUNT(*) FROM scores") == 1);
    assert(queryInt(db.get(), "SELECT COUNT(*) FROM course_scores") == 1);
    removeAttemptIdentityFromCurrentSchema(db.get());
    execOrAbort(db.get(), "PRAGMA user_version = 7");
  }

  ScoreDBHelper migrated(path);
  assert(migrated.EnsureSchema());
  assert(migrated.LoadBestScore(meta).has_value());
  assert(migrated.LoadBestCourseScore(session).has_value());

  auto db = openDatabase(path);
  assert(queryInt(db.get(), "PRAGMA user_version") ==
         ScoreDBHelper::kCurrentSchemaVersion);
  for (const std::string table : {"scores", "course_scores"}) {
    assert(queryInt(db.get(),
                    "SELECT eligibility FROM " + table + " WHERE id=1") ==
           static_cast<int>(ScoreEligibility::Verified));
    assert(readStoredProvenance(db.get(), table, 1).eligibility ==
           ScoreEligibility::Verified);
  }
}

void testFutureVersionRejectsWithoutSchemaMutation(
    const std::filesystem::path &root) {
  const auto path = root / "future" / "score.db";
  auto db = openDatabase(path);
  execOrAbort(db.get(), "CREATE TABLE sentinel(value TEXT)");
  execOrAbort(db.get(), "INSERT INTO sentinel VALUES ('unchanged')");
  execOrAbort(db.get(), "PRAGMA user_version = 99");
  const std::string before = schemaSnapshot(db.get());
  db.reset();

  ScoreDBHelper helper(path);
  assert(!helper.EnsureSchema());

  auto after = openDatabase(path);
  assert(queryInt(after.get(), "PRAGMA user_version") == 99);
  assert(schemaSnapshot(after.get()) == before);
  assert(queryText(after.get(), "SELECT value FROM sentinel") == "unchanged");
  assert(!tableExists(after.get(), "scores"));
  assert(!tableExists(after.get(), "course_scores"));
}

void testLegacyPublicWriteEntryPointsEnsureUnifiedSchema(
    const std::filesystem::path &root) {
  const auto path = root / "legacy-public-api" / "score.db";
  ScoreDBHelper helper(path);
  SqliteConnectionHandle db(helper.Connect());
  assert(db);
  assert(helper.CreateScoreTable(db.get()));
  assert(helper.CreateCourseScoreTable(db.get()));

  const auto meta = sampleMeta(root, "legacy-public-api");
  const auto state = sampleState(12, 3);
  CoursePlaySession session;
  session.courseId = 51;
  session.courseName = "Legacy public API course";
  session.entries.push_back({.meta = meta});

  assert(helper.InsertScore(db.get(), meta, state));
  assert(helper.InsertCourseScore(db.get(), session, state, 1, 1));
  assert(queryInt(db.get(), "PRAGMA user_version") ==
         ScoreDBHelper::kCurrentSchemaVersion);
  assert(queryInt(db.get(), "SELECT COUNT(*) FROM scores") == 1);
  assert(queryInt(db.get(), "SELECT COUNT(*) FROM course_scores") == 1);
}

void testFutureVersionRejectsDirectScoreWrites(
    const std::filesystem::path &root) {
  const auto path = root / "future-direct-write" / "score.db";
  ScoreDBHelper helper(path);
  assert(helper.EnsureSchema());

  SqliteConnectionHandle db(helper.Connect());
  assert(db);
  execOrAbort(db.get(), "PRAGMA user_version = 99");
  const std::string schemaBefore = schemaSnapshot(db.get());

  const auto meta = sampleMeta(root, "future-direct-write");
  const auto state = sampleState(9, 1);
  CoursePlaySession session;
  session.courseId = 52;
  session.courseName = "Future direct write course";
  session.entries.push_back({.meta = meta});

  assert(!helper.InsertScore(db.get(), meta, state));
  assert(!helper.InsertCourseScore(db.get(), session, state, 1, 1));
  assert(queryInt(db.get(), "PRAGMA user_version") == 99);
  assert(schemaSnapshot(db.get()) == schemaBefore);
  assert(queryInt(db.get(), "SELECT COUNT(*) FROM scores") == 0);
  assert(queryInt(db.get(), "SELECT COUNT(*) FROM course_scores") == 0);
}

void testInvalidProvenanceRejectsScoreWrites(
    const std::filesystem::path &root) {
  const auto path = root / "invalid-provenance-write" / "score.db";
  ScoreDBHelper helper(path);
  assert(helper.EnsureSchema());

  const auto meta = sampleMeta(root, "invalid-provenance-write");
  const auto state = sampleState(11, 2);
  CoursePlaySession session;
  session.courseId = 53;
  session.courseName = "Invalid provenance course";
  session.entries.push_back({.meta = meta});

  const auto assertRejectedWithoutRows = [&](const ScoreProvenance &value) {
    const std::uint64_t revisionBefore = helper.GetRevision();
    assert(!helper.SaveScore(meta, state, value));
    assert(!helper.SaveCourseScore(session, state, 1, 1, value));
    assert(helper.GetRevision() == revisionBefore);

    auto db = openDatabase(path);
    assert(queryInt(db.get(), "SELECT COUNT(*) FROM scores") == 0);
    assert(queryInt(db.get(), "SELECT COUNT(*) FROM course_scores") == 0);
  };

  auto futureSchema = sampleProvenance("future-schema");
  futureSchema.schemaVersion = ScoreProvenance::kSchemaVersion + 1;
  assertRejectedWithoutRows(futureSchema);

  auto futureRuleset = sampleProvenance("future-ruleset");
  futureRuleset.ruleset.version = RulesetDescriptor::kCurrentVersion + 1;
  assertRejectedWithoutRows(futureRuleset);

  auto invalidEnum = sampleProvenance("invalid-enum");
  invalidEnum.gaugeType = static_cast<GaugeType>(999);
  assertRejectedWithoutRows(invalidEnum);
}

void testInvalidProvenanceDoesNotCreateScoreDatabase(
    const std::filesystem::path &root) {
  auto invalid = sampleProvenance("no-database");
  invalid.schemaVersion = ScoreProvenance::kSchemaVersion + 1;
  const auto meta = sampleMeta(root, "no-database");
  const auto state = sampleState(7, 1);

  const auto chartPath = root / "invalid-chart-no-database" / "score.db";
  ScoreDBHelper chartHelper(chartPath);
  assert(!chartHelper.SaveScore(meta, state, invalid));
  assert(!std::filesystem::exists(chartPath));

  CoursePlaySession session;
  session.courseId = 54;
  session.courseName = "Invalid provenance no database";
  session.entries.push_back({.meta = meta});
  const auto coursePath = root / "invalid-course-no-database" / "score.db";
  ScoreDBHelper courseHelper(coursePath);
  assert(!courseHelper.SaveCourseScore(session, state, 1, 1, invalid));
  assert(!std::filesystem::exists(coursePath));
}

void testPublicWritesNestInsideCallerTransactions(
    const std::filesystem::path &root) {
  const auto meta = sampleMeta(root, "nested-public-write");
  const auto state = sampleState(13, 2);
  CoursePlaySession session;
  session.courseId = 55;
  session.courseName = "Nested public write course";
  session.entries.push_back({.meta = meta});

  const auto prepareLegacySchema = [](ScoreDBHelper &helper) {
    SqliteConnectionHandle db(helper.Connect());
    assert(db);
    assert(helper.CreateScoreTable(db.get()));
    assert(helper.CreateCourseScoreTable(db.get()));
    assert(queryInt(db.get(), "PRAGMA user_version") == 4);
    return db;
  };

  {
    ScoreDBHelper helper(root / "nested-chart-commit" / "score.db");
    auto db = prepareLegacySchema(helper);
    execOrAbort(db.get(), "BEGIN IMMEDIATE TRANSACTION");
    assert(helper.InsertScore(db.get(), meta, state));
    assert(queryInt(db.get(), "PRAGMA user_version") ==
           ScoreDBHelper::kCurrentSchemaVersion);
    assert(queryInt(db.get(), "SELECT COUNT(*) FROM scores") == 1);
    execOrAbort(db.get(), "COMMIT");
    assert(queryInt(db.get(), "PRAGMA user_version") ==
           ScoreDBHelper::kCurrentSchemaVersion);
    assert(queryInt(db.get(), "SELECT COUNT(*) FROM scores") == 1);
  }

  {
    ScoreDBHelper helper(root / "nested-chart-rollback" / "score.db");
    auto db = prepareLegacySchema(helper);
    execOrAbort(db.get(), "BEGIN IMMEDIATE TRANSACTION");
    assert(helper.InsertScore(db.get(), meta, state));
    assert(queryInt(db.get(), "PRAGMA user_version") ==
           ScoreDBHelper::kCurrentSchemaVersion);
    assert(queryInt(db.get(), "SELECT COUNT(*) FROM scores") == 1);
    execOrAbort(db.get(), "ROLLBACK");
    assert(queryInt(db.get(), "PRAGMA user_version") == 4);
    assert(queryInt(db.get(), "SELECT COUNT(*) FROM scores") == 0);
    assert(!columnExists(db.get(), "scores", "provenance_json"));
  }

  {
    ScoreDBHelper helper(root / "nested-course-commit" / "score.db");
    auto db = prepareLegacySchema(helper);
    execOrAbort(db.get(), "BEGIN IMMEDIATE TRANSACTION");
    assert(helper.InsertCourseScore(db.get(), session, state, 1, 1));
    assert(queryInt(db.get(), "PRAGMA user_version") ==
           ScoreDBHelper::kCurrentSchemaVersion);
    assert(queryInt(db.get(), "SELECT COUNT(*) FROM course_scores") == 1);
    execOrAbort(db.get(), "COMMIT");
    assert(queryInt(db.get(), "PRAGMA user_version") ==
           ScoreDBHelper::kCurrentSchemaVersion);
    assert(queryInt(db.get(), "SELECT COUNT(*) FROM course_scores") == 1);
  }

  {
    ScoreDBHelper helper(root / "nested-course-rollback" / "score.db");
    auto db = prepareLegacySchema(helper);
    execOrAbort(db.get(), "BEGIN IMMEDIATE TRANSACTION");
    assert(helper.InsertCourseScore(db.get(), session, state, 1, 1));
    assert(queryInt(db.get(), "PRAGMA user_version") ==
           ScoreDBHelper::kCurrentSchemaVersion);
    assert(queryInt(db.get(), "SELECT COUNT(*) FROM course_scores") == 1);
    execOrAbort(db.get(), "ROLLBACK");
    assert(queryInt(db.get(), "PRAGMA user_version") == 4);
    assert(queryInt(db.get(), "SELECT COUNT(*) FROM course_scores") == 0);
    assert(!columnExists(db.get(), "course_scores", "provenance_json"));
  }
}

void testFutureScoreWritesPreservePersistentDatabaseState(
    const std::filesystem::path &root) {
  const auto meta = sampleMeta(root, "future-persistent-state");
  const auto state = sampleState(8, 2);
  const auto provenance = sampleProvenance("future-persistent-state");
  CoursePlaySession session;
  session.courseId = 56;
  session.courseName = "Future persistent state course";
  session.entries.push_back({.meta = meta});

  const auto assertUnchanged = [&](const std::filesystem::path &path,
                                   const auto &operation) {
    createFutureSentinelDatabase(path);
    const auto before = persistentDatabaseSnapshot(path);
    operation(path);
    const auto after = persistentDatabaseSnapshot(path);
    assert(after == before);
  };

  assertUnchanged(root / "future-save-score" / "score.db",
                  [&](const auto &path) {
                    ScoreDBHelper helper(path);
                    assert(!helper.SaveScore(meta, state, provenance));
                  });
  assertUnchanged(
      root / "future-save-course-score" / "score.db", [&](const auto &path) {
        ScoreDBHelper helper(path);
        assert(!helper.SaveCourseScore(session, state, 1, 1, provenance));
      });
  assertUnchanged(root / "future-direct-score" / "score.db",
                  [&](const auto &path) {
                    ScoreDBHelper helper(path);
                    SqliteConnectionHandle db(helper.Connect());
                    assert(!db);
                  });
  assertUnchanged(root / "future-direct-course-score" / "score.db",
                  [&](const auto &path) {
                    ScoreDBHelper helper(path);
                    SqliteConnectionHandle db(helper.Connect());
                    assert(!db);
                  });
}

void testFutureScorePreflightPreservesRawDatabaseFamily(
    const std::filesystem::path &root) {
#if !TARGET_OS_WINDOWS
  const auto meta = sampleMeta(root, "future-raw-preflight");
  const auto state = sampleState(8, 2);
  const auto provenance = sampleProvenance("future-raw-preflight");
  CoursePlaySession session;
  session.courseId = 57;
  session.courseName = "Future raw preflight course";
  session.entries.push_back({.meta = meta});

  const auto assertRejectedWithoutFamilyMutation =
      [&](FutureDatabaseState databaseState, const std::string &operationName,
          const auto &operation) {
        const auto path =
            root / "future-raw-score" /
            (operationName + "-" + futureDatabaseStateName(databaseState)) /
            "score.db";
        createFutureDatabaseState(path, databaseState);
        const auto before = rawDatabaseFamilySnapshot(path);
        const bool rejected = operation(path);
        const auto after = rawDatabaseFamilySnapshot(path);
        assert(after == before);
        assert(rejected);
      };

  constexpr std::array states = {FutureDatabaseState::WalAfterExit,
                                 FutureDatabaseState::HotJournal,
                                 FutureDatabaseState::Delete};
  for (const FutureDatabaseState databaseState : states) {
    assertRejectedWithoutFamilyMutation(
        databaseState, "connect", [&](const auto &path) {
          ScoreDBHelper helper(path);
          SqliteConnectionHandle db(helper.Connect());
          return !db;
        });
    assertRejectedWithoutFamilyMutation(
        databaseState, "save-chart", [&](const auto &path) {
          ScoreDBHelper helper(path);
          return !helper.SaveScore(meta, state, provenance);
        });
    assertRejectedWithoutFamilyMutation(
        databaseState, "save-course", [&](const auto &path) {
          ScoreDBHelper helper(path);
          return !helper.SaveCourseScore(session, state, 1, 1, provenance);
        });
  }

  const auto directPath =
      root / "future-raw-score" / "direct-live-wal" / "score.db";
  auto directDb = openDatabase(directPath);
  execOrAbort(directDb.get(), "PRAGMA journal_mode=WAL");
  execOrAbort(directDb.get(), "PRAGMA wal_autocheckpoint=0");
  execOrAbort(directDb.get(), "CREATE TABLE sentinel(value TEXT)");
  execOrAbort(directDb.get(), "INSERT INTO sentinel VALUES ('unchanged')");
  execOrAbort(directDb.get(), "PRAGMA user_version=99");
  const auto directBefore = rawDatabaseFamilySnapshot(directPath);
  ScoreDBHelper directHelper(directPath);
  assert(!directHelper.InsertScore(directDb.get(), meta, state, provenance));
  assert(!directHelper.InsertCourseScore(directDb.get(), session, state, 1, 1,
                                         provenance));
  const auto directAfter = rawDatabaseFamilySnapshot(directPath);
  // Reading a live WAL database may update transient reader marks in -shm.
  // Durable files and application-visible state must remain unchanged.
  assert(sameDatabaseFamilyIgnoringWalSharedMemoryBytes(directAfter,
                                                        directBefore));
  assert(queryInt(directDb.get(), "PRAGMA user_version") == 99);
  assert(queryText(directDb.get(), "SELECT value FROM sentinel") ==
         "unchanged");
  assert(!tableExists(directDb.get(), "scores"));
  assert(!tableExists(directDb.get(), "course_scores"));
#else
  (void)root;
#endif
}

void testScorePreflightRejectsMalformedStatesAndAllowsCreation(
    const std::filesystem::path &root) {
  {
    const auto path = root / "preflight-negative-version" / "score.db";
    auto db = openDatabase(path);
    execOrAbort(db.get(), "PRAGMA user_version=-1");
    db.reset();
    const auto before = rawDatabaseFamilySnapshot(path);
    ScoreDBHelper helper(path);
    SqliteConnectionHandle connection(helper.Connect());
    assert(!connection);
    assert(rawDatabaseFamilySnapshot(path) == before);
  }

  {
    const auto path = root / "preflight-stale-shm" / "score.db";
    createFutureSentinelDatabase(path);
    std::ofstream staleShm(path.string() + "-shm", std::ios::binary);
    staleShm << "stale-shm-without-wal";
    staleShm.close();
    const auto before = rawDatabaseFamilySnapshot(path);
    ScoreDBHelper helper(path);
    SqliteConnectionHandle connection(helper.Connect());
    assert(!connection);
    assert(rawDatabaseFamilySnapshot(path) == before);
  }

  {
    const auto path = root / "preflight-zero-byte" / "score.db";
    std::filesystem::create_directories(path.parent_path());
    std::ofstream empty(path, std::ios::binary);
    empty.close();
    ScoreDBHelper helper(path);
    assert(helper.EnsureSchema());
    assert(std::filesystem::file_size(path) > 0);
  }

  {
    const auto path = root / "preflight-missing" / "score.db";
    ScoreDBHelper helper(path);
    assert(helper.EnsureSchema());
    assert(std::filesystem::exists(path));
  }

#if !TARGET_OS_WINDOWS
  {
    const auto path = root / "preflight-live-writer" / "score.db";
    withLiveWalWriter(path, 6, [&] {
      const auto before = rawDatabaseFamilySnapshot(path);
      ScoreDBHelper helper(path);
      SqliteConnectionHandle connection(helper.Connect());
      const auto after = rawDatabaseFamilySnapshot(path);
      assert(!connection);
      assert(after == before);
    });
  }
#endif
}

int denyJournalModeAuthorizer(void *, int action, const char *first,
                              const char *, const char *, const char *) {
  return action == SQLITE_PRAGMA && first != nullptr &&
                 std::string_view(first) == "journal_mode"
             ? SQLITE_DENY
             : SQLITE_OK;
}

int denyUserVersionAuthorizer(void *, int action, const char *first,
                              const char *, const char *, const char *) {
  return action == SQLITE_PRAGMA && first != nullptr &&
                 std::string_view(first) == "user_version"
             ? SQLITE_DENY
             : SQLITE_OK;
}

int installDenyJournalMode(sqlite3 *db, char **, const sqlite3_api_routines *) {
  return sqlite3_set_authorizer(db, denyJournalModeAuthorizer, nullptr);
}

class ScopedDenyJournalModeAutoExtension {
public:
  ScopedDenyJournalModeAutoExtension() {
    sqlite3_reset_auto_extension();
    assert(sqlite3_auto_extension(reinterpret_cast<void (*)()>(
               installDenyJournalMode)) == SQLITE_OK);
  }
  ~ScopedDenyJournalModeAutoExtension() { sqlite3_reset_auto_extension(); }
};

void testEquivalentScoreAliasesRetainValidatedSession(
    const std::filesystem::path &root) {
#if !TARGET_OS_WINDOWS
  const auto path = root / "equivalent-alias" / "score.db";
  const auto hardLink = root / "equivalent-alias" / "score-hard-link.db";
  const auto symlink = root / "equivalent-alias" / "score-symlink.db";
  ScoreDBHelper helper(path);
  assert(helper.EnsureSchema());

  std::error_code linkError;
  std::filesystem::create_hard_link(path, hardLink, linkError);
  assert(!linkError);
  {
    ScopedDenyJournalModeAutoExtension denyJournalMode;
    std::string error;
    assert(helper.BindDatabasePath(hardLink, error));
  }
  assert(helper.GetDatabasePath() == hardLink);

  std::filesystem::create_symlink(hardLink, symlink, linkError);
  assert(!linkError);
  {
    ScopedDenyJournalModeAutoExtension denyJournalMode;
    helper.SetDatabasePath(symlink);
    assert(helper.EnsureSchema());
  }
  assert(helper.GetDatabasePath() == symlink);
#else
  (void)root;
#endif
}

void writeHeaderShapedCorruptDatabase(const std::filesystem::path &path) {
  std::filesystem::create_directories(path.parent_path());
  std::string bytes(4096, '\0');
  constexpr std::string_view magic("SQLite format 3\0", 16);
  std::copy(magic.begin(), magic.end(), bytes.begin());
  bytes[16] = 0x10;
  bytes[17] = 0;
  bytes[18] = 1;
  bytes[19] = 1;
  std::ofstream output(path, std::ios::binary);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  output.close();
  assert(output);
}

void testScoreOwnedOpenRejectsRecoveryAndConfigurationRaces(
    const std::filesystem::path &root) {
#if !TARGET_OS_WINDOWS
  {
    const auto path = root / "wal-without-shm-supported" / "score.db";
    createWalDatabaseWithVersion(path, 6);
    assert(std::filesystem::remove(path.string() + "-shm"));
    const auto before = rawDatabaseFamilySnapshot(path);
    ScoreDBHelper helper(path);
    SqliteConnectionHandle connection(helper.Connect());
    assert(connection);
    connection.reset();
    assert(rawDatabaseFamilySnapshot(path) == before);
  }

  {
    const auto path = root / "wal-without-shm-future" / "score.db";
    createWalDatabaseWithVersion(path, 99);
    assert(std::filesystem::remove(path.string() + "-shm"));
    const auto before = rawDatabaseFamilySnapshot(path);
    ScoreDBHelper helper(path);
    SqliteConnectionHandle connection(helper.Connect());
    assert(!connection);
    assert(rawDatabaseFamilySnapshot(path) == before);
  }
#endif

  {
    const auto path = root / "header-shaped-corrupt" / "score.db";
    writeHeaderShapedCorruptDatabase(path);
    const auto before = rawDatabaseFamilySnapshot(path);
    ScoreDBHelper helper(path);
    SqliteConnectionHandle connection(helper.Connect());
    assert(!connection);
    assert(rawDatabaseFamilySnapshot(path) == before);
  }

  {
    const auto path = root / "required-pragma-error" / "score.db";
    auto db = openDatabase(path);
    execOrAbort(db.get(), "CREATE TABLE sentinel(value TEXT)");
    execOrAbort(db.get(), "PRAGMA user_version=8");
    db.reset();
    const auto before = rawDatabaseFamilySnapshot(path);
    ScopedDenyJournalModeAutoExtension denyJournalMode;
    ScoreDBHelper helper(path);
    SqliteConnectionHandle connection(helper.Connect());
    assert(!connection);
    assert(rawDatabaseFamilySnapshot(path) == before);
  }
}

#if !TARGET_OS_WINDOWS
struct OwnedOpenRaceContext {
  std::filesystem::path path;
  std::optional<RawDatabaseFamilySnapshot> afterWriter;
};

void commitFutureVersionAfterSnapshot(void *rawContext) {
  auto &context = *static_cast<OwnedOpenRaceContext *>(rawContext);
  const pid_t child = fork();
  assert(child >= 0);
  if (child == 0) {
    executeChildSqlAndExit(context.path, "PRAGMA wal_autocheckpoint=0;"
                                         "PRAGMA user_version=99;");
  }
  waitForSuccessfulChild(child);
  context.afterWriter = rawDatabaseFamilySnapshot(context.path);
}

bool failNoCheckpointConfiguration(sqlite3 *, void *, std::string &error) {
  error = "injected no-checkpoint failure";
  return false;
}
#endif

void testOwnedOpenClosesTheApprovalGapAndBoundsSnapshots(
    const std::filesystem::path &root) {
  static_assert(sqliteSnapshotFitsBudget(
      kMaximumSqliteSnapshotBytes - kSqliteSnapshotAuxiliaryReserveBytes, 0));
  static_assert(!sqliteSnapshotFitsBudget(
      kMaximumSqliteSnapshotBytes - kSqliteSnapshotAuxiliaryReserveBytes + 1,
      0));
  static_assert(!sqliteSnapshotFitsBudget(kMaximumSqliteSnapshotBytes / 2,
                                          kMaximumSqliteSnapshotBytes / 2));
  static_assert(sqliteSnapshotFitsBudget(
      kMaximumSqliteSnapshotBytes / 2 - kSqliteSnapshotAuxiliaryReserveBytes,
      kMaximumSqliteSnapshotBytes / 2));
  static_assert(!sqliteSnapshotFitsBudget(
      kMaximumSqliteSnapshotBytes / 2 - kSqliteSnapshotAuxiliaryReserveBytes,
      kMaximumSqliteSnapshotBytes / 2 + 1));
#if !TARGET_OS_WINDOWS
  {
    const auto path = root / "snapshot-budget-overflow" / "score.db";
    auto db = openDatabase(path);
    execOrAbort(db.get(), "CREATE TABLE sentinel(value TEXT)");
    db.reset();
    std::filesystem::resize_file(path,
                                 kMaximumSqliteSnapshotBytes -
                                     kSqliteSnapshotAuxiliaryReserveBytes + 1);

    std::string stateError;
    const auto before = readSqliteDatabaseFamilyState(path, stateError);
    assert(before.has_value());
    std::string preflightError;
    assert(!preflightSqliteUserVersion(path, 6, preflightError).has_value());
    assert(preflightError.find("snapshot budget") != std::string::npos);
    const auto after = readSqliteDatabaseFamilyState(path, stateError);
    assert(after == before);
  }

  {
    const auto path = root / "owned-open-race" / "score.db";
    createWalDatabaseWithVersion(path, 6);
    OwnedOpenRaceContext context{.path = path};
    SqliteValidatedOpenHooks hooks;
    hooks.context = &context;
    hooks.afterSnapshotValidated = commitFutureVersionAfterSnapshot;
    std::string error;
    SqliteConnectionHandle connection(
        openValidatedSqliteDatabase(path, 6, false, error, hooks));
    assert(!connection);
    assert(context.afterWriter.has_value());
    assert(rawDatabaseFamilySnapshot(path) == *context.afterWriter);
  }

  {
    const auto path = root / "wal-without-shm-config-error" / "score.db";
    createWalDatabaseWithVersion(path, 6);
    assert(std::filesystem::remove(path.string() + "-shm"));
    const auto before = rawDatabaseFamilySnapshot(path);
    SqliteValidatedOpenHooks hooks;
    hooks.configureNoCheckpoint = failNoCheckpointConfiguration;
    std::string error;
    SqliteConnectionHandle connection(
        openValidatedSqliteDatabase(path, 6, false, error, hooks));
    assert(!connection);
    assert(rawDatabaseFamilySnapshot(path) == before);
  }
#else
  (void)root;
#endif
}

void testScoreTransactionalVersionErrorsDoNotMutateSchema(
    const std::filesystem::path &root) {
  {
    const auto path = root / "transaction-negative-version" / "score.db";
    auto db = openDatabase(path);
    execOrAbort(db.get(), "PRAGMA user_version=-1");
    const std::string beforeSchema = schemaSnapshot(db.get());
    execOrAbort(db.get(), "BEGIN TRANSACTION");
    ScoreDBHelper helper(path);
    assert(!helper.CreateScoreTable(db.get()));
    assert(queryInt(db.get(), "PRAGMA user_version") == -1);
    assert(schemaSnapshot(db.get()) == beforeSchema);
    execOrAbort(db.get(), "ROLLBACK");
  }

  {
    const auto path = root / "transaction-version-read-error" / "score.db";
    auto db = openDatabase(path);
    const std::string beforeSchema = schemaSnapshot(db.get());
    execOrAbort(db.get(), "BEGIN TRANSACTION");
    assert(sqlite3_set_authorizer(db.get(), denyUserVersionAuthorizer,
                                  nullptr) == SQLITE_OK);
    ScoreDBHelper helper(path);
    assert(!helper.CreateScoreTable(db.get()));
    assert(schemaSnapshot(db.get()) == beforeSchema);
    assert(sqlite3_set_authorizer(db.get(), nullptr, nullptr) == SQLITE_OK);
    execOrAbort(db.get(), "ROLLBACK");
  }
}

void testLargeWalPreflightPreservesFamily(const std::filesystem::path &root) {
#if !TARGET_OS_WINDOWS
  const auto walPath = root / "preflight-large-wal" / "score.db";
  createLargeFutureWalDatabase(walPath);
  const auto before = rawDatabaseFamilySnapshot(walPath);
  ScoreDBHelper helper(walPath);
  SqliteConnectionHandle connection(helper.Connect());
  assert(!connection);
  assert(rawDatabaseFamilySnapshot(walPath) == before);
#else
  (void)root;
#endif
}

} // namespace

int main() {
  const auto root = std::filesystem::temp_directory_path() /
                    "asobmashow_score_provenance_db_tests";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  testVersion8MigrationAddsAttemptIdentity(root);
  testStaleVersionRecognizesAppliedAttemptIdentity(root);
  testStaleVersionRejectsPartialAttemptIdentity(root);
  testLegacyNullAttemptScoresRemainRepeatable(root);
  testProjectedScoreIsIdempotent(root);
  testProjectedScoreConflictDoesNotMutateExistingRow(root);
  testProjectedScoreUsesReplayTimestamp(root);
  testProjectedRetryUpdatesSummaryCachesOnce(root);
  testPreviousBestExcludesExactAttemptAtSameTimestamp(root);
  testProjectedScoreValidatesStoredTypesAndCanonicalProvenance(root);
  testUnrelatedScoreConstraintIsStorageFailure(root);
  testVersion4MigrationPreservesOutcomesAndRows(root);
  testVersion5MigrationPreservesRawCourseEvidence(root);
  testVersion6MigrationIsSavepointSafeAndAtomic(root);
  testVersion7MigrationRepairsPopulatedScoreSummariesExactlyOnce(root);
  testCourseWritesUseAuthoritativeKeysAndExactMode(root);
  testCourseReadsAreKeyAndModeAuthoritative(root);
  testCourseLampCacheSeparatesKeysIdsAndModes(root);
  testCourseRecoveryUsesStrongestCommonEvidenceAndOwnsResult(root);
  testCourseRecoveryFailsClosedOnAmbiguityAndFailure(root);
  testIgnoredRecoveryUpdateDoesNotAdvanceRevision(root);
  testFutureVersionNinePlusOneIsRejected(root);
  testChartAndCourseRoundTripAndPathIsolation(root);
  testModifiedPlaybackDoesNotUpdateBestScores(root);
  testVersion8MigrationReclassifiesBeatorajaValidScores(root);
  testFutureVersionRejectsWithoutSchemaMutation(root);
  testLegacyPublicWriteEntryPointsEnsureUnifiedSchema(root);
  testFutureVersionRejectsDirectScoreWrites(root);
  testInvalidProvenanceRejectsScoreWrites(root);
  testInvalidProvenanceDoesNotCreateScoreDatabase(root);
  testPublicWritesNestInsideCallerTransactions(root);
  testFutureScoreWritesPreservePersistentDatabaseState(root);
  testFutureScorePreflightPreservesRawDatabaseFamily(root);
  testScorePreflightRejectsMalformedStatesAndAllowsCreation(root);
  testEquivalentScoreAliasesRetainValidatedSession(root);
  testScoreOwnedOpenRejectsRecoveryAndConfigurationRaces(root);
  testOwnedOpenClosesTheApprovalGapAndBoundsSnapshots(root);
  testScoreTransactionalVersionErrorsDoNotMutateSchema(root);
  testLargeWalPreflightPreservesFamily(root);

  std::filesystem::remove_all(root);
  std::cout << "score provenance database tests passed\n";
  return 0;
}
