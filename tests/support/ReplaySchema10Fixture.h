#pragma once

#include "sqlite3.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace replay_schema10_fixture {

inline void execute(sqlite3 *database, std::string_view sql) {
  char *message = nullptr;
  if (sqlite3_exec(database, std::string(sql).c_str(), nullptr, nullptr,
                   &message) != SQLITE_OK) {
    const std::string diagnostic = message != nullptr ? message : "SQL error";
    sqlite3_free(message);
    throw std::runtime_error(diagnostic);
  }
}

inline std::string quote(std::string_view value) {
  std::string result("'");
  for (const char character : value) {
    result.push_back(character);
    if (character == '\'') {
      result.push_back('\'');
    }
  }
  result.push_back('\'');
  return result;
}

inline void createExactSchema(sqlite3 *database) {
  execute(
      database,
      R"SQL(
CREATE TABLE replays(
 id INTEGER PRIMARY KEY AUTOINCREMENT,chart_path TEXT,chart_md5 TEXT,
 chart_sha256 TEXT,chart_title TEXT,chart_artist TEXT,
 ln_mode INTEGER NOT NULL DEFAULT 0,gauge_type INTEGER NOT NULL,
 gauge_auto_shift INTEGER NOT NULL,final_score INTEGER NOT NULL,
 max_combo INTEGER NOT NULL DEFAULT 0,final_gauge REAL NOT NULL,
 clear_type INTEGER NOT NULL,random_seed INTEGER,random_prng TEXT,
 random_values TEXT,play_option TEXT,play_option_seed INTEGER,
 play_option2 TEXT,play_option2_seed INTEGER,assist_option TEXT,
 created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
 ruleset_version INTEGER NOT NULL DEFAULT 0,
 eligibility INTEGER NOT NULL DEFAULT 2,
 provenance_json TEXT NOT NULL,attempt_id TEXT,attempt_fingerprint TEXT);
CREATE TABLE replay_events(
 id INTEGER PRIMARY KEY AUTOINCREMENT,replay_id INTEGER NOT NULL,
 event_index INTEGER NOT NULL,action INTEGER NOT NULL,lane INTEGER NOT NULL,
 note_time_micros INTEGER NOT NULL,song_time_micros INTEGER NOT NULL,
 judge_time_micros INTEGER NOT NULL,judgement INTEGER NOT NULL,
 diff_micros INTEGER NOT NULL,gauge REAL NOT NULL,gauge_type INTEGER NOT NULL,
 combo INTEGER NOT NULL,score INTEGER NOT NULL,
 FOREIGN KEY(replay_id) REFERENCES replays(id) ON DELETE CASCADE);
CREATE TABLE replay_touch_samples(
 id INTEGER PRIMARY KEY AUTOINCREMENT,replay_id INTEGER NOT NULL,
 sample_index INTEGER NOT NULL,action INTEGER NOT NULL,
 finger_id INTEGER NOT NULL,song_time_micros INTEGER NOT NULL,
 x REAL NOT NULL,y REAL NOT NULL,
 FOREIGN KEY(replay_id) REFERENCES replays(id) ON DELETE CASCADE);
CREATE TABLE replay_lane_cover_events(
 id INTEGER PRIMARY KEY AUTOINCREMENT,replay_id INTEGER NOT NULL,
 event_index INTEGER NOT NULL,song_time_micros INTEGER NOT NULL,
 note_start_position_percent INTEGER NOT NULL,
 reset_visible_time_reference INTEGER NOT NULL,
 FOREIGN KEY(replay_id) REFERENCES replays(id) ON DELETE CASCADE);
CREATE TABLE course_replays(
 id INTEGER PRIMARY KEY AUTOINCREMENT,course_id INTEGER NOT NULL,
 course_key TEXT NOT NULL DEFAULT '',course_name TEXT,course_group_name TEXT,
 constraint_json TEXT,gauge_type INTEGER NOT NULL,
 gauge_profile INTEGER NOT NULL DEFAULT 0,gauge_auto_shift INTEGER NOT NULL,
 ln_mode INTEGER NOT NULL DEFAULT 0,requested_play_option TEXT,
 assist_option TEXT,final_score INTEGER NOT NULL,
 max_combo INTEGER NOT NULL DEFAULT 0,final_gauge REAL NOT NULL,
 clear_type INTEGER NOT NULL,completed_charts INTEGER NOT NULL,
 total_charts INTEGER NOT NULL,
 created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
 ruleset_version INTEGER NOT NULL DEFAULT 0,
 eligibility INTEGER NOT NULL DEFAULT 2,provenance_json TEXT NOT NULL);
CREATE TABLE course_replay_stages(
 id INTEGER PRIMARY KEY AUTOINCREMENT,course_replay_id INTEGER NOT NULL,
 stage_index INTEGER NOT NULL,replay_id INTEGER NOT NULL,
 rest_micros_after_stage INTEGER NOT NULL DEFAULT 0,
 FOREIGN KEY(course_replay_id) REFERENCES course_replays(id) ON DELETE CASCADE,
 FOREIGN KEY(replay_id) REFERENCES replays(id) ON DELETE CASCADE);
CREATE UNIQUE INDEX idx_replays_attempt_id ON replays(attempt_id)
 WHERE attempt_id IS NOT NULL;
CREATE INDEX idx_replays_chart_sha256 ON replays(chart_sha256,id);
CREATE INDEX idx_replays_chart_md5 ON replays(chart_md5,id);
CREATE INDEX idx_replays_chart_path ON replays(chart_path,id);
CREATE INDEX idx_replay_events_replay_order
 ON replay_events(replay_id,event_index);
CREATE INDEX idx_replay_touch_samples_replay_order
 ON replay_touch_samples(replay_id,sample_index);
CREATE INDEX idx_replay_lane_cover_events_replay_order
 ON replay_lane_cover_events(replay_id,event_index);
CREATE INDEX idx_course_replays_course ON course_replays(course_id,id);
CREATE INDEX idx_course_replays_key_id ON course_replays(course_key,id);
CREATE INDEX idx_course_replay_stages_course_order
 ON course_replay_stages(course_replay_id,stage_index);
CREATE INDEX idx_course_replay_stages_replay
 ON course_replay_stages(replay_id);
CREATE TABLE pending_chart_score_writes(
 attempt_id TEXT PRIMARY KEY NOT NULL,replay_id INTEGER NOT NULL UNIQUE,
 chart_path TEXT NOT NULL,chart_md5 TEXT NOT NULL,
 chart_sha256 TEXT NOT NULL,chart_title TEXT NOT NULL,
 chart_artist TEXT NOT NULL,ln_mode INTEGER NOT NULL,score INTEGER NOT NULL,
 max_score INTEGER NOT NULL,max_combo INTEGER NOT NULL,
 combo_break INTEGER NOT NULL,pgreat INTEGER NOT NULL,great INTEGER NOT NULL,
 good INTEGER NOT NULL,bad INTEGER NOT NULL,poor INTEGER NOT NULL,
 kpoor INTEGER NOT NULL,fast INTEGER NOT NULL,slow INTEGER NOT NULL,
 final_gauge REAL NOT NULL,clear_type INTEGER NOT NULL,
 ruleset_version INTEGER NOT NULL,eligibility INTEGER NOT NULL,
 provenance_json TEXT NOT NULL,created_at TEXT NOT NULL,
 recovery_attempts INTEGER NOT NULL DEFAULT 0,last_recovery_at TEXT,
 FOREIGN KEY(replay_id) REFERENCES replays(id) ON DELETE CASCADE);
CREATE INDEX idx_pending_chart_score_created ON pending_chart_score_writes(
 recovery_attempts,last_recovery_at,created_at,attempt_id);
CREATE TABLE ir_outbox(
 id INTEGER PRIMARY KEY AUTOINCREMENT,provider_id TEXT NOT NULL,
 attempt_id TEXT NOT NULL,chart_md5 TEXT,chart_sha256 TEXT NOT NULL,
 payload_json TEXT NOT NULL,ruleset_id TEXT NOT NULL,
 ruleset_revision INTEGER NOT NULL,validation_fingerprint TEXT NOT NULL,
 state INTEGER NOT NULL,local_result_ready INTEGER NOT NULL DEFAULT 0,
 request_attempt_count INTEGER NOT NULL DEFAULT 0,
 consecutive_failure_count INTEGER NOT NULL DEFAULT 0,
 remote_poll_count INTEGER NOT NULL DEFAULT 0,next_attempt_at_ms INTEGER,
 next_request_user_intent INTEGER NOT NULL DEFAULT 0,remote_job_id TEXT,
 remote_origin TEXT,last_error_code TEXT,last_error_message TEXT,
 created_at_ms INTEGER NOT NULL,updated_at_ms INTEGER NOT NULL,
 completed_at_ms INTEGER,UNIQUE(provider_id,attempt_id),
 CHECK(local_result_ready IN(0,1)),CHECK(next_request_user_intent IN(0,1)),
 CHECK((remote_job_id IS NULL AND remote_origin IS NULL) OR
       (remote_job_id IS NOT NULL AND remote_origin IS NOT NULL)));
CREATE INDEX idx_ir_outbox_due ON
 ir_outbox(local_result_ready,state,next_attempt_at_ms,id);
CREATE INDEX idx_ir_outbox_attempt ON ir_outbox(provider_id,attempt_id);
CREATE TABLE ir_submission_receipts(
 id INTEGER PRIMARY KEY AUTOINCREMENT,provider_id TEXT NOT NULL,
 server_origin TEXT NOT NULL,replay_id INTEGER NOT NULL,
 attempt_id TEXT NOT NULL,chart_md5 TEXT,chart_sha256 TEXT NOT NULL,
 remote_user_id INTEGER,remote_chart_id TEXT,remote_score_id TEXT,
 confirmation_source INTEGER NOT NULL,
 observed_in_snapshot INTEGER NOT NULL DEFAULT 0,
 confirmed_at_ms INTEGER NOT NULL,
 UNIQUE(provider_id,server_origin,replay_id),
 CHECK(observed_in_snapshot IN(0,1)),
 FOREIGN KEY(replay_id) REFERENCES replays(id) ON DELETE CASCADE);
CREATE INDEX idx_ir_submission_receipts_attempt ON
 ir_submission_receipts(provider_id,server_origin,attempt_id);
CREATE INDEX idx_ir_submission_receipts_remote_score ON
 ir_submission_receipts(provider_id,server_origin,remote_score_id);
CREATE TABLE ir_remote_scores(
 provider_id TEXT NOT NULL,server_origin TEXT NOT NULL,
 remote_score_id TEXT NOT NULL,remote_user_id INTEGER NOT NULL,
 game TEXT NOT NULL,remote_chart_id TEXT NOT NULL,chart_md5 TEXT NOT NULL,
 chart_sha256 TEXT NOT NULL,title TEXT NOT NULL,artist TEXT NOT NULL,
 difficulty TEXT,level TEXT,level_number REAL,note_count INTEGER NOT NULL,
 score INTEGER NOT NULL,lamp_rank INTEGER NOT NULL,service TEXT NOT NULL,
 time_achieved_ms INTEGER,time_added_ms INTEGER NOT NULL,
 pgreat INTEGER,great INTEGER,good INTEGER,bad INTEGER,poor INTEGER,
 early_pgreat INTEGER,late_pgreat INTEGER,early_great INTEGER,
 late_great INTEGER,early_good INTEGER,late_good INTEGER,early_bad INTEGER,
 late_bad INTEGER,early_poor INTEGER,late_poor INTEGER,fast INTEGER,
 slow INTEGER,max_combo INTEGER,bad_points INTEGER,final_gauge REAL,
 gauge_history_json TEXT,random_mode TEXT,gauge_mode TEXT,input_device TEXT,
 client TEXT,sync_generation INTEGER NOT NULL,
 PRIMARY KEY(provider_id,server_origin,remote_score_id),
 CHECK(game IN('bms-7k','bms-14k')),CHECK(remote_user_id>0),
 CHECK(length(chart_md5)=32 AND chart_md5=lower(chart_md5) AND
       chart_md5 NOT GLOB '*[^0-9a-f]*'),
 CHECK(length(chart_sha256)=64 AND chart_sha256=lower(chart_sha256) AND
       chart_sha256 NOT GLOB '*[^0-9a-f]*'),
 CHECK(note_count>=0 AND score>=0 AND score<=note_count*2),
 CHECK(lamp_rank IN(0,100,150,200,300,400,500,600)),
 CHECK(time_achieved_ms IS NULL OR time_achieved_ms>0),
 CHECK(time_added_ms>0),CHECK(pgreat IS NULL OR pgreat>=0),
 CHECK(great IS NULL OR great>=0),CHECK(good IS NULL OR good>=0),
 CHECK(bad IS NULL OR bad>=0),CHECK(poor IS NULL OR poor>=0),
 CHECK(early_pgreat IS NULL OR early_pgreat>=0),
 CHECK(late_pgreat IS NULL OR late_pgreat>=0),
 CHECK(early_great IS NULL OR early_great>=0),
 CHECK(late_great IS NULL OR late_great>=0),
 CHECK(early_good IS NULL OR early_good>=0),
 CHECK(late_good IS NULL OR late_good>=0),
 CHECK(early_bad IS NULL OR early_bad>=0),
 CHECK(late_bad IS NULL OR late_bad>=0),
 CHECK(early_poor IS NULL OR early_poor>=0),
 CHECK(late_poor IS NULL OR late_poor>=0),
 CHECK(fast IS NULL OR fast>=0),CHECK(slow IS NULL OR slow>=0),
 CHECK(max_combo IS NULL OR max_combo>=0),
 CHECK(bad_points IS NULL OR bad_points>=0),
 CHECK(final_gauge IS NULL OR(final_gauge>=0 AND final_gauge<=100)),
 CHECK(sync_generation>0));
CREATE INDEX idx_ir_remote_scores_chart_sha256 ON
 ir_remote_scores(provider_id,server_origin,chart_sha256);
CREATE INDEX idx_ir_remote_scores_remote_chart_id ON
 ir_remote_scores(provider_id,server_origin,remote_chart_id);
PRAGMA user_version=10;
)SQL");
}

inline void insertSimpleChart(sqlite3 *database, std::int64_t id,
                              std::string_view md5, std::string_view sha256,
                              std::string_view createdAt, int score,
                              std::string_view provenance,
                              std::string_view attemptId = {}) {
  const std::string attempt = attemptId.empty() ? "NULL" : quote(attemptId);
  execute(
      database,
      "INSERT INTO replays(id,chart_path,chart_md5,chart_sha256,chart_title,"
      "chart_artist,ln_mode,gauge_type,gauge_auto_shift,final_score,max_combo,"
      "final_gauge,clear_type,play_option,play_option2,assist_option,created_at,"
      "ruleset_version,eligibility,provenance_json,attempt_id) VALUES(" +
          std::to_string(id) + ",'chart.bms'," + quote(md5) + "," +
          quote(sha256) + ",'Chart','Artist',0,0,0," +
          std::to_string(score) +
          ",1,75.0,300,'NORMAL','NORMAL','OFF'," + quote(createdAt) +
          ",0,2," + quote(provenance) + "," + attempt + ")");
  execute(
      database,
      "INSERT INTO replay_events(replay_id,event_index,action,lane,"
      "note_time_micros,song_time_micros,judge_time_micros,judgement,"
      "diff_micros,gauge,gauge_type,combo,score) VALUES(" +
          std::to_string(id) + ",0,0,1,1000,1000,1000,0,0,75.0,0,1," +
          std::to_string(score) + "),(" + std::to_string(id) +
          ",1,1,1,1000,1100,1100,6,100,75.0,0,1," +
          std::to_string(score) + ")");
}

} // namespace replay_schema10_fixture
