#include "ScoreProvenance.h"
#include "CourseIdentity.h"
#include "replay/BeatorajaReplayCodec.h"
#include "replay/ReplayFileStore.h"
#include "repositories/ReplayRepositoryReplayFileMigration.h"
#include "repositories/ReplayRepository.h"
#include "sqlite3.h"
#include "support/ReplaySchema10Fixture.h"

#include <concepts>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

static_assert(std::default_initializable<
              replay_repository_detail::ReplayMigrationFaults>);
static_assert(std::same_as<
              decltype(replay_repository_detail::ReplayMigrationFaults{}
                           .failAt),
              std::function<bool(std::string_view, std::int64_t)>>);

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    std::mt19937_64 random(std::random_device{}());
    for (int attempt = 0; attempt < 100; ++attempt) {
      path_ = std::filesystem::temp_directory_path() /
              ("asobmashow-v10-replay-migration-" +
               std::to_string(random()));
      std::error_code error;
      if (std::filesystem::create_directory(path_, error)) {
        return;
      }
    }
    throw std::runtime_error("could not create migration test directory");
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  [[nodiscard]] const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

class Database {
public:
  explicit Database(const std::filesystem::path &path) {
    if (sqlite3_open_v2(path.string().c_str(), &database_,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) !=
        SQLITE_OK) {
      throw std::runtime_error("could not open migration test database");
    }
    sqlite3_extended_result_codes(database_, 1);
    sqlite3_exec(database_, "PRAGMA foreign_keys=ON", nullptr, nullptr,
                 nullptr);
  }

  ~Database() { sqlite3_close(database_); }
  Database(const Database &) = delete;
  Database &operator=(const Database &) = delete;

  [[nodiscard]] sqlite3 *get() const { return database_; }

private:
  sqlite3 *database_ = nullptr;
};

void executeOrThrow(sqlite3 *database, std::string_view sql) {
  char *message = nullptr;
  if (sqlite3_exec(database, std::string(sql).c_str(), nullptr, nullptr,
                   &message) != SQLITE_OK) {
    const std::string diagnostic = message != nullptr ? message : "SQL error";
    sqlite3_free(message);
    throw std::runtime_error(diagnostic);
  }
}

std::string sqlQuote(std::string_view value) {
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

std::int64_t integer(sqlite3 *database, std::string_view query) {
  sqlite3_stmt *statement = nullptr;
  if (sqlite3_prepare_v2(database, std::string(query).c_str(), -1, &statement,
                         nullptr) != SQLITE_OK ||
      sqlite3_step(statement) != SQLITE_ROW) {
    sqlite3_finalize(statement);
    return -1;
  }
  const auto value = sqlite3_column_int64(statement, 0);
  const bool complete = sqlite3_step(statement) == SQLITE_DONE;
  sqlite3_finalize(statement);
  return complete ? value : -1;
}

std::string text(sqlite3 *database, std::string_view query) {
  sqlite3_stmt *statement = nullptr;
  if (sqlite3_prepare_v2(database, std::string(query).c_str(), -1, &statement,
                         nullptr) != SQLITE_OK ||
      sqlite3_step(statement) != SQLITE_ROW) {
    sqlite3_finalize(statement);
    return {};
  }
  const auto *value = sqlite3_column_text(statement, 0);
  std::string result = value == nullptr
                           ? std::string{}
                           : reinterpret_cast<const char *>(value);
  if (sqlite3_step(statement) != SQLITE_DONE) {
    result.clear();
  }
  sqlite3_finalize(statement);
  return result;
}

bool tableExists(sqlite3 *database, std::string_view table) {
  return integer(database,
                 "SELECT count(*) FROM sqlite_master WHERE type='table' AND "
                 "name=" +
                     sqlQuote(table)) == 1;
}

struct LegacySnapshot {
  std::int64_t userVersion = 0;
  std::string schema;
  std::vector<std::int64_t> counts;
  std::string replayFacts;
  std::string pendingFacts;
  std::string durableIrFacts;

  bool operator==(const LegacySnapshot &) const = default;
};

LegacySnapshot snapshotLegacyDatabase(sqlite3 *database) {
  LegacySnapshot snapshot;
  snapshot.userVersion = integer(database, "PRAGMA user_version");
  snapshot.schema = text(
      database,
      "SELECT group_concat(value,char(10)) FROM (SELECT type||':'||name||':'||"
      "COALESCE(sql,'') AS value FROM sqlite_master ORDER BY type,name)");
  for (std::string_view table :
       {"replays", "replay_events", "replay_touch_samples",
        "replay_lane_cover_events", "course_replays", "course_replay_stages",
        "pending_chart_score_writes", "ir_outbox",
        "ir_submission_receipts", "ir_remote_scores"}) {
    snapshot.counts.push_back(
        integer(database, "SELECT count(*) FROM " + std::string(table)));
  }
  snapshot.replayFacts = text(
      database,
      "SELECT group_concat(value,';') FROM (SELECT id||'|'||chart_sha256||'|'"
      "||COALESCE(attempt_id,'')||'|'||final_score||'|'||created_at AS value "
      "FROM replays ORDER BY id)");
  snapshot.pendingFacts = text(
      database,
      "SELECT group_concat(value,';') FROM (SELECT attempt_id||'|'||replay_id"
      "||'|'||score||'|'||recovery_attempts AS value FROM "
      "pending_chart_score_writes ORDER BY attempt_id)");
  snapshot.durableIrFacts = text(
      database,
      "SELECT (SELECT COALESCE(group_concat(id||'|'||payload_json,';'),'') "
      "FROM ir_outbox)||(SELECT COALESCE(group_concat(id||'|'||replay_id,';'),"
      "'') FROM ir_submission_receipts)");
  return snapshot;
}

void createExactSchema10(sqlite3 *database) {
  executeOrThrow(
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

struct FixtureFacts {
  std::string attemptId = "123e4567-e89b-42d3-a456-426614174001";
  std::string md5 = std::string(32, 'b');
  std::string sha256 = std::string(64, 'a');
  std::string provenance =
      serializeScoreProvenance(ScoreProvenance::Legacy());
  std::int64_t playedAtUnixMillis = 0;
};

FixtureFacts insertChartFixture(sqlite3 *database) {
  FixtureFacts facts;
  const std::string provenance = sqlQuote(facts.provenance);
  executeOrThrow(
      database,
      "INSERT INTO replays(id,chart_path,chart_md5,chart_sha256,chart_title,"
      "chart_artist,ln_mode,gauge_type,gauge_auto_shift,final_score,max_combo,"
      "final_gauge,clear_type,random_seed,random_prng,random_values,"
      "play_option,play_option_seed,play_option2,play_option2_seed,"
      "assist_option,created_at,ruleset_version,eligibility,provenance_json,"
      "attempt_id,attempt_fingerprint) VALUES(42,'BMS/chart.bms','" +
          facts.md5 + "','" + facts.sha256 +
          "','Migration Chart','Artist',1,2,0,3,2,64.5,300,77,"
          "'xorshift128','3,4','MIRROR',91,'NORMAL',NULL,'OFF',"
          "'2026-07-25 01:02:03',0,2," + provenance + ",'" +
          facts.attemptId + "','" + std::string(64, 'f') + "')");
  executeOrThrow(
      database,
      "INSERT INTO replay_events(replay_id,event_index,action,lane,"
      "note_time_micros,song_time_micros,judge_time_micros,judgement,"
      "diff_micros,gauge,gauge_type,combo,score) VALUES"
      "(42,0,0,0,1000,1100,1100,0,100,55.0,2,1,2),"
      "(42,1,1,0,1000,1300,1300,1,300,64.5,2,2,3)");
  executeOrThrow(
      database,
      "INSERT INTO replay_touch_samples(replay_id,sample_index,action,"
      "finger_id,song_time_micros,x,y) VALUES(42,0,0,9,900,0.25,0.75)");
  executeOrThrow(
      database,
      "INSERT INTO replay_lane_cover_events(replay_id,event_index,"
      "song_time_micros,note_start_position_percent,"
      "reset_visible_time_reference) VALUES"
      "(42,0,0,30,0),(42,1,5000,45,1)");
  executeOrThrow(
      database,
      "INSERT INTO pending_chart_score_writes(attempt_id,replay_id,"
      "chart_path,chart_md5,chart_sha256,chart_title,chart_artist,ln_mode,"
      "score,max_score,max_combo,combo_break,pgreat,great,good,bad,poor,"
      "kpoor,fast,slow,final_gauge,clear_type,ruleset_version,eligibility,"
      "provenance_json,created_at,recovery_attempts,last_recovery_at) VALUES('" +
          facts.attemptId + "',42,'BMS/chart.bms','" + facts.md5 + "','" +
          facts.sha256 +
          "','Migration Chart','Artist',1,3,4,2,0,1,1,0,0,0,0,2,0,64.5,"
          "300,0,2," + provenance +
          ",'2026-07-25 01:02:03',2,'2026-07-25 01:05:00')");
  executeOrThrow(
      database,
      "INSERT INTO ir_outbox(id,provider_id,attempt_id,chart_md5,"
      "chart_sha256,payload_json,ruleset_id,ruleset_revision,"
      "validation_fingerprint,state,local_result_ready,request_attempt_count,"
      "consecutive_failure_count,remote_poll_count,next_attempt_at_ms,"
      "next_request_user_intent,created_at_ms,updated_at_ms) VALUES"
      "(7,'tachi','" +
          facts.attemptId + "','" + facts.md5 + "','" + facts.sha256 +
          "','{\"score\":3}','legacy',0,'proof',0,1,2,1,4,12345,1,100,200)");
  executeOrThrow(
      database,
      "INSERT INTO ir_submission_receipts(id,provider_id,server_origin,"
      "replay_id,attempt_id,chart_md5,chart_sha256,remote_user_id,"
      "remote_chart_id,remote_score_id,confirmation_source,"
      "observed_in_snapshot,confirmed_at_ms) VALUES(8,'tachi',"
      "'https://example.test',42,'" +
          facts.attemptId + "','" + facts.md5 + "','" + facts.sha256 +
          "',99,'chart-1','score-1',1,1,300)");
  executeOrThrow(
      database,
      "INSERT INTO ir_remote_scores(provider_id,server_origin,remote_score_id,"
      "remote_user_id,game,remote_chart_id,chart_md5,chart_sha256,title,artist,"
      "note_count,score,lamp_rank,service,time_added_ms,sync_generation) VALUES"
      "('tachi','https://example.test','mirror-1',99,'bms-7k','chart-1','" +
          facts.md5 + "','" + facts.sha256 +
          "','Migration Chart','Artist',2,3,300,'Bokutachi',300,1)");
  facts.playedAtUnixMillis = integer(
      database,
      "SELECT CAST(strftime('%s',created_at) AS INTEGER)*1000 FROM replays "
      "WHERE id=42");
  return facts;
}

std::string insertCourseFixture(sqlite3 *database,
                                const FixtureFacts &chart) {
  const std::array identities{course_identity::ChartIdentity{
      .sha256 = chart.sha256, .md5 = chart.md5}};
  constexpr std::string_view constraints =
      R"(["grade_mirror","no_speed","gauge_7k","cn"])";
  const std::string courseKey =
      course_identity::makeCourseKey(identities, constraints);
  executeOrThrow(
      database,
      "INSERT INTO course_replays(id,course_id,course_key,course_name,"
      "course_group_name,constraint_json,gauge_type,gauge_profile,"
      "gauge_auto_shift,ln_mode,requested_play_option,assist_option,"
      "final_score,max_combo,final_gauge,clear_type,completed_charts,"
      "total_charts,created_at,ruleset_version,eligibility,provenance_json) "
      "VALUES(77,12," +
          sqlQuote(courseKey) +
          ",'Migration Course','Folder'," + sqlQuote(constraints) +
          ",2,1,0,1,'MIRROR','OFF',3,2,"
          "64.5,300,1,1,'2026-07-25 01:03:04',0,2," +
          sqlQuote(chart.provenance) + ")");
  executeOrThrow(
      database,
      "INSERT INTO course_replay_stages(id,course_replay_id,stage_index,"
      "replay_id,rest_micros_after_stage) VALUES(88,77,0,42,250000)");
  return courseKey;
}

void insertPartialCourseFixture(sqlite3 *database, const FixtureFacts &chart,
                                std::string_view courseKey) {
  executeOrThrow(
      database,
      "INSERT INTO course_replays(id,course_id,course_key,course_name,"
      "course_group_name,constraint_json,gauge_type,gauge_profile,"
      "gauge_auto_shift,ln_mode,requested_play_option,assist_option,"
      "final_score,max_combo,final_gauge,clear_type,completed_charts,"
      "total_charts,created_at,ruleset_version,eligibility,provenance_json) "
      "VALUES(78,13," +
          sqlQuote(courseKey) +
          ",'Partial Migration Course','Folder',"
          "'[\"grade_mirror\",\"no_speed\",\"gauge_7k\",\"cn\"]',"
          "2,1,0,1,'MIRROR','OFF',"
          "3,2,64.5,200,1,2,'2026-07-25 01:04:04',0,2," +
          sqlQuote(chart.provenance) + ")");
  executeOrThrow(
      database,
      "INSERT INTO course_replay_stages(id,course_replay_id,stage_index,"
      "replay_id,rest_micros_after_stage) VALUES(89,78,0,42,500000)");
}

std::string insertEarlierSameStemFixture(sqlite3 *database,
                                         const FixtureFacts &chart) {
  const std::string attemptId = "123e4567-e89b-42d3-a456-426614174002";
  executeOrThrow(
      database,
      "INSERT INTO replays(id,chart_path,chart_md5,chart_sha256,chart_title,"
      "chart_artist,ln_mode,gauge_type,gauge_auto_shift,final_score,max_combo,"
      "final_gauge,clear_type,play_option,play_option2,assist_option,created_at,"
      "ruleset_version,eligibility,provenance_json,attempt_id,"
      "attempt_fingerprint) VALUES(5,'BMS/earlier.bms','" +
          chart.md5 + "','" + chart.sha256 +
          "','Earlier','Artist',1,2,0,2,1,50.0,300,'NORMAL','NORMAL','OFF',"
          "'2026-07-24 01:02:03',0,2," + sqlQuote(chart.provenance) + ",'" +
          attemptId + "','" + std::string(64, 'e') + "')");
  executeOrThrow(
      database,
      "INSERT INTO replay_events(replay_id,event_index,action,lane,"
      "note_time_micros,song_time_micros,judge_time_micros,judgement,"
      "diff_micros,gauge,gauge_type,combo,score) VALUES"
      "(5,0,0,1,2000,2100,2100,0,100,50.0,2,1,2),"
      "(5,1,1,1,2000,2300,2300,6,300,50.0,2,1,2)");
  executeOrThrow(
      database,
      "INSERT INTO pending_chart_score_writes(attempt_id,replay_id,"
      "chart_path,chart_md5,chart_sha256,chart_title,chart_artist,ln_mode,"
      "score,max_score,max_combo,combo_break,pgreat,great,good,bad,poor,kpoor,"
      "fast,slow,final_gauge,clear_type,ruleset_version,eligibility,"
      "provenance_json,created_at,recovery_attempts) VALUES('" +
          attemptId + "',5,'BMS/earlier.bms','" + chart.md5 + "','" +
          chart.sha256 +
          "','Earlier','Artist',1,2,2,1,0,1,0,0,0,0,0,1,0,50.0,300,0,2," +
          sqlQuote(chart.provenance) + ",'2026-07-24 01:02:03',0)");
  return attemptId;
}

void testMigratesChartRowsToReplayFileAndCompactResult() {
  TemporaryDirectory temporary;
  const auto databasePath = temporary.path() / "replay.db";
  Database database(databasePath);
  replay_schema10_fixture::createExactSchema(database.get());
  const FixtureFacts fixture = insertChartFixture(database.get());

  replay::BeatorajaReplayCodec codec;
  replay::ReplayFileStore store(temporary.path());
  const auto outcome = replay_repository_detail::migrateReplaySchema10To11(
      database.get(), temporary.path(), codec, store);

  if (outcome.status !=
      replay_repository_detail::ReplayMigrationOutcome::Status::Migrated) {
    std::cerr << "migration diagnostic: " << outcome.diagnostic << '\n';
  }

  expect(outcome.status ==
             replay_repository_detail::ReplayMigrationOutcome::Status::Migrated,
         "schema-v10 chart rows migrate successfully");
  expect(outcome.chartFiles == 1 && outcome.courseFiles == 0,
         "migration reports the finalized chart file");
  expect(integer(database.get(), "PRAGMA user_version") == 11,
         "migration advances the schema exactly once");
  expect(!tableExists(database.get(), "replays") &&
             !tableExists(database.get(), "replay_events") &&
             !tableExists(database.get(), "replay_touch_samples") &&
             !tableExists(database.get(), "replay_lane_cover_events"),
         "migration removes legacy replay row tables");
  expect(integer(database.get(),
                 "SELECT count(*) FROM chart_results WHERE id=42 AND score=3 "
                 "AND max_score=4 AND max_combo=2 AND played_at_unix_ms=" +
                     std::to_string(fixture.playedAtUnixMillis)) == 1,
         "compact result preserves the public ID and result facts");
  expect(text(database.get(),
              "SELECT provenance_json FROM chart_results WHERE id=42") ==
             fixture.provenance,
         "compact result preserves provenance without coupling it to replay");
  expect(text(database.get(),
              "SELECT relative_path FROM replay_files WHERE "
              "chart_result_id=42") ==
             "replay/C" + fixture.sha256 + ".brd",
         "undefined-CN replay uses the exact Beatoraja path layout");
  expect(integer(database.get(),
                 "SELECT history_index FROM replay_files WHERE "
                 "chart_result_id=42") == 0,
         "first deterministic stem receives history index zero");
  expect(integer(database.get(),
                 "SELECT result_id FROM pending_chart_score_writes WHERE "
                 "attempt_id=" +
                     sqlQuote(fixture.attemptId)) == 42,
         "pending projection relinks to the compact result");
  expect(text(database.get(), "SELECT payload_json FROM ir_outbox WHERE id=7") ==
             "{\"score\":3}",
         "provider outbox payload remains byte-for-byte independent");
  expect(integer(database.get(),
                 "SELECT result_id FROM ir_submission_receipts WHERE id=8") ==
             42,
         "IR receipt relinks to the compact result");
  expect(text(database.get(),
              "SELECT remote_score_id FROM ir_remote_scores WHERE "
              "provider_id='tachi' AND server_origin='https://example.test'") ==
             "mirror-1",
         "provider remote mirror survives independently of replay playback");
  expect(integer(database.get(),
                 "SELECT count(*) FROM ir_submission_snapshots") == 0,
         "migration never manufactures an IR snapshot");
  expect(integer(database.get(), "SELECT count(*) FROM pragma_foreign_key_check") ==
             0,
         "migrated database has no foreign-key violations");

  const std::filesystem::path relative =
      "replay/C" + fixture.sha256 + ".brd";
  const std::filesystem::path replayPath = temporary.path() / relative;
  expect(std::filesystem::is_regular_file(replayPath),
         "migration writes a durable BRD file");
  replay::ReplayFileMetadata metadata{
      .relativePath = relative,
      .sha256 = text(database.get(),
                     "SELECT content_sha256 FROM replay_files WHERE "
                     "chart_result_id=42"),
      .compressedSize = static_cast<std::uint64_t>(integer(
          database.get(),
          "SELECT compressed_size FROM replay_files WHERE chart_result_id=42")),
      .codecVersion = replay::BeatorajaReplayCodec::kCodecVersion,
  };
  const auto decoded = store.load(metadata, codec);
  expect(decoded.chart.has_value(), "migrated BRD decodes as a chart replay");
  if (decoded.chart.has_value()) {
    expect(decoded.chart->setup.chartSha256 == fixture.sha256 &&
               decoded.chart->setup.longNoteMode == 1 &&
               decoded.chart->setup.initialLaneCoverPercent == 30,
           "decoded replay preserves chart setup and stock lane cover");
    expect(decoded.chart->input.size() == 2 &&
               decoded.chart->touchSamples.size() == 1 &&
               decoded.chart->laneCoverEvents.size() == 2,
           "decoded replay preserves input, touch, and timed cover tracks");
    expect(decoded.chart->legacy.has_value() &&
               decoded.chart->legacy->events.size() == 2,
           "migration-only legacy playback annotations remain in extension");
  }
}

void testMigratesCompleteAndPartialCoursesToBeatorajaCourseFiles() {
  TemporaryDirectory temporary;
  Database database(temporary.path() / "replay.db");
  replay_schema10_fixture::createExactSchema(database.get());
  const FixtureFacts chart = insertChartFixture(database.get());
  const std::string courseKey = insertCourseFixture(database.get(), chart);
  insertPartialCourseFixture(database.get(), chart, courseKey);

  replay::BeatorajaReplayCodec codec;
  replay::ReplayFileStore store(temporary.path());
  const auto outcome = replay_repository_detail::migrateReplaySchema10To11(
      database.get(), temporary.path(), codec, store);
  if (outcome.status !=
      replay_repository_detail::ReplayMigrationOutcome::Status::Migrated) {
    std::cerr << "course migration diagnostic: " << outcome.diagnostic << '\n';
  }

  expect(outcome.status ==
             replay_repository_detail::ReplayMigrationOutcome::Status::Migrated &&
             outcome.chartFiles == 1 && outcome.courseFiles == 2,
         "complete and partial courses migrate their chart and course BRDs");
  expect(integer(database.get(),
                 "SELECT count(*) FROM course_results WHERE id=77 AND "
                 "legacy_course_id=12 AND completed_charts=1 AND "
                 "total_charts=1 AND final_score=3 AND max_score=4") == 1,
         "course result preserves its public ID and aggregate facts");
  expect(text(database.get(),
              "SELECT course_key FROM course_results WHERE id=77") ==
             courseKey,
         "course result preserves its canonical content identity");
  expect(integer(database.get(),
                 "SELECT count(*) FROM course_results WHERE id=78 AND "
                 "completed_charts=1 AND total_charts=2 AND clear_type=200") ==
             1,
         "partial course preserves progress and partial-clear result facts");
  expect(integer(database.get(),
                 "SELECT count(*) FROM course_result_stages WHERE "
                 "course_result_id=77 AND stage_index=0 AND score=3 AND "
                 "max_score=4") == 1,
         "course stage result is independent of replay playback rows");

  const std::string relative =
      "replay/C" + chart.sha256.substr(0, 10) + "_040913.brd";
  expect(text(database.get(),
              "SELECT relative_path FROM replay_files WHERE "
              "course_result_id=77") == relative,
         "course replay uses Beatoraja's stage-hash filename layout");
  expect(text(database.get(),
              "SELECT relative_path FROM replay_files WHERE "
              "course_result_id=78") == relative.substr(0, relative.size() - 4) +
                                                    "_1.brd",
         "later partial course uses the deterministic next history slot");
  replay::ReplayFileMetadata metadata{
      .relativePath = relative,
      .sha256 = text(database.get(),
                     "SELECT content_sha256 FROM replay_files WHERE "
                     "course_result_id=77"),
      .compressedSize = static_cast<std::uint64_t>(integer(
          database.get(),
          "SELECT compressed_size FROM replay_files WHERE course_result_id=77")),
      .codecVersion = replay::BeatorajaReplayCodec::kCodecVersion,
  };
  const auto decoded = store.load(metadata, codec);
  expect(decoded.course.has_value() && decoded.course->stages.size() == 1 &&
             decoded.course->restMicrosAfterStage ==
                 std::vector<std::int64_t>{250000},
         "course BRD decodes with exact stage order and rest timing");
  if (decoded.course.has_value()) {
    expect(decoded.course->stages.front().setup.chartSha256 == chart.sha256 &&
               decoded.course->stages.front().legacy.has_value(),
           "course BRD preserves the migrated stage identity and extension");
  }
}

void testAssignsSameStemHistoryByTimestampThenPublicId() {
  TemporaryDirectory temporary;
  Database database(temporary.path() / "replay.db");
  replay_schema10_fixture::createExactSchema(database.get());
  const FixtureFacts chart = insertChartFixture(database.get());
  insertEarlierSameStemFixture(database.get(), chart);

  replay::BeatorajaReplayCodec codec;
  replay::ReplayFileStore store(temporary.path());
  const auto outcome = replay_repository_detail::migrateReplaySchema10To11(
      database.get(), temporary.path(), codec, store);
  if (outcome.status !=
      replay_repository_detail::ReplayMigrationOutcome::Status::Migrated) {
    std::cerr << "history migration diagnostic: " << outcome.diagnostic << '\n';
  }

  const std::string stem = "C" + chart.sha256;
  expect(outcome.status ==
             replay_repository_detail::ReplayMigrationOutcome::Status::Migrated &&
             outcome.chartFiles == 2,
         "two same-stem replay rows migrate");
  expect(text(database.get(),
              "SELECT relative_path FROM replay_files WHERE "
              "chart_result_id=5") ==
             "replay/" + stem + ".brd",
         "earlier timestamp receives unsuffixed Beatoraja history slot");
  expect(text(database.get(),
              "SELECT relative_path FROM replay_files WHERE "
              "chart_result_id=42") ==
             "replay/" + stem + "_1.brd",
         "later timestamp receives the next Beatoraja history slot");
  expect(integer(database.get(),
                 "SELECT history_index FROM replay_files WHERE "
                 "chart_result_id=5") == 0 &&
             integer(database.get(),
                     "SELECT history_index FROM replay_files WHERE "
                     "chart_result_id=42") == 1,
         "history indexes are independent of insertion and public ID order");
}

void testEveryDatabaseFailureRollsBackAndRetryReusesFiles() {
  const std::vector<std::string_view> phases{
      "legacy-read",
      "encode",
      "pre-cutover-revalidation",
      "begin",
      "schema-create",
      "copy-chart",
      "copy-course",
      "copy-durable-work",
      "count-verification",
      "foreign-key-verification",
      "legacy-drop",
      "version-update",
      "commit",
  };
  for (const std::string_view phase : phases) {
    TemporaryDirectory temporary;
    Database database(temporary.path() / "replay.db");
    replay_schema10_fixture::createExactSchema(database.get());
    const FixtureFacts chart = insertChartFixture(database.get());
    insertCourseFixture(database.get(), chart);
    const LegacySnapshot before = snapshotLegacyDatabase(database.get());

    replay::BeatorajaReplayCodec codec;
    replay::ReplayFileStore store(temporary.path());
    const auto failed = replay_repository_detail::migrateReplaySchema10To11(
        database.get(), temporary.path(), codec, store,
        {.failAt = [phase](std::string_view candidate, std::int64_t) {
          return candidate == phase;
        }});
    expect(failed.status !=
               replay_repository_detail::ReplayMigrationOutcome::Status::Migrated &&
               failed.status != replay_repository_detail::
                                    ReplayMigrationOutcome::Status::AlreadyCurrent,
           std::string("injected migration phase fails: ") +
               std::string(phase));
    expect(snapshotLegacyDatabase(database.get()) == before,
           std::string("injected phase leaves all v10 rows and schema intact: ") +
               std::string(phase));
    if (phase != "legacy-read" && phase != "encode") {
      expect(failed.chartFiles == 1 && failed.courseFiles == 1,
             std::string("failure reports already-finalized files: ") +
                 std::string(phase));
    }

    replay::ReplayFileStore retryStore(temporary.path());
    const auto retry = replay_repository_detail::migrateReplaySchema10To11(
        database.get(), temporary.path(), codec, retryStore);
    expect(retry.status ==
                   replay_repository_detail::ReplayMigrationOutcome::Status::Migrated &&
               retry.chartFiles == 1 && retry.courseFiles == 1,
           std::string("retry validates or reuses deterministic files: ") +
               std::string(phase));
  }
}

void testEveryReplayFileFailurePreservesV10AndCanRetry() {
  const std::vector<std::string_view> phases{
      "write", "file-sync", "close", "rename", "directory-sync",
      "read-back", "decode", "hash",
  };
  for (const std::string_view phase : phases) {
    TemporaryDirectory temporary;
    Database database(temporary.path() / "replay.db");
    replay_schema10_fixture::createExactSchema(database.get());
    const FixtureFacts chart = insertChartFixture(database.get());
    insertCourseFixture(database.get(), chart);
    const LegacySnapshot before = snapshotLegacyDatabase(database.get());

    replay::BeatorajaReplayCodec codec;
    replay::ReplayFileStore failingStore(
        temporary.path(),
        {.failAt = [phase](std::string_view candidate) {
          return candidate == phase;
        }});
    const auto failed = replay_repository_detail::migrateReplaySchema10To11(
        database.get(), temporary.path(), codec, failingStore);
    expect(failed.status == replay_repository_detail::ReplayMigrationOutcome::
                                Status::FileFailure,
           std::string("injected replay file phase fails: ") +
               std::string(phase));
    expect(snapshotLegacyDatabase(database.get()) == before,
           std::string("file failure leaves every v10 table intact: ") +
               std::string(phase));

    replay::ReplayFileStore retryStore(temporary.path());
    const auto retry = replay_repository_detail::migrateReplaySchema10To11(
        database.get(), temporary.path(), codec, retryStore);
    expect(retry.status ==
                   replay_repository_detail::ReplayMigrationOutcome::Status::Migrated &&
               retry.chartFiles == 1 && retry.courseFiles == 1,
           std::string("file failure retry reuses safe final paths: ") +
               std::string(phase));
  }
}

void testRepositoryStartupRunsAtomicV10Migration() {
  TemporaryDirectory temporary;
  const auto databasePath = temporary.path() / "replays.db";
  FixtureFacts chart;
  {
    Database database(databasePath);
    replay_schema10_fixture::createExactSchema(database.get());
    chart = insertChartFixture(database.get());
  }

  ReplayRepository repository(databasePath);
  expect(repository.EnsureSchema(),
         "normal replay repository startup migrates a schema-v10 profile");
  repository.Shutdown();

  Database migrated(databasePath);
  expect(integer(migrated.get(), "PRAGMA user_version") == 11 &&
             integer(migrated.get(),
                     "SELECT count(*) FROM chart_results WHERE id=42") == 1,
         "startup exposes only the committed compact schema");
  expect(std::filesystem::is_regular_file(
             temporary.path() / "replay" / ("C" + chart.sha256 + ".brd")),
         "startup migration writes replay files at the profile root");
}

} // namespace

int main() {
  testMigratesChartRowsToReplayFileAndCompactResult();
  testMigratesCompleteAndPartialCoursesToBeatorajaCourseFiles();
  testAssignsSameStemHistoryByTimestampThenPublicId();
  testEveryDatabaseFailureRollsBackAndRetryReusesFiles();
  testEveryReplayFileFailurePreservesV10AndCanRetry();
  testRepositoryStartupRunsAtomicV10Migration();
  if (failures != 0) {
    std::cerr << failures << " replay file migration test(s) failed\n";
    return 1;
  }
  std::cout << "replay file migration tests passed\n";
  return 0;
}
