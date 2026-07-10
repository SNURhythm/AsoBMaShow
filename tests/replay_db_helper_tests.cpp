#include "../src/ReplayDBHelper.h"
#include "../src/ScoreProvenance.h"
#include "../src/SqliteRAII.h"
#include "../src/Utils.h"
#include "../src/targets.h"

#include <cassert>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

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

void createVersion2ReplayFixture(const std::filesystem::path &path) {
  auto db = openDatabase(path);
  execOrAbort(
      db.get(),
      "CREATE TABLE replays ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT, chart_path TEXT, chart_md5 TEXT,"
      "chart_sha256 TEXT, chart_title TEXT, chart_artist TEXT,"
      "ln_mode INTEGER NOT NULL DEFAULT 0, gauge_type INTEGER NOT NULL,"
      "gauge_auto_shift INTEGER NOT NULL, final_score INTEGER NOT NULL,"
      "max_combo INTEGER NOT NULL DEFAULT 0, final_gauge REAL NOT NULL,"
      "clear_type INTEGER NOT NULL, random_seed INTEGER, random_prng TEXT,"
      "random_values TEXT, play_option TEXT, play_option_seed INTEGER,"
      "play_option2 TEXT, play_option2_seed INTEGER, assist_option TEXT,"
      "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP)");
  execOrAbort(
      db.get(),
      "CREATE TABLE replay_events ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT, replay_id INTEGER NOT NULL,"
      "event_index INTEGER NOT NULL, action INTEGER NOT NULL, lane INTEGER NOT "
      "NULL,"
      "note_time_micros INTEGER NOT NULL, song_time_micros INTEGER NOT NULL,"
      "judge_time_micros INTEGER NOT NULL, judgement INTEGER NOT NULL,"
      "diff_micros INTEGER NOT NULL, gauge REAL NOT NULL,"
      "gauge_type INTEGER NOT NULL, combo INTEGER NOT NULL, score INTEGER NOT "
      "NULL,"
      "FOREIGN KEY(replay_id) REFERENCES replays(id) ON DELETE CASCADE)");
  execOrAbort(
      db.get(),
      "CREATE TABLE replay_touch_samples ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT, replay_id INTEGER NOT NULL,"
      "sample_index INTEGER NOT NULL, action INTEGER NOT NULL,"
      "finger_id INTEGER NOT NULL, song_time_micros INTEGER NOT NULL,"
      "x REAL NOT NULL, y REAL NOT NULL,"
      "FOREIGN KEY(replay_id) REFERENCES replays(id) ON DELETE CASCADE)");
  execOrAbort(
      db.get(),
      "CREATE TABLE replay_lane_cover_events ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT, replay_id INTEGER NOT NULL,"
      "event_index INTEGER NOT NULL, song_time_micros INTEGER NOT NULL,"
      "note_start_position_percent INTEGER NOT NULL,"
      "reset_visible_time_reference INTEGER NOT NULL,"
      "FOREIGN KEY(replay_id) REFERENCES replays(id) ON DELETE CASCADE)");
  execOrAbort(
      db.get(),
      "CREATE TABLE course_replays ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT, course_id INTEGER NOT NULL,"
      "course_name TEXT, course_group_name TEXT, constraint_json TEXT,"
      "gauge_type INTEGER NOT NULL, gauge_profile INTEGER NOT NULL DEFAULT 0,"
      "gauge_auto_shift INTEGER NOT NULL, ln_mode INTEGER NOT NULL DEFAULT 0,"
      "requested_play_option TEXT, assist_option TEXT, final_score INTEGER NOT "
      "NULL,"
      "max_combo INTEGER NOT NULL DEFAULT 0, final_gauge REAL NOT NULL,"
      "clear_type INTEGER NOT NULL, completed_charts INTEGER NOT NULL,"
      "total_charts INTEGER NOT NULL,"
      "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP)");
  execOrAbort(
      db.get(),
      "CREATE TABLE course_replay_stages ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT, course_replay_id INTEGER NOT NULL,"
      "stage_index INTEGER NOT NULL, replay_id INTEGER NOT NULL,"
      "rest_micros_after_stage INTEGER NOT NULL DEFAULT 0,"
      "FOREIGN KEY(course_replay_id) REFERENCES course_replays(id) ON DELETE "
      "CASCADE,"
      "FOREIGN KEY(replay_id) REFERENCES replays(id) ON DELETE CASCADE)");
  execOrAbort(
      db.get(),
      "INSERT INTO replays (chart_path, chart_md5, chart_sha256, chart_title,"
      "chart_artist, ln_mode, gauge_type, gauge_auto_shift, final_score,"
      "max_combo, final_gauge, clear_type, play_option, assist_option, "
      "created_at)"
      " VALUES ('BMS/legacy.bms','legacy-md5','legacy-sha','Legacy','Artist',"
      "2,2,1,1234,456,73.5,300,'MIRROR','OFF','2026-01-02 03:04:05')");
  execOrAbort(
      db.get(),
      "INSERT INTO replays (chart_path, chart_md5, chart_sha256, chart_title,"
      "chart_artist, ln_mode, gauge_type, gauge_auto_shift, final_score,"
      "max_combo, final_gauge, clear_type, play_option, assist_option, "
      "created_at)"
      " VALUES ('BMS/stage.bms','stage-md5','stage-sha','Stage','Artist',"
      "1,1,0,777,88,55.5,200,'NORMAL','OFF','2026-02-03 04:05:06')");
  execOrAbort(db.get(),
              "INSERT INTO replay_events (replay_id,event_index,action,lane,"
              "note_time_micros,song_time_micros,judge_time_micros,judgement,"
              "diff_micros,gauge,gauge_type,combo,score)"
              " VALUES (1,0,0,3,100000,100100,100050,0,-50,73.5,2,1,2)");
  execOrAbort(
      db.get(),
      "INSERT INTO course_replays (course_id,course_name,course_group_name,"
      "constraint_json,gauge_type,gauge_profile,gauge_auto_shift,ln_mode,"
      "requested_play_option,assist_option,final_score,max_combo,final_gauge,"
      "clear_type,completed_charts,total_charts,created_at)"
      " VALUES (17,'Legacy Course','Group','{}',2,1,1,2,'MIRROR','OFF',"
      "2345,321,61.25,200,1,2,'2026-03-04 05:06:07')");
  execOrAbort(db.get(),
              "INSERT INTO course_replay_stages "
              "(course_replay_id,stage_index,replay_id,rest_micros_after_stage)"
              " VALUES (1,0,2,250000)");
  execOrAbort(db.get(), "PRAGMA user_version = 2");
}

std::string replayOutcome(sqlite3 *db) {
  return queryText(
      db,
      "SELECT final_score || '|' || max_combo || '|' || final_gauge || '|' || "
      "clear_type || '|' || created_at FROM replays WHERE id=1");
}

std::string courseOutcome(sqlite3 *db) {
  return queryText(
      db,
      "SELECT final_score || '|' || max_combo || '|' || final_gauge || '|' || "
      "clear_type || '|' || completed_charts || '|' || total_charts || '|' || "
      "created_at FROM course_replays WHERE id=1");
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

ReplayData sampleReplay(const std::filesystem::path &root,
                        const std::string &hash) {
  ReplayData replay;
  replay.chartMeta.BmsPath = root / "BMS" / (hash + ".bms");
  replay.chartMeta.MD5 = "md5-" + hash;
  replay.chartMeta.SHA256 = "sha-" + hash;
  replay.chartMeta.Title = "Title " + hash;
  replay.chartMeta.Artist = "Artist";
  replay.chartMeta.TotalNotes = 50;
  replay.chartMeta.LnMode = 2;
  replay.initialGaugeType = GaugeType::Hard;
  replay.gaugeAutoShift = true;
  replay.finalScore = 91;
  replay.maxCombo = 45;
  replay.finalGauge = 82.5f;
  replay.clearType = kClearTypeHardClearRank;
  replay.playOption = "RANDOM";
  replay.playOptionSeed = 1234;
  replay.events.push_back({.action = ReplayEventAction::Press,
                           .lane = 3,
                           .noteTimeMicros = 100000,
                           .songTimeMicros = 100100,
                           .judgeTimeMicros = 100050,
                           .judgement = PGreat,
                           .diffMicros = -50,
                           .gauge = 82.5f,
                           .gaugeType = GaugeType::Hard,
                           .combo = 1,
                           .score = 2});
  replay.provenance = sampleProvenance(hash);
  return replay;
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

void testVersion2MigrationPreservesOutcomesAndRows(
    const std::filesystem::path &root) {
  const auto path = root / "migration" / "replay.db";
  createVersion2ReplayFixture(path);
  auto before = openDatabase(path);
  const std::string replayBefore = replayOutcome(before.get());
  const std::string courseBefore = courseOutcome(before.get());
  before.reset();

  ReplayDBHelper helper(path);
  assert(helper.GetDatabasePath() == path);
  assert(helper.EnsureSchema());

  auto migrated = openDatabase(path);
  assert(queryInt(migrated.get(), "PRAGMA user_version") == 3);
  assert(queryInt(migrated.get(), "SELECT COUNT(*) FROM replays") == 2);
  assert(queryInt(migrated.get(), "SELECT COUNT(*) FROM course_replays") == 1);
  assert(queryInt(migrated.get(), "SELECT COUNT(*) FROM replay_events") == 1);
  assert(queryInt(migrated.get(),
                  "SELECT COUNT(*) FROM course_replay_stages") == 1);
  assert(replayOutcome(migrated.get()) == replayBefore);
  assert(courseOutcome(migrated.get()) == courseBefore);
  for (const std::string table : {"replays", "course_replays"}) {
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
  assert(queryInt(second.get(), "SELECT COUNT(*) FROM replays") == 2);
  assert(queryInt(second.get(), "SELECT COUNT(*) FROM course_replays") == 1);
}

void testChartAndCourseRoundTripAndPathIsolation(
    const std::filesystem::path &root) {
  const auto firstPath = root / "profiles" / "one" / "replay.db";
  const auto secondPath = root / "profiles" / "two" / "replay.db";
  ReplayDBHelper first(firstPath);
  ReplayDBHelper second(secondPath);
  assert(first.EnsureSchema());
  assert(second.EnsureSchema());

  ReplayData replay = sampleReplay(root, "chart");
  const auto replayId = first.SaveReplay(replay);
  assert(replayId.has_value());
  const auto loaded = first.LoadReplay(*replayId, replay.chartMeta);
  assert(loaded.has_value());
  assert(loaded->provenance == replay.provenance);
  const auto summaries = first.ListReplays(replay.chartMeta, 0);
  assert(summaries.size() == 1);
  assert(summaries.front().rulesetVersion == 1);
  assert(summaries.front().eligibility == ScoreEligibility::Verified);

  CourseReplayData course;
  course.courseId = 31;
  course.courseName = "Course";
  course.courseGroupName = "Group";
  course.constraintJson = "{}";
  course.finalScore = replay.finalScore;
  course.maxCombo = replay.maxCombo;
  course.finalGauge = replay.finalGauge;
  course.clearType = replay.clearType;
  course.completedCharts = 1;
  course.totalCharts = 1;
  course.provenance = replay.provenance;
  course.stages.push_back(
      {.replay = sampleReplay(root, "stage"), .restMicrosAfterStage = 250000});
  const auto courseId = first.SaveCourseReplay(course);
  assert(courseId.has_value());
  const auto loadedCourse = first.LoadCourseReplay(*courseId);
  assert(loadedCourse.has_value());
  assert(loadedCourse->provenance == course.provenance);
  assert(loadedCourse->stages.size() == 1);
  assert(loadedCourse->stages.front().replay.provenance ==
         course.stages.front().replay.provenance);
  const auto courseSummaries = first.ListCourseReplays(course.courseId, 0);
  assert(courseSummaries.size() == 1);
  assert(courseSummaries.front().rulesetVersion == 1);
  assert(courseSummaries.front().eligibility == ScoreEligibility::Verified);

  assert(second.ListReplays(replay.chartMeta, 0).empty());
  assert(second.ListCourseReplays(course.courseId, 0).empty());
  assert(first.GetDatabasePath() == firstPath);
  assert(second.GetDatabasePath() == secondPath);

  ReplayDBHelper retargetable;
  retargetable.SetDatabasePath(firstPath);
  assert(retargetable.GetDatabasePath() == firstPath);
}

void testInvalidNewProvenanceFailsLoad(const std::filesystem::path &root) {
  const auto path = root / "invalid" / "replay.db";
  ReplayDBHelper helper(path);
  assert(helper.EnsureSchema());
  ReplayData replay = sampleReplay(root, "invalid");
  const auto replayId = helper.SaveReplay(replay);
  assert(replayId.has_value());

  auto db = openDatabase(path);
  execOrAbort(db.get(), "UPDATE replays SET provenance_json='{' WHERE id=" +
                            std::to_string(*replayId));
  db.reset();
  assert(!helper.LoadReplay(*replayId, replay.chartMeta).has_value());

  CourseReplayData course;
  course.courseId = 44;
  course.provenance = replay.provenance;
  course.stages.push_back({.replay = sampleReplay(root, "invalid-stage")});
  const auto courseId = helper.SaveCourseReplay(course);
  assert(courseId.has_value());
  db = openDatabase(path);
  execOrAbort(db.get(),
              "UPDATE course_replays SET provenance_json='{' WHERE id=" +
                  std::to_string(*courseId));
  db.reset();
  assert(!helper.LoadCourseReplay(*courseId).has_value());
}

void testFutureVersionRejectsWithoutSchemaMutation(
    const std::filesystem::path &root) {
  const auto path = root / "future" / "replay.db";
  auto db = openDatabase(path);
  execOrAbort(db.get(), "CREATE TABLE sentinel(value TEXT)");
  execOrAbort(db.get(), "INSERT INTO sentinel VALUES ('unchanged')");
  execOrAbort(db.get(), "PRAGMA user_version = 99");
  const std::string before = schemaSnapshot(db.get());
  db.reset();

  ReplayDBHelper helper(path);
  assert(!helper.EnsureSchema());

  auto after = openDatabase(path);
  assert(queryInt(after.get(), "PRAGMA user_version") == 99);
  assert(schemaSnapshot(after.get()) == before);
  assert(queryText(after.get(), "SELECT value FROM sentinel") == "unchanged");
  assert(!tableExists(after.get(), "replays"));
  assert(!tableExists(after.get(), "course_replays"));
}

void insertChartReplays(sqlite3 *db, int count) {
  for (int i = 1; i <= count; ++i) {
    execOrAbort(
        db,
        "INSERT INTO replays (chart_path, chart_md5, chart_sha256, "
        "chart_title, chart_artist, gauge_type, gauge_auto_shift, final_score,"
        "max_combo, final_gauge, clear_type, assist_option) VALUES "
        "('BMS/chart.bms','md5','sha','Title','Artist',0,0," +
            std::to_string(i) + "," + std::to_string(i) + ",100.0,300,'OFF')");
  }
}

void insertCourseReplays(sqlite3 *db, int courseId, int count) {
  for (int i = 1; i <= count; ++i) {
    execOrAbort(
        db, "INSERT INTO course_replays (course_id, course_name, "
            "course_group_name, constraint_json, gauge_type, gauge_profile,"
            "gauge_auto_shift, ln_mode, requested_play_option, assist_option,"
            "final_score, max_combo, final_gauge, clear_type, completed_charts,"
            "total_charts) VALUES (" +
                std::to_string(courseId) +
                ",'Course','Group','{}',0,0,0,0,'NORMAL','OFF'," +
                std::to_string(i) + "," + std::to_string(i) +
                ",100.0,300,1,1)");
  }
}

void testExistingListLimits(const std::filesystem::path &root) {
  const auto path = root / "limits" / "replay.db";
  ReplayDBHelper helper(path);
  assert(helper.EnsureSchema());
  auto db = openDatabase(path);
  insertChartReplays(db.get(), 105);
  insertCourseReplays(db.get(), 7, 105);
  db.reset();

  bms_parser::ChartMeta meta;
  meta.SHA256 = "sha";
  meta.MD5 = "md5";
  meta.BmsPath = root / "BMS" / "chart.bms";
  meta.TotalNotes = 500;
  assert(helper.ListReplays(meta).size() == 100);
  const auto allChart = helper.ListReplays(meta, 0);
  assert(allChart.size() == 105);
  assert(allChart.front().id == 105);
  assert(allChart.back().id == 1);
  assert(helper.ListCourseReplays(7).size() == 100);
  const auto allCourse = helper.ListCourseReplays(7, 0);
  assert(allCourse.size() == 105);
  assert(allCourse.front().id == 105);
  assert(allCourse.back().id == 1);
}

} // namespace

int main() {
#if TARGET_OS_WINDOWS
  return 0;
#endif

  const auto root = std::filesystem::temp_directory_path() /
                    "asobmashow_replay_db_helper_tests";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  testVersion2MigrationPreservesOutcomesAndRows(root);
  testChartAndCourseRoundTripAndPathIsolation(root);
  testInvalidNewProvenanceFailsLoad(root);
  testFutureVersionRejectsWithoutSchemaMutation(root);
  testExistingListLimits(root);

  std::filesystem::remove_all(root);
  std::cout << "replay database helper tests passed\n";
  return 0;
}
