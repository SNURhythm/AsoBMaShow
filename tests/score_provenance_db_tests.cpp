#include "../src/ChartDBHelper.h"
#include "../src/CoursePlaySession.h"
#include "../src/ScoreDBHelper.h"
#include "../src/ScoreProvenance.h"
#include "../src/SqliteRAII.h"

#include <cassert>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

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

  std::filesystem::remove_all(root);
  std::cout << "score provenance database tests passed\n";
  return 0;
}
