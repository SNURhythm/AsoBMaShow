#include "repositories/ReplayRepository.h"
#include "repositories/ReplayRepositoryMigrationTestAccess.h"
#include "repositories/SqliteRAII.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() /
           ("asobmashow-legacy-cutover-" + std::to_string(stamp));
    assert(std::filesystem::create_directories(path));
  }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }

  std::filesystem::path path;
};

SqliteConnectionHandle openDatabase(const std::filesystem::path &path) {
  sqlite3 *raw = nullptr;
  assert(sqlite3_open(path.string().c_str(), &raw) == SQLITE_OK);
  assert(sqlite3_exec(raw, "PRAGMA foreign_keys=ON", nullptr, nullptr,
                      nullptr) == SQLITE_OK);
  return SqliteConnectionHandle(raw);
}

void exec(sqlite3 *database, std::string_view sql) {
  char *error = nullptr;
  const std::string statement(sql);
  if (sqlite3_exec(database, statement.c_str(), nullptr, nullptr, &error) !=
      SQLITE_OK) {
    std::cerr << (error != nullptr ? error : "SQLite failure") << '\n';
    sqlite3_free(error);
    std::abort();
  }
}

int queryInt(sqlite3 *database, std::string_view sql) {
  SqliteStatementHandle statement;
  assert(prepareSqliteStatement(database, std::string(sql), statement) ==
         SQLITE_OK);
  assert(sqlite3_step(statement.get()) == SQLITE_ROW);
  const int value = sqlite3_column_int(statement.get(), 0);
  assert(sqlite3_step(statement.get()) == SQLITE_DONE);
  return value;
}

std::string queryText(sqlite3 *database, std::string_view sql) {
  SqliteStatementHandle statement;
  assert(prepareSqliteStatement(database, std::string(sql), statement) ==
         SQLITE_OK);
  assert(sqlite3_step(statement.get()) == SQLITE_ROW);
  const std::string value = sqliteColumnString(statement.get(), 0);
  assert(sqlite3_step(statement.get()) == SQLITE_DONE);
  return value;
}

bool tableExists(sqlite3 *database, std::string_view table) {
  SqliteStatementHandle statement;
  assert(prepareSqliteStatement(
             database,
             "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?",
             statement) == SQLITE_OK);
  assert(sqlite3_bind_text(statement.get(), 1, table.data(),
                           static_cast<int>(table.size()),
                           SQLITE_TRANSIENT) == SQLITE_OK);
  return sqlite3_step(statement.get()) == SQLITE_ROW;
}

std::string readFile(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

void createVersion2Fixture(const std::filesystem::path &path) {
  auto database = openDatabase(path);
  exec(database.get(),
       "CREATE TABLE replays("
       "id INTEGER PRIMARY KEY AUTOINCREMENT,chart_path TEXT,chart_md5 TEXT,"
       "chart_sha256 TEXT,chart_title TEXT,chart_artist TEXT,"
       "ln_mode INTEGER NOT NULL DEFAULT 0,gauge_type INTEGER NOT NULL,"
       "gauge_auto_shift INTEGER NOT NULL,final_score INTEGER NOT NULL,"
       "max_combo INTEGER NOT NULL DEFAULT 0,final_gauge REAL NOT NULL,"
       "clear_type INTEGER NOT NULL,random_seed INTEGER,random_prng TEXT,"
       "random_values TEXT,play_option TEXT,play_option_seed INTEGER,"
       "play_option2 TEXT,play_option2_seed INTEGER,assist_option TEXT,"
       "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP)");
  exec(database.get(),
       "CREATE TABLE replay_events("
       "id INTEGER PRIMARY KEY AUTOINCREMENT,replay_id INTEGER NOT NULL,"
       "event_index INTEGER NOT NULL,action INTEGER NOT NULL,lane INTEGER NOT "
       "NULL,"
       "note_time_micros INTEGER NOT NULL,song_time_micros INTEGER NOT NULL,"
       "judge_time_micros INTEGER NOT NULL,judgement INTEGER NOT NULL,"
       "diff_micros INTEGER NOT NULL,gauge REAL NOT NULL,gauge_type INTEGER "
       "NOT NULL,"
       "combo INTEGER NOT NULL,score INTEGER NOT NULL,"
       "FOREIGN KEY(replay_id) REFERENCES replays(id) ON DELETE CASCADE)");
  exec(database.get(),
       "CREATE TABLE replay_touch_samples("
       "id INTEGER PRIMARY KEY AUTOINCREMENT,replay_id INTEGER NOT NULL,"
       "sample_index INTEGER NOT NULL,action INTEGER NOT NULL,"
       "finger_id INTEGER NOT NULL,song_time_micros INTEGER NOT NULL,"
       "x REAL NOT NULL,y REAL NOT NULL,"
       "FOREIGN KEY(replay_id) REFERENCES replays(id) ON DELETE CASCADE)");
  exec(database.get(),
       "CREATE TABLE replay_lane_cover_events("
       "id INTEGER PRIMARY KEY AUTOINCREMENT,replay_id INTEGER NOT NULL,"
       "event_index INTEGER NOT NULL,song_time_micros INTEGER NOT NULL,"
       "note_start_position_percent INTEGER NOT NULL,"
       "reset_visible_time_reference INTEGER NOT NULL,"
       "FOREIGN KEY(replay_id) REFERENCES replays(id) ON DELETE CASCADE)");
  exec(database.get(),
       "CREATE TABLE course_replays("
       "id INTEGER PRIMARY KEY AUTOINCREMENT,course_id INTEGER NOT NULL,"
       "course_name TEXT,course_group_name TEXT,constraint_json TEXT,"
       "gauge_type INTEGER NOT NULL,gauge_profile INTEGER NOT NULL DEFAULT 0,"
       "gauge_auto_shift INTEGER NOT NULL,ln_mode INTEGER NOT NULL DEFAULT 0,"
       "requested_play_option TEXT,assist_option TEXT,final_score INTEGER NOT "
       "NULL,"
       "max_combo INTEGER NOT NULL DEFAULT 0,final_gauge REAL NOT NULL,"
       "clear_type INTEGER NOT NULL,completed_charts INTEGER NOT NULL,"
       "total_charts INTEGER NOT NULL,"
       "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP)");
  exec(database.get(),
       "CREATE TABLE course_replay_stages("
       "id INTEGER PRIMARY KEY AUTOINCREMENT,course_replay_id INTEGER NOT NULL,"
       "stage_index INTEGER NOT NULL,replay_id INTEGER NOT NULL,"
       "rest_micros_after_stage INTEGER NOT NULL DEFAULT 0,"
       "FOREIGN KEY(course_replay_id) REFERENCES course_replays(id) ON DELETE "
       "CASCADE,"
       "FOREIGN KEY(replay_id) REFERENCES replays(id) ON DELETE CASCADE)");

  exec(database.get(),
       "INSERT INTO replays(id,chart_path,chart_md5,chart_sha256,chart_title,"
       "chart_artist,ln_mode,gauge_type,gauge_auto_shift,final_score,max_combo,"
       "final_gauge,clear_type,created_at) VALUES"
       "(11,'BMS/kept.bms','aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',"
       "'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',"
       "'Kept','Artist',1,0,0,1234,456,72.5,300,'2026-07-01 02:03:04'),"
       "(12,'BMS/partial.bms','broken','broken','Partial','Artist',"
       "1,0,0,'malformed',44,20.0,100,'2026-07-02 02:03:04')");
  exec(database.get(),
       "INSERT INTO replay_events(replay_id,event_index,action,lane,"
       "note_time_micros,song_time_micros,judge_time_micros,judgement,"
       "diff_micros,gauge,gauge_type,combo,score)"
       " VALUES(11,0,0,1,1,2,3,0,0,99.0,0,9999,999999)");
  exec(database.get(),
       "INSERT INTO replay_touch_samples(replay_id,sample_index,action,"
       "finger_id,song_time_micros,x,y) VALUES(11,0,0,1,2,0.5,0.5)");
  exec(database.get(),
       "INSERT INTO replay_lane_cover_events(replay_id,event_index,"
       "song_time_micros,note_start_position_percent,"
       "reset_visible_time_reference) VALUES(11,0,2,25,1)");
  exec(database.get(),
       "INSERT INTO course_replays(id,course_id,course_name,course_group_name,"
       "constraint_json,gauge_type,gauge_profile,gauge_auto_shift,ln_mode,"
       "requested_play_option,assist_option,final_score,max_combo,final_gauge,"
       "clear_type,completed_charts,total_charts,created_at) VALUES"
       "(21,7,'Partial Course','Group','{}',0,0,0,1,'NORMAL','OFF',"
       "777,88,55.5,200,1,2,'2026-07-03 02:03:04')");
  exec(database.get(),
       "INSERT INTO course_replay_stages(course_replay_id,stage_index,"
       "replay_id,rest_micros_after_stage) VALUES(21,0,11,123456)");
  exec(database.get(), "PRAGMA user_version=2");
}

void createVersion13Fixture(const std::filesystem::path &path) {
  {
    ReplayRepository repository(path);
    assert(repository.EnsureSchema());
    repository.Shutdown();
  }

  auto database = openDatabase(path);
  exec(database.get(), "PRAGMA foreign_keys=OFF");
  exec(database.get(), "DROP TABLE ir_submission_receipts");
  exec(database.get(), "DROP TABLE legacy_course_result_summaries");
  exec(database.get(), "DROP TABLE legacy_chart_result_summaries");
  exec(database.get(),
       "CREATE TABLE replays("
       "id INTEGER PRIMARY KEY AUTOINCREMENT,chart_path TEXT,chart_md5 TEXT,"
       "chart_sha256 TEXT,chart_title TEXT,chart_artist TEXT,"
       "ln_mode INTEGER NOT NULL DEFAULT 0,gauge_type INTEGER NOT NULL,"
       "gauge_auto_shift INTEGER NOT NULL,final_score INTEGER NOT NULL,"
       "max_combo INTEGER NOT NULL DEFAULT 0,final_gauge REAL NOT NULL,"
       "clear_type INTEGER NOT NULL,random_seed INTEGER,random_prng TEXT,"
       "random_values TEXT,play_option TEXT,play_option_seed INTEGER,"
       "play_option2 TEXT,play_option2_seed INTEGER,assist_option TEXT,"
       "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
       "ruleset_version INTEGER NOT NULL DEFAULT 0,"
       "eligibility INTEGER NOT NULL DEFAULT 2,provenance_json TEXT NOT NULL,"
       "attempt_id TEXT,attempt_fingerprint TEXT)");
  exec(database.get(),
       "CREATE UNIQUE INDEX idx_replays_attempt_id ON replays(attempt_id) "
       "WHERE attempt_id IS NOT NULL");
  exec(database.get(),
       "CREATE TABLE replay_events(id INTEGER PRIMARY KEY,replay_id INTEGER)");
  exec(database.get(),
       "CREATE TABLE replay_touch_samples(id INTEGER PRIMARY KEY,replay_id "
       "INTEGER)");
  exec(database.get(),
       "CREATE TABLE replay_lane_cover_events(id INTEGER PRIMARY KEY,replay_id "
       "INTEGER)");
  exec(
      database.get(),
      "CREATE TABLE course_replays("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,course_id INTEGER NOT NULL,"
      "course_key TEXT NOT NULL DEFAULT '',course_name TEXT,"
      "course_group_name TEXT,constraint_json TEXT,gauge_type INTEGER NOT NULL,"
      "gauge_profile INTEGER NOT NULL DEFAULT 0,gauge_auto_shift INTEGER NOT "
      "NULL,ln_mode INTEGER NOT NULL DEFAULT 0,requested_play_option TEXT,"
      "assist_option TEXT,final_score INTEGER NOT NULL,max_combo INTEGER NOT "
      "NULL DEFAULT 0,final_gauge REAL NOT NULL,clear_type INTEGER NOT NULL,"
      "completed_charts INTEGER NOT NULL,total_charts INTEGER NOT NULL,"
      "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "ruleset_version INTEGER NOT NULL DEFAULT 0,"
      "eligibility INTEGER NOT NULL DEFAULT 2,provenance_json TEXT NOT NULL)");
  exec(database.get(),
       "CREATE TABLE course_replay_stages(id INTEGER PRIMARY KEY,"
       "course_replay_id INTEGER,replay_id INTEGER)");
  exec(database.get(),
       "CREATE TABLE pending_chart_score_writes("
       "attempt_id TEXT PRIMARY KEY NOT NULL,replay_id INTEGER NOT NULL UNIQUE,"
       "chart_path TEXT NOT NULL,chart_md5 TEXT NOT NULL,chart_sha256 TEXT NOT "
       "NULL,chart_title TEXT NOT NULL,chart_artist TEXT NOT NULL,"
       "ln_mode INTEGER NOT NULL,score INTEGER NOT NULL,max_score INTEGER NOT "
       "NULL,max_combo INTEGER NOT NULL,combo_break INTEGER NOT NULL,"
       "pgreat INTEGER NOT NULL,great INTEGER NOT NULL,good INTEGER NOT NULL,"
       "bad INTEGER NOT NULL,poor INTEGER NOT NULL,kpoor INTEGER NOT NULL,"
       "fast INTEGER NOT NULL,slow INTEGER NOT NULL,final_gauge REAL NOT NULL,"
       "clear_type INTEGER NOT NULL,ruleset_version INTEGER NOT NULL,"
       "eligibility INTEGER NOT NULL,provenance_json TEXT NOT NULL,"
       "created_at TEXT NOT NULL,recovery_attempts INTEGER NOT NULL DEFAULT 0,"
       "last_recovery_at TEXT,FOREIGN KEY(replay_id) REFERENCES replays(id) ON "
       "DELETE CASCADE)");
  exec(database.get(),
       "CREATE INDEX idx_pending_chart_score_created ON "
       "pending_chart_score_writes(recovery_attempts,last_recovery_at,"
       "created_at,attempt_id)");
  exec(database.get(),
       "CREATE TABLE ir_submission_receipts("
       "id INTEGER PRIMARY KEY AUTOINCREMENT,provider_id TEXT NOT NULL,"
       "server_origin TEXT NOT NULL,replay_id INTEGER,modern_chart_result_id "
       "INTEGER,attempt_id TEXT NOT NULL,chart_md5 TEXT,chart_sha256 TEXT NOT "
       "NULL,remote_user_id INTEGER,remote_chart_id TEXT,remote_score_id TEXT,"
       "confirmation_source INTEGER NOT NULL,observed_in_snapshot INTEGER NOT "
       "NULL DEFAULT 0,confirmed_at_ms INTEGER NOT NULL,"
       "UNIQUE(provider_id,server_origin,replay_id),"
       "UNIQUE(provider_id,server_origin,modern_chart_result_id),"
       "CHECK((replay_id IS NOT NULL)!=(modern_chart_result_id IS NOT NULL)),"
       "CHECK(observed_in_snapshot IN(0,1)),FOREIGN KEY(replay_id) REFERENCES "
       "replays(id) ON DELETE CASCADE,FOREIGN KEY(modern_chart_result_id) "
       "REFERENCES modern_chart_results(id) ON DELETE CASCADE)");
  exec(database.get(),
       "CREATE INDEX idx_ir_submission_receipts_attempt ON "
       "ir_submission_receipts(provider_id,server_origin,attempt_id)");
  exec(database.get(),
       "CREATE INDEX idx_ir_submission_receipts_remote_score ON "
       "ir_submission_receipts(provider_id,server_origin,remote_score_id)");

  constexpr std::string_view provenance =
      "{\"schemaVersion\":1,\"ruleset\":{\"version\":0},\"stages\":[],"
      "\"eligibility\":\"legacy-unverified\"}";
  exec(database.get(),
       "INSERT INTO replays(id,chart_path,chart_md5,chart_sha256,chart_title,"
       "chart_artist,ln_mode,gauge_type,gauge_auto_shift,final_score,max_combo,"
       "final_gauge,clear_type,created_at,ruleset_version,eligibility,"
       "provenance_json,attempt_id,attempt_fingerprint) VALUES"
       "(11,'BMS/inactive.bms','aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',"
       "'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',"
       "'Inactive','Artist',1,0,0,1111,111,71.0,300,'2026-07-04',0,2,'" +
           std::string(provenance) +
           "','legacy-inactive','fingerprint-inactive'),"
           "(12,'BMS/ready.bms','cccccccccccccccccccccccccccccccc',"
           "'dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd',"
           "'Ready','Artist',1,0,0,2222,222,72.0,300,'2026-07-05',0,2,'" +
           std::string(provenance) + "','legacy-ready','fingerprint-ready')");
  exec(database.get(),
       "INSERT INTO course_replays(id,course_id,course_key,course_name,"
       "course_group_name,constraint_json,gauge_type,gauge_profile,"
       "gauge_auto_shift,ln_mode,final_score,max_combo,final_gauge,clear_type,"
       "completed_charts,total_charts,created_at,ruleset_version,eligibility,"
       "provenance_json) VALUES(21,7,'course-key','Course','Group','{}',0,0,0,"
       "1,3333,333,73.0,300,2,2,'2026-07-06',0,2,'" +
           std::string(provenance) + "')");
  exec(database.get(),
       "INSERT INTO ir_outbox(provider_id,attempt_id,chart_md5,chart_sha256,"
       "payload_json,ruleset_id,ruleset_revision,validation_fingerprint,state,"
       "local_result_ready,next_attempt_at_ms,created_at_ms,updated_at_ms) "
       "VALUES('provider','legacy-inactive',NULL,"
       "'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',"
       "'{\"inactive\":true}','lr2',1,'validation-inactive',0,0,123,1000,"
       "1000),('provider','legacy-ready',NULL,"
       "'dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd',"
       "'{\"ready\":true}','lr2',1,'validation-ready',1,1,456,2000,2000)");
  exec(database.get(),
       "INSERT INTO ir_submission_receipts(id,provider_id,server_origin,"
       "replay_id,modern_chart_result_id,attempt_id,chart_md5,chart_sha256,"
       "remote_user_id,remote_chart_id,remote_score_id,confirmation_source,"
       "observed_in_snapshot,confirmed_at_ms) VALUES(77,'provider',"
       "'https://example.invalid',11,NULL,'legacy-inactive',NULL,"
       "'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',"
       "42,'chart-42','score-77',2,1,7777)");
  exec(database.get(), "PRAGMA user_version=13");
  exec(database.get(), "PRAGMA foreign_keys=ON");
}

void createVersion10ReceiptFixture(const std::filesystem::path &path) {
  createVersion13Fixture(path);
  auto database = openDatabase(path);
  exec(database.get(), "PRAGMA foreign_keys=OFF");
  exec(database.get(), "DROP TABLE ir_submission_receipts");
  exec(database.get(),
       "CREATE TABLE ir_submission_receipts("
       "id INTEGER PRIMARY KEY AUTOINCREMENT,provider_id TEXT NOT NULL,"
       "server_origin TEXT NOT NULL,replay_id INTEGER NOT NULL,"
       "attempt_id TEXT NOT NULL,chart_md5 TEXT,chart_sha256 TEXT NOT NULL,"
       "remote_user_id INTEGER,remote_chart_id TEXT,remote_score_id TEXT,"
       "confirmation_source INTEGER NOT NULL,observed_in_snapshot INTEGER "
       "NOT NULL DEFAULT 0,confirmed_at_ms INTEGER NOT NULL,"
       "UNIQUE(provider_id,server_origin,replay_id),"
       "CHECK(observed_in_snapshot IN(0,1)),FOREIGN KEY(replay_id) "
       "REFERENCES replays(id) ON DELETE CASCADE)");
  exec(database.get(),
       "CREATE INDEX idx_ir_submission_receipts_attempt ON "
       "ir_submission_receipts(provider_id,server_origin,attempt_id)");
  exec(database.get(),
       "CREATE INDEX idx_ir_submission_receipts_remote_score ON "
       "ir_submission_receipts(provider_id,server_origin,remote_score_id)");
  exec(database.get(),
       "INSERT INTO ir_submission_receipts(id,provider_id,server_origin,"
       "replay_id,attempt_id,chart_md5,chart_sha256,remote_user_id,"
       "remote_chart_id,remote_score_id,confirmation_source,"
       "observed_in_snapshot,confirmed_at_ms) VALUES(77,'provider',"
       "'https://example.invalid',11,'legacy-inactive',NULL,"
       "'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',"
       "42,'chart-42','score-77',0,1,7777)");
  exec(database.get(), "PRAGMA user_version=10");
  exec(database.get(), "PRAGMA foreign_keys=ON");
}

struct DetailReadGuard {
  int readAttempts = 0;
};

int denyDetailReads(void *raw, int action, const char *first, const char *,
                    const char *, const char *) {
  if (action != SQLITE_READ || first == nullptr) {
    return SQLITE_OK;
  }
  constexpr std::array detailTables{
      std::string_view("replay_events"),
      std::string_view("replay_touch_samples"),
      std::string_view("replay_lane_cover_events"),
      std::string_view("course_replay_stages"),
  };
  if (std::ranges::find(detailTables, std::string_view(first)) ==
      detailTables.end()) {
    return SQLITE_OK;
  }
  ++static_cast<DetailReadGuard *>(raw)->readAttempts;
  return SQLITE_DENY;
}

void testHeaderOnlyCutover() {
  TemporaryDirectory temporary;
  const auto path = temporary.path / "replay.db";
  createVersion2Fixture(path);
  auto database = openDatabase(path);
  DetailReadGuard guard;
  assert(sqlite3_set_authorizer(database.get(), denyDetailReads, &guard) ==
         SQLITE_OK);
  assert(replay_repository_test::RunSchemaMigration(database.get()));
  assert(guard.readAttempts == 0);
  assert(queryInt(database.get(), "PRAGMA user_version") == 15);

  assert(queryInt(database.get(),
                  "SELECT COUNT(*) FROM legacy_chart_result_summaries") == 2);
  assert(queryInt(database.get(),
                  "SELECT final_score FROM legacy_chart_result_summaries "
                  "WHERE legacy_replay_id=11") == 1234);
  assert(queryInt(database.get(),
                  "SELECT max_combo FROM legacy_chart_result_summaries "
                  "WHERE legacy_replay_id=11") == 456);
  assert(queryText(database.get(),
                   "SELECT typeof(final_score) FROM "
                   "legacy_chart_result_summaries WHERE legacy_replay_id=12") ==
         "null");
  assert(queryInt(database.get(),
                  "SELECT partial FROM legacy_chart_result_summaries "
                  "WHERE legacy_replay_id=12") == 1);
  assert(queryInt(database.get(), "SELECT completed_charts FROM "
                                  "legacy_course_result_summaries WHERE "
                                  "legacy_course_replay_id=21") == 1);
  assert(queryInt(database.get(),
                  "SELECT partial FROM legacy_course_result_summaries "
                  "WHERE legacy_course_replay_id=21") == 1);

  constexpr std::array rawTables{
      std::string_view("replays"),
      std::string_view("replay_events"),
      std::string_view("replay_touch_samples"),
      std::string_view("replay_lane_cover_events"),
      std::string_view("course_replays"),
      std::string_view("course_replay_stages"),
      std::string_view("pending_chart_score_writes"),
  };
  for (const auto table : rawTables) {
    assert(!tableExists(database.get(), table));
  }
}

void testSchema10LegacySummaryBoundaryIsHeaderOnly() {
  TemporaryDirectory temporary;
  const auto path = temporary.path / "replay.db";
  createVersion10ReceiptFixture(path);
  auto database = openDatabase(path);
  DetailReadGuard guard;
  assert(sqlite3_set_authorizer(database.get(), denyDetailReads, &guard) ==
         SQLITE_OK);
  assert(replay_repository_test::RunSchemaMigration(database.get()));
  assert(guard.readAttempts == 0);
  assert(queryInt(database.get(), "PRAGMA user_version") == 15);
  assert(queryInt(database.get(),
                  "SELECT final_score FROM legacy_chart_result_summaries "
                  "WHERE legacy_replay_id=11") == 1111);
  assert(queryInt(database.get(),
                  "SELECT final_score FROM legacy_course_result_summaries "
                  "WHERE legacy_course_replay_id=21") == 3333);
  for (const std::string_view table :
       {"replay_events", "replay_touch_samples", "replay_lane_cover_events",
        "course_replay_stages"}) {
    assert(!tableExists(database.get(), table));
  }
}

void testVersion10MigrationPreservesLegacyReceiptOwnership() {
  TemporaryDirectory temporary;
  const auto path = temporary.path / "replay.db";
  createVersion10ReceiptFixture(path);
  auto database = openDatabase(path);
  assert(replay_repository_test::RunSchemaMigration(database.get()));
  assert(queryInt(database.get(), "PRAGMA user_version") == 15);
  assert(queryInt(database.get(),
                  "SELECT replay_id FROM ir_submission_receipts WHERE id=77") ==
         11);
  assert(queryInt(database.get(),
                  "SELECT modern_chart_result_id IS NULL FROM "
                  "ir_submission_receipts WHERE id=77") == 1);
  assert(queryText(database.get(),
                   "SELECT \"table\" FROM pragma_foreign_key_list("
                   "'ir_submission_receipts') WHERE \"from\"='replay_id'") ==
         "legacy_chart_result_summaries");
}

void testFreshSchemaHasNoRawReplayTables() {
  TemporaryDirectory temporary;
  const auto path = temporary.path / "replay.db";
  ReplayRepository repository(path);
  assert(repository.EnsureSchema());
  repository.Shutdown();
  auto database = openDatabase(path);
  assert(queryInt(database.get(), "PRAGMA user_version") == 15);
  assert(tableExists(database.get(), "legacy_chart_result_summaries"));
  assert(tableExists(database.get(), "legacy_course_result_summaries"));
  assert(!tableExists(database.get(), "replays"));
  assert(!tableExists(database.get(), "replay_events"));
  assert(!tableExists(database.get(), "course_replays"));
}

void testDurableReceiptsAndOutboxWorkSurvive() {
  TemporaryDirectory temporary;
  const auto path = temporary.path / "replay.db";
  createVersion13Fixture(path);
  auto database = openDatabase(path);
  assert(replay_repository_test::RunSchemaMigration(database.get()));

  assert(queryInt(database.get(), "PRAGMA user_version") == 15);
  assert(queryInt(database.get(),
                  "SELECT COUNT(*) FROM legacy_chart_result_summaries") == 2);
  assert(queryInt(database.get(),
                  "SELECT COUNT(*) FROM legacy_course_result_summaries") == 1);
  assert(queryInt(database.get(), "SELECT id FROM ir_submission_receipts") ==
         77);
  assert(queryInt(database.get(),
                  "SELECT replay_id FROM ir_submission_receipts") == 11);
  assert(queryText(database.get(),
                   "SELECT remote_score_id FROM ir_submission_receipts") ==
         "score-77");
  assert(queryText(database.get(),
                   "SELECT \"table\" FROM pragma_foreign_key_list("
                   "'ir_submission_receipts') WHERE \"from\"='replay_id'") ==
         "legacy_chart_result_summaries");

  assert(queryInt(database.get(),
                  "SELECT local_result_ready FROM ir_outbox WHERE "
                  "attempt_id='legacy-ready'") == 1);
  assert(queryInt(database.get(), "SELECT state FROM ir_outbox WHERE "
                                  "attempt_id='legacy-ready'") == 1);
  assert(queryText(database.get(),
                   "SELECT payload_json FROM ir_outbox WHERE "
                   "attempt_id='legacy-ready'") == "{\"ready\":true}");
  assert(queryInt(database.get(),
                  "SELECT local_result_ready FROM ir_outbox WHERE "
                  "attempt_id='legacy-inactive'") == 0);
  assert(queryInt(database.get(), "SELECT state FROM ir_outbox WHERE "
                                  "attempt_id='legacy-inactive'") == 3);
  assert(queryText(database.get(),
                   "SELECT last_error_code FROM ir_outbox WHERE "
                   "attempt_id='legacy-inactive'") == "legacy_result_cutover");
}

void testMalformedProvenanceDoesNotBlockHeaderMigration() {
  TemporaryDirectory temporary;
  const auto path = temporary.path / "replay.db";
  createVersion13Fixture(path);
  auto database = openDatabase(path);
  constexpr std::string_view malformedProvenance =
      "{\"schemaVersion\":1,\"ruleset\":{\"version\":0},"
      "\"stages\":[{}],\"eligibility\":\"legacy-unverified\"}";
  exec(database.get(),
       "UPDATE replays SET provenance_json='" +
           std::string(malformedProvenance) +
           "' WHERE id=12; UPDATE course_replays SET provenance_json='" +
           std::string(malformedProvenance) + "' WHERE id=21");

  assert(replay_repository_test::RunSchemaMigration(database.get()));
  assert(queryInt(database.get(), "PRAGMA user_version") == 15);
  assert(queryText(database.get(),
                   "SELECT chart_title FROM legacy_chart_result_summaries "
                   "WHERE legacy_replay_id=12") == "Ready");
  assert(queryText(database.get(),
                   "SELECT typeof(provenance_json) FROM "
                   "legacy_chart_result_summaries WHERE legacy_replay_id=12") ==
         "null");
  assert(queryInt(database.get(),
                  "SELECT partial FROM legacy_chart_result_summaries WHERE "
                  "legacy_replay_id=12") == 1);
  assert(queryText(database.get(),
                   "SELECT course_name FROM legacy_course_result_summaries "
                   "WHERE legacy_course_replay_id=21") == "Course");
  assert(queryText(database.get(),
                   "SELECT typeof(provenance_json) FROM "
                   "legacy_course_result_summaries WHERE "
                   "legacy_course_replay_id=21") == "null");
  assert(queryInt(database.get(),
                  "SELECT partial FROM legacy_course_result_summaries WHERE "
                  "legacy_course_replay_id=21") == 1);
}

void testCurrentSchemaRejectsSummaryShapeDrift() {
  TemporaryDirectory temporary;
  const auto path = temporary.path / "replay.db";
  {
    ReplayRepository repository(path);
    assert(repository.EnsureSchema());
    repository.Shutdown();
  }
  {
    auto database = openDatabase(path);
    exec(database.get(),
         "ALTER TABLE legacy_chart_result_summaries ADD COLUMN invented "
         "INTEGER");
  }
  ReplayRepository repository(path);
  assert(!repository.EnsureSchema());
}

int denyPendingCourseScoreTable(void *, int action, const char *first,
                                const char *, const char *, const char *) {
  if (action == SQLITE_CREATE_TABLE && first != nullptr &&
      std::string_view(first) == "modern_pending_course_score_writes") {
    return SQLITE_DENY;
  }
  return SQLITE_OK;
}

struct DatabaseFileState {
  bool exists = false;
  std::string bytes;

  bool operator==(const DatabaseFileState &) const = default;
};

using DatabaseFamilySnapshot = std::array<DatabaseFileState, 4>;

DatabaseFamilySnapshot
snapshotDatabaseFamily(const std::filesystem::path &databasePath) {
  constexpr std::array suffixes{
      std::string_view(""), std::string_view("-journal"),
      std::string_view("-wal"), std::string_view("-shm")};
  DatabaseFamilySnapshot snapshot;
  for (std::size_t index = 0; index < suffixes.size(); ++index) {
    const auto member = std::filesystem::path(databasePath.string() +
                                              std::string(suffixes[index]));
    std::error_code error;
    snapshot[index].exists = std::filesystem::exists(member, error);
    assert(!error);
    if (snapshot[index].exists) {
      snapshot[index].bytes = readFile(member);
    }
  }
  return snapshot;
}

void testVersion14CourseScoreOutboxMigrationRollsBackAtomically() {
  TemporaryDirectory temporary;
  const auto path = temporary.path / "replay.db";
  {
    ReplayRepository repository(path);
    assert(repository.EnsureSchema());
    repository.Shutdown();
  }
  {
    auto database = openDatabase(path);
    exec(database.get(), "DROP TABLE modern_pending_course_score_writes");
    exec(database.get(), "PRAGMA user_version=14");
  }
  const auto before = snapshotDatabaseFamily(path);
  {
    auto database = openDatabase(path);
    assert(sqlite3_set_authorizer(database.get(), denyPendingCourseScoreTable,
                                  nullptr) == SQLITE_OK);
    assert(!replay_repository_test::RunSchemaMigration(database.get()));
    assert(sqlite3_set_authorizer(database.get(), nullptr, nullptr) ==
           SQLITE_OK);
  }
  assert(snapshotDatabaseFamily(path) == before);
  {
    auto database = openDatabase(path);
    assert(queryInt(database.get(), "PRAGMA user_version") == 14);
    assert(!tableExists(database.get(),
                        "modern_pending_course_score_writes"));
    assert(replay_repository_test::RunSchemaMigration(database.get()));
    assert(queryInt(database.get(), "PRAGMA user_version") == 15);
    assert(tableExists(database.get(),
                       "modern_pending_course_score_writes"));
  }
}

enum class MigrationPhase : std::uint8_t {
  None,
  Create,
  Copy,
  Receipt,
  Drop,
  Verify,
};

constexpr std::size_t phaseIndex(MigrationPhase phase) {
  assert(phase != MigrationPhase::None);
  return static_cast<std::size_t>(phase) - 1;
}

struct MigrationProbe {
  int callbacks = 0;
  int interruptAt = 0;
  bool fired = false;
  bool sawRawDrop = false;
  MigrationPhase current = MigrationPhase::None;
  MigrationPhase interrupted = MigrationPhase::None;
  std::array<int, 5> firstCallback{};
};

bool isRawDrop(std::string_view sql) {
  constexpr std::array rawTables{
      std::string_view("pending_chart_score_writes"),
      std::string_view("course_replay_stages"),
      std::string_view("replay_events"),
      std::string_view("replay_touch_samples"),
      std::string_view("replay_lane_cover_events"),
      std::string_view("course_replays"),
      std::string_view("replays"),
  };
  return sql.starts_with("DROP TABLE ") &&
         std::ranges::any_of(rawTables, [&](std::string_view table) {
           return sql.find(table) != std::string_view::npos;
         });
}

int traceMigrationSql(unsigned mask, void *raw, void *statement, void *) {
  if (mask != SQLITE_TRACE_STMT || statement == nullptr) {
    return 0;
  }
  auto &probe = *static_cast<MigrationProbe *>(raw);
  const char *rawSql = sqlite3_sql(static_cast<sqlite3_stmt *>(statement));
  const std::string_view sql = rawSql != nullptr ? rawSql : "";
  probe.current = MigrationPhase::None;
  if ((sql.starts_with("CREATE TABLE legacy_") ||
       sql.starts_with("CREATE INDEX idx_legacy_"))) {
    probe.current = MigrationPhase::Create;
  } else if (sql.starts_with("INSERT INTO legacy_") ||
             sql.starts_with("SELECT id,chart_path") ||
             sql.starts_with("SELECT id,course_id")) {
    probe.current = MigrationPhase::Copy;
  } else if (sql.find("ir_submission_receipts") != std::string_view::npos) {
    probe.current = MigrationPhase::Receipt;
  } else if (isRawDrop(sql)) {
    probe.current = MigrationPhase::Drop;
    probe.sawRawDrop = true;
  } else if (probe.sawRawDrop) {
    probe.current = MigrationPhase::Verify;
  }
  return 0;
}

int probeMigrationProgress(void *raw) {
  auto &probe = *static_cast<MigrationProbe *>(raw);
  ++probe.callbacks;
  if (probe.current != MigrationPhase::None) {
    int &first = probe.firstCallback[phaseIndex(probe.current)];
    if (first == 0) {
      first = probe.callbacks;
    }
  }
  if (!probe.fired && probe.interruptAt > 0 &&
      probe.callbacks >= probe.interruptAt) {
    probe.fired = true;
    probe.interrupted = probe.current;
    return 1;
  }
  return 0;
}

void installMigrationProbe(sqlite3 *database, MigrationProbe &probe) {
  assert(sqlite3_trace_v2(database, SQLITE_TRACE_STMT, traceMigrationSql,
                          &probe) == SQLITE_OK);
  sqlite3_progress_handler(database, 1, probeMigrationProgress, &probe);
}

void removeMigrationProbe(sqlite3 *database) {
  sqlite3_progress_handler(database, 0, nullptr, nullptr);
  assert(sqlite3_trace_v2(database, 0, nullptr, nullptr) == SQLITE_OK);
}

void testRollbackFaultMatrixPreservesOriginalDatabase() {
  TemporaryDirectory temporary;
  const auto pristinePath = temporary.path / "pristine.db";
  createVersion13Fixture(pristinePath);
  const DatabaseFamilySnapshot pristine = snapshotDatabaseFamily(pristinePath);

  const auto dryRunPath = temporary.path / "dry-run.db";
  assert(std::filesystem::copy_file(pristinePath, dryRunPath));
  MigrationProbe dryRun;
  {
    auto database = openDatabase(dryRunPath);
    installMigrationProbe(database.get(), dryRun);
    assert(replay_repository_test::RunSchemaMigration(database.get()));
    removeMigrationProbe(database.get());
  }
  assert(std::ranges::all_of(dryRun.firstCallback,
                             [](int callback) { return callback > 0; }));

  std::vector<int> interruptionThresholds(dryRun.firstCallback.begin(),
                                          dryRun.firstCallback.end());
  std::ranges::sort(interruptionThresholds);
  interruptionThresholds.erase(
      std::unique(interruptionThresholds.begin(), interruptionThresholds.end()),
      interruptionThresholds.end());
  std::array<bool, 5> phasesExercised{};
  for (std::size_t trial = 0; trial < interruptionThresholds.size(); ++trial) {
    const auto trialPath =
        temporary.path / ("fault-" + std::to_string(trial) + ".db");
    assert(std::filesystem::copy_file(pristinePath, trialPath));
    MigrationProbe probe{.interruptAt = interruptionThresholds[trial]};
    {
      auto database = openDatabase(trialPath);
      installMigrationProbe(database.get(), probe);
      assert(!replay_repository_test::RunSchemaMigration(database.get()));
      assert(probe.fired);
      removeMigrationProbe(database.get());
    }
    assert(probe.interrupted != MigrationPhase::None);
    phasesExercised[phaseIndex(probe.interrupted)] = true;
    assert(snapshotDatabaseFamily(trialPath) == pristine);

    auto database = openDatabase(trialPath);
    assert(queryInt(database.get(), "PRAGMA user_version") == 13);
    assert(queryInt(database.get(), "SELECT COUNT(*) FROM replays") == 2);
    assert(queryInt(database.get(),
                    "SELECT COUNT(*) FROM ir_submission_receipts") == 1);
    assert(!tableExists(database.get(), "legacy_chart_result_summaries"));
  }
  assert(std::ranges::all_of(phasesExercised,
                             [](bool exercised) { return exercised; }));

  const auto successPath = temporary.path / "success.db";
  assert(std::filesystem::copy_file(pristinePath, successPath));
  MigrationProbe success{.interruptAt = dryRun.callbacks + 1000};
  {
    auto database = openDatabase(successPath);
    installMigrationProbe(database.get(), success);
    assert(replay_repository_test::RunSchemaMigration(database.get()));
    assert(!success.fired);
    removeMigrationProbe(database.get());
  }
  auto database = openDatabase(successPath);
  assert(queryInt(database.get(), "PRAGMA user_version") == 15);
  assert(!tableExists(database.get(), "replays"));
}

} // namespace

int main() {
  static_assert(ReplayRepository::kCurrentSchemaVersion == 15);
  testHeaderOnlyCutover();
  testSchema10LegacySummaryBoundaryIsHeaderOnly();
  testVersion10MigrationPreservesLegacyReceiptOwnership();
  testFreshSchemaHasNoRawReplayTables();
  testDurableReceiptsAndOutboxWorkSurvive();
  testMalformedProvenanceDoesNotBlockHeaderMigration();
  testCurrentSchemaRejectsSummaryShapeDrift();
  testVersion14CourseScoreOutboxMigrationRollsBackAtomically();
  testRollbackFaultMatrixPreservesOriginalDatabase();
  std::cout << "legacy replay migration tests passed\n";
  return 0;
}
