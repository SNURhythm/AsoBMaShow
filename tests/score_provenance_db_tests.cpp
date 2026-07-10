#include "../src/ChartDBHelper.h"
#include "../src/CoursePlaySession.h"
#include "../src/ScoreDBHelper.h"
#include "../src/ScoreProvenance.h"
#include "../src/SqliteRAII.h"
#include "../src/targets.h"

#include <array>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#if !TARGET_OS_WINDOWS
#include <sys/stat.h>
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
  meta.MD5 = "md5-" + hash;
  meta.SHA256 = "sha-" + hash;
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
  assert(queryInt(migrated.get(), "PRAGMA user_version") == 5);
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
    assert(queryInt(firstDb.get(), "SELECT ruleset_version FROM " + table +
                                       " WHERE id=1") == 1);
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
  assert(queryInt(db.get(), "PRAGMA user_version") == 5);
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
    assert(queryInt(db.get(), "PRAGMA user_version") == 5);
    assert(queryInt(db.get(), "SELECT COUNT(*) FROM scores") == 1);
    execOrAbort(db.get(), "COMMIT");
    assert(queryInt(db.get(), "PRAGMA user_version") == 5);
    assert(queryInt(db.get(), "SELECT COUNT(*) FROM scores") == 1);
  }

  {
    ScoreDBHelper helper(root / "nested-chart-rollback" / "score.db");
    auto db = prepareLegacySchema(helper);
    execOrAbort(db.get(), "BEGIN IMMEDIATE TRANSACTION");
    assert(helper.InsertScore(db.get(), meta, state));
    assert(queryInt(db.get(), "PRAGMA user_version") == 5);
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
    assert(queryInt(db.get(), "PRAGMA user_version") == 5);
    assert(queryInt(db.get(), "SELECT COUNT(*) FROM course_scores") == 1);
    execOrAbort(db.get(), "COMMIT");
    assert(queryInt(db.get(), "PRAGMA user_version") == 5);
    assert(queryInt(db.get(), "SELECT COUNT(*) FROM course_scores") == 1);
  }

  {
    ScoreDBHelper helper(root / "nested-course-rollback" / "score.db");
    auto db = prepareLegacySchema(helper);
    execOrAbort(db.get(), "BEGIN IMMEDIATE TRANSACTION");
    assert(helper.InsertCourseScore(db.get(), session, state, 1, 1));
    assert(queryInt(db.get(), "PRAGMA user_version") == 5);
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
  assert(rawDatabaseFamilySnapshot(directPath) == directBefore);
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
    withLiveWalWriter(path, 5, [&] {
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

void testLargeWalPreflightUsesOnlySparseFirstPage(
    const std::filesystem::path &root) {
#if !TARGET_OS_WINDOWS
  const auto sourcePath = root / "preflight-sparse-source" / "score.db";
  {
    auto db = openDatabase(sourcePath);
    execOrAbort(db.get(), "CREATE TABLE padding(payload BLOB)");
    execOrAbort(db.get(),
                "WITH RECURSIVE counter(value) AS (VALUES(1) UNION ALL "
                "SELECT value + 1 FROM counter WHERE value < 512) "
                "INSERT INTO padding SELECT randomblob(4096) FROM counter");
  }
  const auto sourceSize = std::filesystem::file_size(sourcePath);
  assert(sourceSize > 2 * 1024 * 1024);
  std::string error;
  const auto pageSize = readRawSqlitePageSize(sourcePath, error);
  assert(pageSize.has_value());

  const auto snapshotPath = root / "preflight-sparse-copy" / "score.db";
  std::filesystem::create_directories(snapshotPath.parent_path());
  assert(writeSqliteFirstPageSnapshot(sourcePath, snapshotPath, *pageSize,
                                      sourceSize, error));
  assert(std::filesystem::file_size(snapshotPath) == sourceSize);

  std::vector<char> sourceFirstPage(*pageSize);
  std::vector<char> snapshotFirstPage(*pageSize);
  std::vector<char> sourceSecondPage(*pageSize);
  std::vector<char> snapshotSecondPage(*pageSize);
  std::ifstream source(sourcePath, std::ios::binary);
  std::ifstream snapshot(snapshotPath, std::ios::binary);
  assert(source.read(sourceFirstPage.data(), sourceFirstPage.size()));
  assert(snapshot.read(snapshotFirstPage.data(), snapshotFirstPage.size()));
  assert(sourceFirstPage == snapshotFirstPage);
  assert(source.read(sourceSecondPage.data(), sourceSecondPage.size()));
  assert(snapshot.read(snapshotSecondPage.data(), snapshotSecondPage.size()));
  assert(std::any_of(sourceSecondPage.begin(), sourceSecondPage.end(),
                     [](char value) { return value != 0; }));
  assert(std::all_of(snapshotSecondPage.begin(), snapshotSecondPage.end(),
                     [](char value) { return value == 0; }));

  struct stat snapshotStat{};
  assert(stat(snapshotPath.c_str(), &snapshotStat) == 0);
  const auto physicalBytes =
      static_cast<std::uintmax_t>(snapshotStat.st_blocks) * 512U;
  assert(physicalBytes < sourceSize / 8U);

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

  testVersion4MigrationPreservesOutcomesAndRows(root);
  testChartAndCourseRoundTripAndPathIsolation(root);
  testFutureVersionRejectsWithoutSchemaMutation(root);
  testLegacyPublicWriteEntryPointsEnsureUnifiedSchema(root);
  testFutureVersionRejectsDirectScoreWrites(root);
  testInvalidProvenanceRejectsScoreWrites(root);
  testInvalidProvenanceDoesNotCreateScoreDatabase(root);
  testPublicWritesNestInsideCallerTransactions(root);
  testFutureScoreWritesPreservePersistentDatabaseState(root);
  testFutureScorePreflightPreservesRawDatabaseFamily(root);
  testScorePreflightRejectsMalformedStatesAndAllowsCreation(root);
  testLargeWalPreflightUsesOnlySparseFirstPage(root);

  std::filesystem::remove_all(root);
  std::cout << "score provenance database tests passed\n";
  return 0;
}
