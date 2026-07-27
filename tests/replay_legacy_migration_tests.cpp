#include "repositories/ReplayRepository.h"
#include "repositories/ReplayRepositoryInternal.h"
#include "repositories/SqliteRAII.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

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
       "event_index INTEGER NOT NULL,action INTEGER NOT NULL,lane INTEGER NOT NULL,"
       "note_time_micros INTEGER NOT NULL,song_time_micros INTEGER NOT NULL,"
       "judge_time_micros INTEGER NOT NULL,judgement INTEGER NOT NULL,"
       "diff_micros INTEGER NOT NULL,gauge REAL NOT NULL,gauge_type INTEGER NOT NULL,"
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
       "requested_play_option TEXT,assist_option TEXT,final_score INTEGER NOT NULL,"
       "max_combo INTEGER NOT NULL DEFAULT 0,final_gauge REAL NOT NULL,"
       "clear_type INTEGER NOT NULL,completed_charts INTEGER NOT NULL,"
       "total_charts INTEGER NOT NULL,"
       "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP)");
  exec(database.get(),
       "CREATE TABLE course_replay_stages("
       "id INTEGER PRIMARY KEY AUTOINCREMENT,course_replay_id INTEGER NOT NULL,"
       "stage_index INTEGER NOT NULL,replay_id INTEGER NOT NULL,"
       "rest_micros_after_stage INTEGER NOT NULL DEFAULT 0,"
       "FOREIGN KEY(course_replay_id) REFERENCES course_replays(id) ON DELETE CASCADE,"
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
  assert(replay_repository_detail::CreateReplayTablesOnConnection(
      database.get()));
  assert(guard.readAttempts == 0);
  assert(queryInt(database.get(), "PRAGMA user_version") == 14);

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
  assert(queryInt(database.get(),
                  "SELECT completed_charts FROM "
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

void testFreshSchemaHasNoRawReplayTables() {
  TemporaryDirectory temporary;
  const auto path = temporary.path / "replay.db";
  ReplayRepository repository(path);
  assert(repository.EnsureSchema());
  repository.Shutdown();
  auto database = openDatabase(path);
  assert(queryInt(database.get(), "PRAGMA user_version") == 14);
  assert(tableExists(database.get(), "legacy_chart_result_summaries"));
  assert(tableExists(database.get(), "legacy_course_result_summaries"));
  assert(!tableExists(database.get(), "replays"));
  assert(!tableExists(database.get(), "replay_events"));
  assert(!tableExists(database.get(), "course_replays"));
}

struct OneShotInterrupt {
  int remaining = 1;
  bool fired = false;
};

int interruptOnce(void *raw) {
  auto &state = *static_cast<OneShotInterrupt *>(raw);
  if (!state.fired && --state.remaining <= 0) {
    state.fired = true;
    return 1;
  }
  return 0;
}

void testInterruptedMigrationPreservesOriginalDatabase() {
  TemporaryDirectory temporary;
  const auto path = temporary.path / "replay.db";
  createVersion2Fixture(path);
  const std::string before = readFile(path);
  {
    auto database = openDatabase(path);
    OneShotInterrupt interrupt{.remaining = 20};
    sqlite3_progress_handler(database.get(), 1, interruptOnce, &interrupt);
    assert(!replay_repository_detail::CreateReplayTablesOnConnection(
        database.get()));
    assert(interrupt.fired);
    sqlite3_progress_handler(database.get(), 0, nullptr, nullptr);
  }
  assert(readFile(path) == before);
  auto database = openDatabase(path);
  assert(queryInt(database.get(), "PRAGMA user_version") == 2);
  assert(queryInt(database.get(), "SELECT COUNT(*) FROM replays") == 2);
  assert(queryInt(database.get(), "SELECT COUNT(*) FROM replay_events") == 1);
  assert(!tableExists(database.get(), "legacy_chart_result_summaries"));
}

} // namespace

int main() {
  static_assert(ReplayRepository::kCurrentSchemaVersion == 14);
  testHeaderOnlyCutover();
  testFreshSchemaHasNoRawReplayTables();
  testInterruptedMigrationPreservesOriginalDatabase();
  std::cout << "legacy replay migration tests passed\n";
  return 0;
}
