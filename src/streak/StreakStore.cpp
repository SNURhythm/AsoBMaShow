#include "StreakStore.h"
#include "../../yoga/lib/nlohmann/json.hpp"
#include "../repositories/SqliteRAII.h"
#include <stdexcept>

namespace streak {
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Level, source, level, label)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Assignment, id, name, levels, eligibleLamp,
    eligibleOrBelow, targetLamp, scoreNumerator, scoreDenominator, customRate, chartCount, points)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Config, dailyGoal, weeklyGoal, monthlyGoal, assignments)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Progress, requirement, completedRounds, bonusRounds, earnedPoints, charts, usedCharts)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Day, config, played, frozen, points, assignments)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(State, version, configurations, days, savings, freezes,
    freezePurchases, currentStreak, bestStreak, milestones)
namespace {
void require(bool condition, sqlite3 *db) {
  if (!condition) throw std::runtime_error("Streak storage: " + sqliteDatabaseError(db));
}
void execute(sqlite3 *db, const char *sql) {
  require(sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK, db);
}
}
State load(sqlite3 *db) {
  SqliteStatementHandle exists;
  require(prepareSqliteStatement(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name='streak_state'", exists) == SQLITE_OK, db);
  const int existence = sqlite3_step(exists);
  if (existence == SQLITE_DONE) return {};
  require(existence == SQLITE_ROW, db);
  SqliteStatementHandle query;
  require(prepareSqliteStatement(db, "SELECT body FROM streak_state WHERE id=1", query) == SQLITE_OK, db);
  const int result = sqlite3_step(query);
  if (result == SQLITE_DONE) return {};
  require(result == SQLITE_ROW, db);
  auto json = nlohmann::json::parse(sqliteColumnString(query, 0));
  const auto version = json.at("version").get<int>();
  if (version != 1 && version != 2)
    throw std::runtime_error("This streak data needs a different app version.");
  if (version == 1) {
    // Preserve all previously earned totals and savings. The old format did
    // not track bonus rounds or purchases, so their new counters start at zero.
    json["version"] = 2;
    json["freezePurchases"] = 0;
    for (auto &entry : json.at("days")) {
      for (auto &progress : entry.at(1).at("assignments")) {
        progress["bonusRounds"] = 0;
        progress["earnedPoints"] = static_cast<std::int64_t>(progress.at("completedRounds").get<int>()) *
            progress.at("requirement").at("points").get<int>();
      }
    }
  }
  auto state = json.get<State>();
  if (state.savings < 0 || state.freezes < 0 || state.bestStreak < 0 || state.freezePurchases < 0)
    throw std::runtime_error("Invalid streak balances.");
  for (const auto &[id, config] : state.configurations)
    if (auto error = validate(config); !error.empty()) throw std::runtime_error(error);
  for (const auto &[id, day] : state.days) {
    if (auto error = validate(day.config); !error.empty()) throw std::runtime_error(error);
    if (day.points < 0 || (day.frozen && day.played) ||
        day.assignments.size() != day.config.assignments.size())
      throw std::runtime_error("Invalid daily streak progress.");
    std::int64_t earned = 0;
    for (std::size_t i = 0; i < day.assignments.size(); ++i) {
      const auto &progress = day.assignments[i];
      const auto &base = day.config.assignments[i];
      Config active = day.config;
      active.assignments = {progress.requirement};
      if (auto error = validate(active); !error.empty()) throw std::runtime_error(error);
      if (progress.requirement.id != base.id || progress.requirement.points != base.points ||
          progress.completedRounds < 0 || progress.bonusRounds < 0 ||
          progress.bonusRounds > dailyBonusLimit || progress.bonusRounds > progress.completedRounds ||
          progress.earnedPoints < progress.bonusRounds ||
          progress.earnedPoints > static_cast<std::int64_t>(progress.completedRounds - progress.bonusRounds) *
              progress.requirement.points + progress.bonusRounds ||
          progress.charts < 0 ||
          progress.charts >= progress.requirement.chartCount ||
          static_cast<std::uint64_t>(progress.charts) + progress.completedRounds > progress.usedCharts.size() ||
          progress.usedCharts.contains("") || (!day.played && !progress.usedCharts.empty()))
        throw std::runtime_error("Invalid assignment progress.");
      earned += progress.earnedPoints;
    }
    if (earned != day.points || (!day.played && day.points != 0))
      throw std::runtime_error("Daily points do not match completed assignments.");
  }
  return state;
}
void save(sqlite3 *db, const State &state) {
  execute(db, "CREATE TABLE IF NOT EXISTS streak_state (id INTEGER PRIMARY KEY CHECK(id=1), body TEXT NOT NULL)");
  SqliteStatementHandle query;
  require(prepareSqliteStatement(db, "INSERT INTO streak_state(id,body) VALUES(1,?) ON CONFLICT(id) DO UPDATE SET body=excluded.body", query) == SQLITE_OK, db);
  require(bindSqliteText(query, 1, nlohmann::json(state).dump()), db);
  require(sqlite3_step(query) == SQLITE_DONE, db);
}
void recordSavedPlay(sqlite3 *db, const std::filesystem::path &chartPath,
                     Play play, const std::string &md5,
                     const std::string &createdAt, std::uint32_t randomChoice) {
  if (!play.manual) return;
  auto state = load(db);
  if (state.configurations.empty()) return;
  SqliteStatementHandle time;
  require(prepareSqliteStatement(db, "SELECT CAST(strftime('%s', ?) AS INTEGER)", time) == SQLITE_OK, db);
  require(bindSqliteText(time, 1, createdAt), db);
  require(sqlite3_step(time) == SQLITE_ROW && sqlite3_column_type(time, 0) != SQLITE_NULL, db);
  play.day = localDay(sqlite3_column_int64(time, 0));
  if (play.day < state.configurations.begin()->first) return;

  // Read only indexed metadata for this chart, never chart/archive payloads.
  const auto pathText = chartPath.u8string();
  sqlite3 *raw = nullptr;
  const int opened = sqlite3_open_v2(reinterpret_cast<const char *>(pathText.c_str()),
                                    &raw, SQLITE_OPEN_READONLY, nullptr);
  SqliteConnectionHandle charts(raw);
  if (opened != SQLITE_OK) throw std::runtime_error("Difficulty-table storage is unavailable.");
  SqliteStatementHandle query;
  require(prepareSqliteStatement(charts.get(),
      "SELECT DISTINCT t.source_url, e.level FROM difficulty_table_entries e "
      "JOIN difficulty_tables t ON t.id=e.table_id "
      "WHERE (e.sha256=? AND e.sha256<>'') OR (e.md5=? AND e.md5<>'')", query) == SQLITE_OK, charts.get());
  require(bindSqliteText(query, 1, play.chart) && bindSqliteText(query, 2, md5), charts.get());
  int result;
  while ((result = sqlite3_step(query)) == SQLITE_ROW)
    play.levels.push_back({sqliteColumnString(query, 0), sqliteColumnString(query, 1), {}});
  require(result == SQLITE_DONE, charts.get());
  record(state, play, randomChoice);
  save(db, state);
}
} // namespace streak
