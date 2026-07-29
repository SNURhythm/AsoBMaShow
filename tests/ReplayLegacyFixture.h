#pragma once

#include "../src/sqlite3.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>

namespace replay_legacy_fixture {

inline bool execute(sqlite3 *database, std::string_view sql,
                    std::string &error) {
  char *rawError = nullptr;
  const std::string statement(sql);
  const int result =
      sqlite3_exec(database, statement.c_str(), nullptr, nullptr, &rawError);
  if (result == SQLITE_OK) {
    return true;
  }
  error = rawError == nullptr ? "unknown SQLite failure" : rawError;
  sqlite3_free(rawError);
  return false;
}

inline bool createVersion2Schema(sqlite3 *database, int userVersion,
                                 std::string &error) {
  if (database == nullptr || userVersion < 0 || userVersion >= 14) {
    error = "legacy replay fixture version is invalid";
    return false;
  }
  const std::string schema =
      "PRAGMA foreign_keys=OFF;"
      "CREATE TABLE replays("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,chart_path TEXT,chart_md5 TEXT,"
      "chart_sha256 TEXT,chart_title TEXT,chart_artist TEXT,"
      "ln_mode INTEGER NOT NULL DEFAULT 0,gauge_type INTEGER NOT NULL,"
      "gauge_auto_shift INTEGER NOT NULL,final_score INTEGER NOT NULL,"
      "max_combo INTEGER NOT NULL DEFAULT 0,final_gauge REAL NOT NULL,"
      "clear_type INTEGER NOT NULL,random_seed INTEGER,random_prng TEXT,"
      "random_values TEXT,play_option TEXT,play_option_seed INTEGER,"
      "play_option2 TEXT,play_option2_seed INTEGER,assist_option TEXT,"
      "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);"
      "CREATE TABLE replay_events("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,replay_id INTEGER NOT NULL,"
      "event_index INTEGER NOT NULL,action INTEGER NOT NULL,lane INTEGER NOT NULL,"
      "note_time_micros INTEGER NOT NULL,song_time_micros INTEGER NOT NULL,"
      "judge_time_micros INTEGER NOT NULL,judgement INTEGER NOT NULL,"
      "diff_micros INTEGER NOT NULL,gauge REAL NOT NULL,gauge_type INTEGER NOT NULL,"
      "combo INTEGER NOT NULL,score INTEGER NOT NULL,"
      "FOREIGN KEY(replay_id) REFERENCES replays(id) ON DELETE CASCADE);"
      "CREATE TABLE replay_touch_samples("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,replay_id INTEGER NOT NULL,"
      "sample_index INTEGER NOT NULL,action INTEGER NOT NULL,"
      "finger_id INTEGER NOT NULL,song_time_micros INTEGER NOT NULL,"
      "x REAL NOT NULL,y REAL NOT NULL,"
      "FOREIGN KEY(replay_id) REFERENCES replays(id) ON DELETE CASCADE);"
      "CREATE TABLE replay_lane_cover_events("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,replay_id INTEGER NOT NULL,"
      "event_index INTEGER NOT NULL,song_time_micros INTEGER NOT NULL,"
      "note_start_position_percent INTEGER NOT NULL,"
      "reset_visible_time_reference INTEGER NOT NULL,"
      "FOREIGN KEY(replay_id) REFERENCES replays(id) ON DELETE CASCADE);"
      "CREATE TABLE course_replays("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,course_id INTEGER NOT NULL,"
      "course_name TEXT,course_group_name TEXT,constraint_json TEXT,"
      "gauge_type INTEGER NOT NULL,gauge_profile INTEGER NOT NULL DEFAULT 0,"
      "gauge_auto_shift INTEGER NOT NULL,ln_mode INTEGER NOT NULL DEFAULT 0,"
      "requested_play_option TEXT,assist_option TEXT,final_score INTEGER NOT NULL,"
      "max_combo INTEGER NOT NULL DEFAULT 0,final_gauge REAL NOT NULL,"
      "clear_type INTEGER NOT NULL,completed_charts INTEGER NOT NULL,"
      "total_charts INTEGER NOT NULL,"
      "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);"
      "CREATE TABLE course_replay_stages("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,course_replay_id INTEGER NOT NULL,"
      "stage_index INTEGER NOT NULL,replay_id INTEGER NOT NULL,"
      "rest_micros_after_stage INTEGER NOT NULL DEFAULT 0,"
      "FOREIGN KEY(course_replay_id) REFERENCES course_replays(id) ON DELETE CASCADE,"
      "FOREIGN KEY(replay_id) REFERENCES replays(id) ON DELETE CASCADE);"
      "PRAGMA user_version=" +
      std::to_string(userVersion);
  return execute(database, schema, error);
}

inline bool replaceWithVersion2Database(const std::filesystem::path &path,
                                        int userVersion,
                                        std::string_view seedSql,
                                        std::string &error) {
  std::error_code filesystemError;
  std::filesystem::remove(path, filesystemError);
  if (filesystemError) {
    error = "could not replace replay fixture: " + filesystemError.message();
    return false;
  }
  for (const std::string suffix : {std::string("-wal"), std::string("-shm")}) {
    filesystemError.clear();
    std::filesystem::remove(path.string() + suffix, filesystemError);
    if (filesystemError) {
      error = "could not clear replay fixture family: " +
              filesystemError.message();
      return false;
    }
  }
  std::filesystem::create_directories(path.parent_path(), filesystemError);
  if (filesystemError) {
    error = "could not create replay fixture directory: " +
            filesystemError.message();
    return false;
  }
  sqlite3 *database = nullptr;
  if (sqlite3_open(path.string().c_str(), &database) != SQLITE_OK) {
    error = database == nullptr ? "could not open replay fixture"
                                : sqlite3_errmsg(database);
    if (database != nullptr) {
      sqlite3_close(database);
    }
    return false;
  }
  const bool created = createVersion2Schema(database, userVersion, error) &&
                       (seedSql.empty() || execute(database, seedSql, error));
  if (sqlite3_close(database) != SQLITE_OK && created) {
    error = "could not close replay fixture";
    return false;
  }
  return created;
}

} // namespace replay_legacy_fixture
