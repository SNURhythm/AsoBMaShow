#pragma once

#include "../sqlite3.h"

namespace replay_repository_legacy {

inline constexpr const char *kChartSummaryTableSql =
    "CREATE TABLE legacy_chart_result_summaries("
    "legacy_replay_id INTEGER PRIMARY KEY,chart_path TEXT,chart_md5 TEXT,"
    "chart_sha256 TEXT,chart_title TEXT,chart_artist TEXT,"
    "long_note_mode INTEGER,final_score INTEGER,max_combo INTEGER,"
    "final_gauge REAL,clear_type INTEGER,created_at TEXT,"
    "ruleset_version INTEGER,eligibility INTEGER,provenance_json TEXT,"
    "partial INTEGER NOT NULL CHECK(partial IN(0,1)))";

inline constexpr const char *kCourseSummaryTableSql =
    "CREATE TABLE legacy_course_result_summaries("
    "legacy_course_replay_id INTEGER PRIMARY KEY,legacy_course_id INTEGER,"
    "course_key TEXT,course_name TEXT,course_group_name TEXT,"
    "constraint_json TEXT,final_score INTEGER,max_combo INTEGER,"
    "final_gauge REAL,clear_type INTEGER,completed_charts INTEGER,"
    "total_charts INTEGER,created_at TEXT,ruleset_version INTEGER,"
    "eligibility INTEGER,provenance_json TEXT,"
    "partial INTEGER NOT NULL CHECK(partial IN(0,1)))";

inline constexpr const char *kChartShaIndexSql =
    "CREATE INDEX idx_legacy_chart_summaries_sha256 ON "
    "legacy_chart_result_summaries(chart_sha256,legacy_replay_id DESC)";
inline constexpr const char *kChartMd5IndexSql =
    "CREATE INDEX idx_legacy_chart_summaries_md5 ON "
    "legacy_chart_result_summaries(chart_md5,legacy_replay_id DESC)";
inline constexpr const char *kChartPathIndexSql =
    "CREATE INDEX idx_legacy_chart_summaries_path ON "
    "legacy_chart_result_summaries(chart_path,legacy_replay_id DESC)";
inline constexpr const char *kCourseLookupIndexSql =
    "CREATE INDEX idx_legacy_course_summaries_lookup ON "
    "legacy_course_result_summaries(course_key,legacy_course_id,"
    "legacy_course_replay_id DESC)";

inline constexpr const char *kReceiptTableSql =
    "CREATE TABLE ir_submission_receipts ("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,provider_id TEXT NOT NULL,"
    "server_origin TEXT NOT NULL,replay_id INTEGER,"
    "modern_chart_result_id INTEGER,attempt_id TEXT NOT NULL,chart_md5 TEXT,"
    "chart_sha256 TEXT NOT NULL,remote_user_id INTEGER,remote_chart_id TEXT,"
    "remote_score_id TEXT,confirmation_source INTEGER NOT NULL,"
    "observed_in_snapshot INTEGER NOT NULL DEFAULT 0,"
    "confirmed_at_ms INTEGER NOT NULL,"
    "UNIQUE(provider_id, server_origin, replay_id),"
    "UNIQUE(provider_id, server_origin, modern_chart_result_id),"
    "CHECK((replay_id IS NOT NULL) != (modern_chart_result_id IS NOT NULL)),"
    "CHECK(observed_in_snapshot IN (0, 1)),"
    "FOREIGN KEY(replay_id) REFERENCES "
    "legacy_chart_result_summaries(legacy_replay_id) ON DELETE CASCADE,"
    "FOREIGN KEY(modern_chart_result_id) REFERENCES modern_chart_results(id) "
    "ON DELETE CASCADE)";

[[nodiscard]] bool migrateToSummarySchema(sqlite3 *database, int sourceVersion);
[[nodiscard]] bool inspectCurrentSchema(sqlite3 *database);

} // namespace replay_repository_legacy
