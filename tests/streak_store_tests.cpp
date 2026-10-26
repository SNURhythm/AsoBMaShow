#include "streak/StreakStore.h"
#include "sqlite3.h"
#include "../yoga/lib/nlohmann/json.hpp"
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while (false)
using namespace streak;
int main() {
  sqlite3 *db = nullptr;
  CHECK(sqlite3_open(":memory:", &db) == SQLITE_OK);
  CHECK(load(db).days.empty());
  State state;
  Config config;
  config.dailyGoal = 10;
  config.assignments = {{.id = "one", .name = "Explore", .levels = {{"https://table", "0", "sl0"}}, .points = 20}};
  configure(state, config, today());
  save(db, state);
  CHECK(load(db).days.size() == 1);
  CHECK(load(db).days.begin()->second.config.assignments[0].levels[0].source == "https://table");
  CHECK(sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr) == SQLITE_OK);
  state.savings = 123;
  save(db, state);
  CHECK(sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr) == SQLITE_OK);
  CHECK(load(db).savings == 0);
  sqlite3 *other = nullptr;
  CHECK(sqlite3_open(":memory:", &other) == SQLITE_OK);
  CHECK(load(other).days.empty());
  sqlite3_close(other);
  auto damaged = load(db);
  damaged.days.begin()->second.assignments[0].requirement.points = -40;
  save(db, damaged);
  bool damagedRejected = false;
  try { (void)load(db); } catch (const std::exception &) { damagedRejected = true; }
  CHECK(damagedRejected);
  State earned;
  configure(earned, config, today());
  Play attempt{.day = today(), .chart = "goal", .levels = {{"https://table", "0", ""}}, .lamp = 600};
  CHECK(record(earned, attempt, 0) == 10);
  attempt.chart = "bonus-one"; record(earned, attempt, 0);
  attempt.chart = "bonus-two"; CHECK(record(earned, attempt, 0) == 1);
  earned.savings = 100;
  CHECK(buyFreeze(earned, today()));
  save(db, earned);
  auto reopened = load(db);
  CHECK(reopened.version == 2 && freezePrice(reopened) == 30 && reopened.savings == 80);
  CHECK(reopened.days.begin()->second.assignments[0].bonusRounds == 1);
  CHECK(reopened.days.begin()->second.assignments[0].earnedPoints == 11);
  advance(reopened, today() + 3); // Consumes freeze, then breaks.
  CHECK(freezePrice(reopened) == 20);
  CHECK(buyFreeze(reopened, today() + 3));
  save(db, reopened);
  reopened = load(db);
  attempt.day = today() + 3; attempt.chart = "restart";
  record(reopened, attempt, 0);
  CHECK(freezePrice(reopened) == 30); // Old break cannot reset again after reload.

  auto rejects = [&](State bad) {
    save(db, bad);
    try { (void)load(db); return false; } catch (const std::exception &) { return true; }
  };
  auto bad = earned; bad.days.begin()->second.assignments[0].bonusRounds = 6;
  CHECK(rejects(bad));
  bad = earned; ++bad.days.begin()->second.assignments[0].earnedPoints;
  CHECK(rejects(bad));
  bad = earned; bad.freezePurchases = -1;
  CHECK(rejects(bad));

  // Reconstruct a v1 payload, including historical overflow and pending edits.
  configure(earned, config, today());
  earned.milestones.insert("streak:7");
  save(db, earned);
  sqlite3_stmt *query = nullptr;
  CHECK(sqlite3_prepare_v2(db, "SELECT body FROM streak_state", -1, &query, nullptr) == SQLITE_OK);
  CHECK(sqlite3_step(query) == SQLITE_ROW);
  auto legacy = nlohmann::json::parse(reinterpret_cast<const char *>(sqlite3_column_text(query, 0)));
  sqlite3_finalize(query);
  legacy["version"] = 1;
  legacy.erase("freezePurchases");
  for (auto &entry : legacy["days"]) {
    auto &day = entry[1];
    std::int64_t total = 0;
    for (auto &progress : day["assignments"]) {
      total += progress["completedRounds"].get<int>() * progress["requirement"]["points"].get<int>();
      progress.erase("bonusRounds"); progress.erase("earnedPoints");
    }
    day["points"] = total;
  }
  const auto body = legacy.dump();
  CHECK(sqlite3_prepare_v2(db, "UPDATE streak_state SET body=?", -1, &query, nullptr) == SQLITE_OK);
  CHECK(sqlite3_bind_text(query, 1, body.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK);
  CHECK(sqlite3_step(query) == SQLITE_DONE);
  sqlite3_finalize(query);
  auto migrated = load(db);
  CHECK(migrated.version == 2 && migrated.savings == 80 && migrated.freezes == 1);
  CHECK(migrated.configurations.size() == 2 && migrated.milestones.contains("streak:7"));
  CHECK(migrated.days.begin()->second.points == 40);
  CHECK(migrated.days.begin()->second.assignments[0].earnedPoints == 40);
  CHECK(migrated.days.begin()->second.assignments[0].usedCharts.size() == 3);
  CHECK(freezePrice(migrated) == 20);
  save(db, migrated);
  CHECK(load(db).days.begin()->second.points == 40);
  // Unsupported or damaged state must be reported, never silently replaced.
  CHECK(sqlite3_exec(db, "UPDATE streak_state SET body = '{\"version\":99}'", nullptr, nullptr, nullptr) == SQLITE_OK);
  bool rejected = false;
  try { (void)load(db); } catch (const std::exception &) { rejected = true; }
  CHECK(rejected);
  sqlite3_close(db);
  std::cout << "streak store tests passed\n";
}
