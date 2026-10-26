#include "streak/StreakStore.h"
#include "sqlite3.h"
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
  // Unsupported or damaged state must be reported, never silently replaced.
  CHECK(sqlite3_exec(db, "UPDATE streak_state SET body = '{\"version\":99}'", nullptr, nullptr, nullptr) == SQLITE_OK);
  bool rejected = false;
  try { (void)load(db); } catch (const std::exception &) { rejected = true; }
  CHECK(rejected);
  sqlite3_close(db);
  std::cout << "streak store tests passed\n";
}
