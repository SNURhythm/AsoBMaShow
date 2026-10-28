#include "ChartGoalStore.h"
#include "../ClearLampOrder.h"
#include "../repositories/SqliteRAII.h"
#include "../../yoga/lib/nlohmann/json.hpp"
#include <stdexcept>

namespace chart_goals {
namespace {
void require(bool condition, sqlite3 *db) {
  if (!condition) throw std::runtime_error("Goal storage: " + sqliteDatabaseError(db));
}
bool exists(sqlite3 *db) {
  SqliteStatementHandle query;
  require(prepareSqliteStatement(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name='chart_goals'", query) == SQLITE_OK, db);
  const int rc = sqlite3_step(query);
  require(rc == SQLITE_ROW || rc == SQLITE_DONE, db);
  return rc == SQLITE_ROW;
}
int integer(const nlohmann::json &body, const char *key, int minimum, int maximum) {
  const auto &value = body.at(key);
  if (!value.is_number_integer() || value < minimum || value > maximum)
    throw std::runtime_error("Invalid goal data.");
  return value.get<int>();
}
void validateGoal(const Goal &goal) {
  validate(goal.target);
  (void)normalizedHash(goal.chart.SHA256);
  if (goal.setAtUnixMillis < 0 || goal.achievedAtUnixMillis < 0 ||
      goal.setAtUnixMillis > 253402300799999LL || goal.achievedAtUnixMillis > 253402300799999LL ||
      (goal.achievedAtUnixMillis > 0 && goal.achievedAtUnixMillis < goal.setAtUnixMillis))
    throw std::invalid_argument("Invalid goal dates.");
  if (goal.longNoteMode < 0 || goal.longNoteMode > 3 ||
      goal.chart.LnMode < 0 || goal.chart.LnMode > 3 ||
      goal.chart.TotalLongNotes < 0 || goal.chart.TotalBackSpinNotes < 0 ||
      goal.chart.Title.size() > 16384 || goal.chart.SubTitle.size() > 16384 ||
      goal.chart.MD5.size() > 32)
    throw std::invalid_argument("Invalid goal chart metadata.");
  const bool hasLn = goal.chart.TotalLongNotes > 0 || goal.chart.TotalBackSpinNotes > 0;
  if ((!hasLn && goal.longNoteMode != 0) ||
      (hasLn && (goal.longNoteMode == 0 ||
                 (goal.chart.LnMode > 0 && goal.chart.LnMode != goal.longNoteMode))))
    throw std::invalid_argument("Invalid goal long-note mode.");
}
Goal decode(sqlite3_stmt *query) {
  if (sqlite3_column_bytes(query, 2) > 65536) throw std::runtime_error("Invalid goal data size.");
  auto body = nlohmann::json::parse(sqliteColumnString(query, 2));
  if (integer(body, "version", 1, 2147483647) != 1)
    throw std::runtime_error("These goals need a different app version.");
  Goal goal;
  goal.chart.SHA256 = sqliteColumnString(query, 0);
  goal.longNoteMode = sqlite3_column_int(query, 1);
  goal.chart.MD5 = body.at("md5").get<std::string>();
  goal.chart.Title = body.at("title").get<std::string>();
  goal.chart.SubTitle = body.at("subtitle").get<std::string>();
  goal.chart.LnMode = integer(body, "lnMode", 0, 3);
  goal.chart.TotalLongNotes = integer(body, "longNotes", 0, 2147483647);
  goal.chart.TotalBackSpinNotes = integer(body, "backSpins", 0, 2147483647);
  goal.target.lamp = normalizeClearLamp(integer(body, "lamp", -1, 600));
  goal.target.scoreNumerator = integer(body, "numerator", 0, 10000);
  goal.target.scoreDenominator = integer(body, "denominator", 1, 10000);
  for (const auto &[name, output] : std::array{
           std::pair{"setAt", &goal.setAtUnixMillis},
           std::pair{"achievedAt", &goal.achievedAtUnixMillis}}) {
    if (!body.contains(name)) continue; // Version 1 rows predating date tracking.
    const auto &value = body.at(name);
    if (!value.is_number_integer() || value < 0 || value > 253402300799999LL)
      throw std::runtime_error("Invalid goal date.");
    *output = value.get<std::int64_t>();
  }
  validateGoal(goal);
  if (goal.chart.SHA256 != normalizedHash(goal.chart.SHA256))
    throw std::runtime_error("Invalid stored goal identity.");
  return goal;
}
void bindIdentity(sqlite3 *db, sqlite3_stmt *query, const std::string &hash, int mode) {
  require(sqlite3_bind_text(query, 1, hash.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK &&
          sqlite3_bind_int(query, 2, mode) == SQLITE_OK, db);
}
}
Index loadIndex(sqlite3 *db) {
  Index result;
  if (!exists(db)) return result;
  SqliteStatementHandle query;
  require(prepareSqliteStatement(db, "SELECT sha256, ln_mode FROM chart_goals", query) == SQLITE_OK, db);
  int rc;
  while ((rc = sqlite3_step(query)) == SQLITE_ROW) {
    const auto hash = sqliteColumnString(query, 0);
    const int mode = sqlite3_column_int(query, 1);
    if (mode < 0 || mode > 3 || normalizedHash(hash) != hash)
      throw std::runtime_error("Invalid stored goal identity.");
    result.modes[hash] |= 1u << mode;
  }
  require(rc == SQLITE_DONE, db);
  return result;
}
std::vector<Goal> loadForChart(sqlite3 *db, std::string_view sha256) {
  const auto hash = normalizedHash(sha256);
  std::vector<Goal> result;
  if (!exists(db)) return result;
  SqliteStatementHandle query;
  require(prepareSqliteStatement(db, "SELECT sha256, ln_mode, body FROM chart_goals WHERE sha256=? ORDER BY ln_mode LIMIT 4", query) == SQLITE_OK, db);
  require(sqlite3_bind_text(query, 1, hash.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK, db);
  int rc;
  while ((rc = sqlite3_step(query)) == SQLITE_ROW) result.push_back(decode(query));
  require(rc == SQLITE_DONE, db);
  return result;
}
Page loadPage(sqlite3 *db, int offset) {
  if (offset < 0) throw std::invalid_argument("Invalid goal page.");
  Page page;
  if (!exists(db)) return page;
  SqliteStatementHandle query;
  require(prepareSqliteStatement(db, "SELECT sha256, ln_mode, body FROM chart_goals ORDER BY sha256, ln_mode LIMIT 21 OFFSET ?", query) == SQLITE_OK, db);
  require(sqlite3_bind_int(query, 1, offset) == SQLITE_OK, db);
  int rc;
  while ((rc = sqlite3_step(query)) == SQLITE_ROW) {
    if (page.goals.size() == pageSize) { page.hasMore = true; break; }
    page.goals.push_back(decode(query));
  }
  require(rc == SQLITE_ROW || rc == SQLITE_DONE, db);
  return page;
}
std::optional<Goal> load(sqlite3 *db, std::string_view sha256, int mode) {
  const auto hash = normalizedHash(sha256);
  if (!exists(db)) return {};
  SqliteStatementHandle query;
  require(prepareSqliteStatement(db, "SELECT sha256, ln_mode, body FROM chart_goals WHERE sha256=? AND ln_mode=?", query) == SQLITE_OK, db);
  bindIdentity(db, query, hash, mode);
  const auto rc = sqlite3_step(query);
  require(rc == SQLITE_ROW || rc == SQLITE_DONE, db);
  if (rc == SQLITE_DONE) return {};
  return decode(query);
}
void save(sqlite3 *db, const Goal &goal) {
  validateGoal(goal);
  const auto hash = normalizedHash(goal.chart.SHA256);
  std::string error;
  SqliteTransactionHandle tx(db, "SAVEPOINT chart_goal_write", error,
      "RELEASE chart_goal_write", "ROLLBACK TO chart_goal_write; RELEASE chart_goal_write");
  if (!tx.active()) throw std::runtime_error(error);
  // Preserve unreadable future/corrupt rows, including on attempted replacement.
  (void)load(db, hash, goal.longNoteMode);
  require(sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS chart_goals (sha256 TEXT NOT NULL, ln_mode INTEGER NOT NULL, body TEXT NOT NULL, PRIMARY KEY (sha256, ln_mode))", nullptr, nullptr, nullptr) == SQLITE_OK, db);
  const auto body = nlohmann::json{{"version", 1}, {"md5", goal.chart.MD5},
      {"title", goal.chart.Title}, {"subtitle", goal.chart.SubTitle},
      {"lnMode", goal.chart.LnMode}, {"longNotes", goal.chart.TotalLongNotes},
      {"backSpins", goal.chart.TotalBackSpinNotes}, {"lamp", goal.target.lamp},
      {"numerator", goal.target.scoreNumerator}, {"denominator", goal.target.scoreDenominator},
      {"setAt", goal.setAtUnixMillis}, {"achievedAt", goal.achievedAtUnixMillis}}.dump();
  if (body.size() > 65536) throw std::invalid_argument("Goal metadata is too large.");
  SqliteStatementHandle query;
  require(prepareSqliteStatement(db, "INSERT INTO chart_goals VALUES (?, ?, ?) ON CONFLICT(sha256, ln_mode) DO UPDATE SET body=excluded.body", query) == SQLITE_OK, db);
  bindIdentity(db, query, hash, goal.longNoteMode);
  require(sqlite3_bind_text(query, 3, body.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK && sqlite3_step(query) == SQLITE_DONE, db);
  if (!tx.commit(error)) throw std::runtime_error(error);
}
void remove(sqlite3 *db, std::string_view sha256, int mode) {
  const auto hash = normalizedHash(sha256);
  std::string error;
  SqliteTransactionHandle tx(db, "SAVEPOINT chart_goal_write", error,
      "RELEASE chart_goal_write", "ROLLBACK TO chart_goal_write; RELEASE chart_goal_write");
  if (!tx.active()) throw std::runtime_error(error);
  if (load(db, hash, mode)) {
    SqliteStatementHandle query;
    require(prepareSqliteStatement(db, "DELETE FROM chart_goals WHERE sha256=? AND ln_mode=?", query) == SQLITE_OK, db);
    bindIdentity(db, query, hash, mode);
    require(sqlite3_step(query) == SQLITE_DONE, db);
  }
  if (!tx.commit(error)) throw std::runtime_error(error);
}
} // namespace chart_goals
