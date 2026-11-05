#pragma once
#include "PlayRecap.h"
#include "../ClearLampOrder.h"
#include <algorithm>
#include "../sqlite3.h"
#include <memory>

namespace recap {
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
inline std::string text(sqlite3_stmt *row, int index) {
  const auto *value = sqlite3_column_text(row, index);
  return value ? reinterpret_cast<const char *>(value) : "";
}
inline std::string utcTime(std::int64_t seconds) {
  const auto value = static_cast<std::time_t>(seconds);
  std::tm time{};
#ifdef _WIN32
  gmtime_s(&time, &value);
#else
  gmtime_r(&value, &time);
#endif
  char result[32]{};
  std::strftime(result, sizeof(result), "%Y-%m-%d %H:%M:%S", &time);
  return result;
}
// A session is the latest contiguous run before the exclusive upper boundary.
// Imported rows do not create or join local sessions.
inline std::optional<Range> sessionRange(sqlite3 *db, std::int64_t before,
                                        std::string *error = nullptr) {
  sqlite3_stmt *raw = nullptr;
  const char *sql = "SELECT CAST(strftime('%s',created_at) AS INTEGER) FROM scores "
      "WHERE score_source=0 AND created_at<? ORDER BY created_at DESC,id DESC";
  const int prepared = sqlite3_prepare_v2(db, sql, -1, &raw, nullptr);
  Statement stmt(raw, sqlite3_finalize);
  if (prepared != SQLITE_OK) { if (error) *error = sqlite3_errmsg(db); return {}; }
  const auto cutoff = utcTime(before);
  sqlite3_bind_text(raw, 1, cutoff.c_str(), -1, SQLITE_TRANSIENT);
  std::optional<Range> range;
  int rc;
  while ((rc = sqlite3_step(raw)) == SQLITE_ROW) {
    if (sqlite3_column_type(raw, 0) == SQLITE_NULL) continue;
    const auto played = sqlite3_column_int64(raw, 0);
    if (!range) range = Range{played, played + 1};
    else if (range->start - played >= 3600) return range;
    else range->start = played;
  }
  if (rc != SQLITE_DONE && error) *error = sqlite3_errmsg(db);
  return range;
}
inline Snapshot load(sqlite3 *db, Range range, const std::string &scoreEligible,
                     const std::string &lampEligible, const std::string &clearRank) {
  Snapshot result{.range = range};
  if (range.start >= range.end) { result.error = "Invalid recap range"; return result; }
  // Only histories of charts/modes actually played in the window are aggregated.
  // Each metric has independent eligibility. Exact BP includes unpassed notes;
  // only the legacy judgement-count fallback needs evidence of completion.
  // Unknown hashes cannot identify charts. Legacy unknown-LN attempts before the
  // window remain baselines for every mode, matching the existing best cache.
  const std::string completed =
      "s.max_score>0 AND s.max_score%2=0 AND s.kpoor>=0 AND "
      "s.pgreat>=0 AND s.great>=0 AND s.good>=0 AND s.bad>=0 AND s.poor>=0 AND "
      "(s.clear_type>0 OR s.pgreat+s.great+s.good+s.poor+"
      "CASE WHEN json_valid(s.provenance_json) AND "
      "json_extract(s.provenance_json,'$.ruleset.id')='beatoraja' AND "
      "json_extract(s.provenance_json,'$.gaugeProfile')='standard-9-keys' THEN 0 ELSE s.bad END=s.max_score/2)";
  // BMSPlayer.createScoreData: BAD + POOR + MISS + notes - pastNotes.
  // captureChartScoreWrite already persists that exact metric in bad_points.
  // Never replace a malformed stored BP with a potentially smaller fallback.
  const std::string badPoints = "(CASE WHEN s.bad_points IS NOT NULL THEN s.bad_points "
      "WHEN " + completed + " THEN s.bad+s.poor+s.kpoor END)";
  const std::string query =
      "WITH touched AS (SELECT DISTINCT lower(trim(chart_sha256)) hash,ln_mode FROM scores "
      "WHERE score_source=0 AND trim(chart_sha256)<>'' AND created_at>=?1 AND created_at<?2), "
      "attempts AS (SELECT lower(trim(s.chart_sha256)) hash,t.ln_mode,s.chart_title,s.created_at,"
      "CASE WHEN " + lampEligible + " THEN " + clearRank + " END lamp,"
      "CASE WHEN " + scoreEligible + " THEN s.score END ex,"
      "CASE WHEN (" + scoreEligible + ") AND typeof(" + badPoints + ")='integer' "
      "AND " + badPoints + " BETWEEN 0 AND 2147483647 THEN " + badPoints + " END bp "
      "FROM scores s JOIN touched t ON lower(trim(s.chart_sha256))=t.hash AND (s.ln_mode=t.ln_mode OR (s.ln_mode=-1 AND s.created_at<?1)) "
      "WHERE s.score_source=0 AND s.created_at<?2) "
      "SELECT hash,ln_mode,COALESCE(MAX(CASE WHEN created_at>=?1 THEN chart_title END),MAX(chart_title),hash),"
      "MAX(CASE WHEN created_at<?1 THEN lamp END),MAX(CASE WHEN created_at>=?1 THEN lamp END),"
      "MAX(CASE WHEN created_at<?1 THEN ex END),MAX(CASE WHEN created_at>=?1 THEN ex END),"
      "MIN(CASE WHEN created_at<?1 THEN bp END),MIN(CASE WHEN created_at>=?1 THEN bp END) "
      "FROM attempts GROUP BY hash,ln_mode ORDER BY 3 COLLATE NOCASE,hash,ln_mode";
  sqlite3_stmt *raw = nullptr;
  const int prepared = sqlite3_prepare_v2(db, query.c_str(), -1, &raw, nullptr);
  Statement stmt(raw, sqlite3_finalize);
  if (prepared != SQLITE_OK) { result.error = sqlite3_errmsg(db); return result; }
  const auto start = utcTime(range.start), end = utcTime(range.end);
  sqlite3_bind_text(raw, 1, start.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(raw, 2, end.c_str(), -1, SQLITE_TRANSIENT);
  int rc;
  while ((rc = sqlite3_step(raw)) == SQLITE_ROW) {
    for (int kind = 0; kind < 3; ++kind) {
      const int oldColumn = 3 + kind * 2, newColumn = oldColumn + 1;
      if (sqlite3_column_type(raw, newColumn) == SQLITE_NULL) continue;
      const auto old = sqlite3_column_type(raw, oldColumn) == SQLITE_NULL ? std::optional<int>{}
          : std::optional{sqlite3_column_int(raw, oldColumn)};
      const int value = sqlite3_column_int(raw, newColumn);
      if (old && (kind == 2 ? value >= *old : value <= *old)) continue;
      auto title = text(raw, 2);
      if (title.empty()) title = text(raw, 0);
      result.sections[kind].push_back({text(raw, 0), std::move(title), sqlite3_column_int(raw, 1), old, value});
    }
  }
  if (rc != SQLITE_DONE) { result.sections = {}; result.error = sqlite3_errmsg(db); }
  std::stable_sort(result.sections[0].begin(), result.sections[0].end(),
      [](const Upgrade &left, const Upgrade &right) {
        return clearLampOrder(left.after) > clearLampOrder(right.after);
      });
  return result;
}
} // namespace recap
