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
  attempt.chart = "bonus-one"; CHECK(record(earned, attempt, 0) == 1);
  attempt.chart = "bonus-two"; CHECK(record(earned, attempt, 0) == 1);
  earned.savings = 100;
  CHECK(buyFreeze(earned, today()));
  save(db, earned);
  auto reopened = load(db);
  CHECK(reopened.version == 3 && freezePrice(reopened) == 25 && reopened.savings == 80);
  CHECK(reopened.days.begin()->second.assignments[0].bonusRounds == 2);
  CHECK(reopened.days.begin()->second.assignments[0].earnedPoints == 12);
  advance(reopened, today() + 3); // Consumes freeze, then breaks.
  CHECK(freezePrice(reopened) == 20);
  CHECK(buyFreeze(reopened, today() + 3));
  save(db, reopened);
  reopened = load(db);
  attempt.day = today() + 3; attempt.chart = "restart";
  record(reopened, attempt, 0);
  CHECK(freezePrice(reopened) == 25); // Old break cannot reset again after reload.

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
  const auto replaceBody = [&](const nlohmann::json &value) {
    const auto body = value.dump();
    sqlite3_stmt *update = nullptr;
    CHECK(sqlite3_prepare_v2(db, "UPDATE streak_state SET body=?", -1, &update, nullptr) == SQLITE_OK);
    CHECK(sqlite3_bind_text(update, 1, body.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK);
    CHECK(sqlite3_step(update) == SQLITE_DONE);
    sqlite3_finalize(update);
  };
  legacy["version"] = 2;
  for (auto &entry : legacy["days"])
    for (auto &progress : entry[1]["assignments"]) progress.erase("chartProgress");
  replaceBody(legacy);
  const auto versionTwo = load(db);
  CHECK(versionTwo.version == 3 && versionTwo.savings == earned.savings);
  CHECK(versionTwo.days.begin()->second.assignments[0].chartProgress.size() == 3);
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
  replaceBody(legacy);
  auto migrated = load(db);
  CHECK(migrated.version == 3 && migrated.savings == 80 && migrated.freezes == 1);
  CHECK(migrated.configurations.size() == 2 && migrated.milestones.contains("streak:7"));
  CHECK(migrated.days.begin()->second.points == 60);
  CHECK(migrated.days.begin()->second.assignments[0].earnedPoints == 60);
  CHECK(migrated.days.begin()->second.assignments[0].usedCharts.size() == 3);
  CHECK(freezePrice(migrated) == 20);
  save(db, migrated);
  CHECK(load(db).days.begin()->second.points == 60);
  // Version 2 days can already exceed the new shared budget. Preserve them
  // across loading and reject further rewards without rewriting their history.
  State oldBudget;
  oldBudget.version = 2;
  auto oldConfig = config;
  for (const auto &id : {"two", "three"}) {
    auto assignment = config.assignments[0]; assignment.id = id;
    oldConfig.assignments.push_back(assignment);
  }
  configure(oldBudget, oldConfig, today());
  auto &oldDay = oldBudget.days.begin()->second;
  oldDay.played = true;
  oldDay.points = 25;
  oldBudget.savings = 15;
  for (std::size_t i = 0; i < oldDay.assignments.size(); ++i) {
    auto &progress = oldDay.assignments[i];
    progress.completedRounds = 6;
    progress.bonusRounds = 5;
    progress.earnedPoints = i == 0 ? 15 : 5;
    for (int n = 0; n < 30; ++n) progress.usedCharts.insert("old" + std::to_string(n));
  }
  save(db, oldBudget);
  auto preserved = load(db);
  attempt.day = today(); attempt.chart = "new-after-upgrade";
  CHECK(record(preserved, attempt, 0) == 0);
  CHECK(preserved.savings == 15 && preserved.days.begin()->second.points == 25);
  CHECK(bonusPoints(preserved.days.begin()->second) == 15);

  // The entire shared budget is now valid on a single assignment, including
  // after upgrading a partially completed bonus round with an older quota.
  State oneAssignment;
  oneAssignment.version = 2;
  configure(oneAssignment, config, today());
  attempt.day = today(); attempt.chart = "goal";
  CHECK(record(oneAssignment, attempt, 0) == 10);
  auto &oldRound = oneAssignment.days.begin()->second.assignments[0];
  oldRound.requirement.chartCount = 4;
  oldRound.charts = 2;
  oldRound.usedCharts.insert("partial-one");
  oldRound.usedCharts.insert("partial-two");
  save(db, oneAssignment);
  oneAssignment = load(db);
  for (int n = 0; n < 10; ++n) {
    attempt.chart = "single-bonus" + std::to_string(n);
    CHECK(record(oneAssignment, attempt, 0) == 1);
  }
  save(db, oneAssignment);
  oneAssignment = load(db);
  CHECK(oneAssignment.days.begin()->second.assignments[0].bonusRounds == 10);
  CHECK(oneAssignment.days.begin()->second.assignments[0].requirement.chartCount == 1);
  attempt.chart = "eleventh";
  CHECK(record(oneAssignment, attempt, 0) == 0 && oneAssignment.savings == 10);

  // Eligibility and separate achievements survive reopening and rollback.
  State partial;
  auto promotion = config;
  promotion.dailyGoal = 100;
  promotion.assignments[0].eligibleLamp = 200;
  promotion.assignments[0].targetLamp = 400;
  promotion.assignments[0].scoreNumerator = 9;
  promotion.assignments[0].scoreDenominator = 10;
  configure(partial, promotion, today());
  attempt = {.day=today(), .chart="promotion", .levels={{"https://table","0",""}},
             .previousLamp=200, .lamp=300, .score=190, .maxScore=200,
             .createdAt="2026-10-26 12:00:00.000"};
  CHECK(record(partial, attempt, 0) == 0);
  save(db, partial);
  partial = load(db);
  CHECK(partial.days.begin()->second.assignments[0].chartProgress.at("promotion").score == 190);
  CHECK(partial.days.begin()->second.assignments[0].chartProgress.at("promotion").eligibleSince == attempt.createdAt);
  attempt.previousLamp = 300; attempt.lamp = 400; attempt.score = 100; attempt.levels.clear();
  CHECK(sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr) == SQLITE_OK);
  CHECK(record(partial, attempt, 0) == 20);
  save(db, partial);
  CHECK(sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr) == SQLITE_OK);
  partial = load(db);
  CHECK(partial.days.begin()->second.points == 0);
  CHECK(partial.days.begin()->second.assignments[0].chartProgress.at("promotion").lamp == 300);
  CHECK(record(partial, attempt, 0) == 20);
  save(db, partial);
  partial = load(db);
  CHECK(record(partial, attempt, 0) == 0);
  bad = partial; bad.days.begin()->second.assignments[0].chartProgress.clear();
  CHECK(rejects(bad));
  bad = partial; bad.days.begin()->second.assignments[0].chartProgress.at("promotion").score = 201;
  CHECK(rejects(bad));
  bad = partial; bad.days.begin()->second.assignments[0].chartProgress.at("promotion").lamp = 250;
  CHECK(rejects(bad));

  // Unsupported or damaged state must be reported, never silently replaced.
  CHECK(sqlite3_exec(db, "UPDATE streak_state SET body = '{\"version\":99}'", nullptr, nullptr, nullptr) == SQLITE_OK);
  bool rejected = false;
  try { (void)load(db); } catch (const std::exception &) { rejected = true; }
  CHECK(rejected);
  sqlite3_close(db);
  std::cout << "streak store tests passed\n";
}
