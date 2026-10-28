#include "kombo/KomboStore.h"
#include "sqlite3.h"
#include "../yoga/lib/nlohmann/json.hpp"
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while (false)
using namespace kombo;
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
  // Previously saved empty routines remain readable so they can be repaired.
  State legacyEmpty = state;
  legacyEmpty.configurations.begin()->second.assignments.clear();
  legacyEmpty.days.begin()->second.config.assignments.clear();
  legacyEmpty.days.begin()->second.assignments.clear();
  save(other, legacyEmpty);
  CHECK(load(other).days.at(today()).assignments.empty());
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
  CHECK(reopened.version == 5 && freezePrice(reopened) == 25 && reopened.savings == 80);
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
  earned.milestones.insert("kombo:7");
  save(db, earned);
  sqlite3_stmt *query = nullptr;
  CHECK(sqlite3_prepare_v2(db, "SELECT body FROM kombo_state", -1, &query, nullptr) == SQLITE_OK);
  CHECK(sqlite3_step(query) == SQLITE_ROW);
  auto legacy = nlohmann::json::parse(reinterpret_cast<const char *>(sqlite3_column_text(query, 0)));
  sqlite3_finalize(query);
  CHECK(legacy.contains("currentKombo") && legacy.contains("bestKombo"));
  const auto replaceBody = [&](const nlohmann::json &value) {
    const auto body = value.dump();
    sqlite3_stmt *update = nullptr;
    CHECK(sqlite3_prepare_v2(db, "UPDATE kombo_state SET body=?", -1, &update, nullptr) == SQLITE_OK);
    CHECK(sqlite3_bind_text(update, 1, body.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK);
    CHECK(sqlite3_step(update) == SQLITE_DONE);
    sqlite3_finalize(update);
  };
  auto versionThree = legacy;
  versionThree["version"] = 3;
  const auto removeScoreFilter = [](auto &assignment) {
    assignment.erase("eligibleScoreRank"); assignment.erase("eligibleScoreOrBelow");
  };
  for (auto &entry : versionThree["configurations"])
    for (auto &assignment : entry[1]["assignments"]) removeScoreFilter(assignment);
  for (auto &entry : versionThree["days"]) {
    for (auto &assignment : entry[1]["config"]["assignments"]) removeScoreFilter(assignment);
    for (auto &progress : entry[1]["assignments"]) removeScoreFilter(progress["requirement"]);
  }
  replaceBody(versionThree);
  auto scoreMigration = load(db);
  CHECK(scoreMigration.version == 5 && scoreMigration.savings == earned.savings);
  CHECK(scoreMigration.days.begin()->second.assignments[0].requirement.eligibleScoreRank == -2);
  scoreMigration.days.begin()->second.assignments[0].requirement.eligibleScoreRank = 21;
  scoreMigration.days.begin()->second.assignments[0].requirement.eligibleScoreOrBelow = true;
  save(db, scoreMigration);
  CHECK(load(db).days.begin()->second.assignments[0].requirement.eligibleScoreRank == 21);
  CHECK(load(db).days.begin()->second.assignments[0].requirement.eligibleScoreOrBelow);
  legacy["version"] = 2;
  for (auto &entry : legacy["days"])
    for (auto &progress : entry[1]["assignments"]) progress.erase("chartProgress");
  replaceBody(legacy);
  const auto versionTwo = load(db);
  CHECK(versionTwo.version == 5 && versionTwo.savings == earned.savings);
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
  CHECK(migrated.version == 5 && migrated.savings == 80 && migrated.freezes == 1);
  CHECK(migrated.configurations.size() == 2 && migrated.milestones.contains("kombo:7"));
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

  // Immediate edits retain partial progress even when the new quota is lower.
  State adjusted;
  auto moreCharts = config;
  moreCharts.dailyGoal = 100;
  moreCharts.assignments[0].chartCount = 3;
  configure(adjusted, moreCharts, today());
  attempt = {.day=today(), .chart="kept", .levels={{"https://table","0",""}}, .lamp=300};
  record(adjusted, attempt, 0);
  auto replacement = moreCharts.assignments[0];
  replacement.chartCount = 1;
  adjustAssignment(adjusted, replacement, today());
  save(db, adjusted);
  adjusted = load(db);
  CHECK(adjusted.days.at(today()).assignments[0].charts == 1);
  CHECK(record(adjusted, attempt, 0) == 0);
  attempt.chart = "new";
  CHECK(record(adjusted, attempt, 0) == config.assignments[0].points);
  save(db, adjusted);
  CHECK(load(db).days.at(today()).points == config.assignments[0].points);

  // Imported order (not lexical level names) supplies the one table promotion.
  const auto chartPath = std::filesystem::temp_directory_path() / ("kombo-levels-" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".sqlite3");
  sqlite3 *chartDb = nullptr;
  CHECK(sqlite3_open(chartPath.string().c_str(), &chartDb) == SQLITE_OK);
  CHECK(sqlite3_exec(chartDb,
      "CREATE TABLE difficulty_tables(id INTEGER,source_url TEXT,symbol TEXT);"
      "CREATE TABLE chart_meta(sha256 TEXT,md5 TEXT);"
      "CREATE TABLE difficulty_table_entries(table_id INTEGER,level TEXT,sha256 TEXT,md5 TEXT,sort_order INTEGER);"
      "INSERT INTO difficulty_tables VALUES(1,'https://table','sl');"
      "INSERT INTO difficulty_table_entries VALUES"
      "(1,'intro','goal','',0),(1,'Z','extra','',1),(1,'A','top','',2),"
      "(1,'intro','easy1','',0),(1,'intro','easy2','',0);",
      nullptr, nullptr, nullptr) == SQLITE_OK);
  sqlite3_close(chartDb);
  State imported;
  auto orderedConfig = config;
  orderedConfig.dailyGoal = 1;
  orderedConfig.assignments[0].levels[0].level = "intro";
  orderedConfig.assignments[0].targetLamp = 600;
  orderedConfig.assignments[0].scoreNumerator = orderedConfig.assignments[0].scoreDenominator = 27;
  const auto importedDay = localDay(1700000000);
  configure(imported, orderedConfig, importedDay);
  save(db, imported);
  auto savedPlay = Play{.chart="goal", .lamp=600, .score=27, .maxScore=27};
  recordSavedPlay(db, chartPath, savedPlay, "", "2023-11-14 22:13:20", 0);
  imported = load(db);
  CHECK(imported.days.at(importedDay).assignments[0].requirement.levels[0].level == "Z");
  CHECK(imported.days.at(importedDay).assignments[0].requirement.levels[0].label == "slZ");
  savedPlay.chart = "extra";
  recordSavedPlay(db, chartPath, savedPlay, "", "2023-11-14 22:14:20", 0);
  imported = load(db);
  CHECK(imported.days.at(importedDay).points == 2);
  CHECK(imported.days.at(importedDay).assignments[0].requirement.levels[0].level == "Z");
  resetExtraChallenge(imported, "one", importedDay);
  save(db, imported);
  savedPlay.chart = "easy1";
  recordSavedPlay(db, chartPath, savedPlay, "", "2023-11-14 22:15:20", 0);
  imported = load(db);
  CHECK(imported.days.at(importedDay).assignments[0].charts == 1 && imported.days.at(importedDay).points == 2);
  resetExtraChallenge(imported, "one", importedDay); // Duplicate confirmation is harmless.
  CHECK(imported.days.at(importedDay).assignments[0].charts == 1);
  savedPlay.chart = "easy2";
  recordSavedPlay(db, chartPath, savedPlay, "", "2023-11-14 22:16:20", 0);
  imported = load(db);
  CHECK(imported.days.at(importedDay).points == 3);
  CHECK(imported.days.at(importedDay).assignments[0].requirement.chartCount == 2);
  CHECK(imported.days.at(importedDay).assignments[0].requirement.levels[0].level == "intro");
  advance(imported, importedDay + 1);
  CHECK(imported.days.at(importedDay + 1).assignments[0].requirement.chartCount == 1);
  CHECK(!imported.days.at(importedDay + 1).assignments[0].bonusDifficultyApplied);
  // A legacy range must not be mistaken for each partial chart's membership.
  CHECK(sqlite3_open(chartPath.string().c_str(), &chartDb) == SQLITE_OK);
  CHECK(sqlite3_exec(chartDb,
      "INSERT INTO difficulty_table_entries VALUES"
      "(1,'intro','','partial-zero-md5',0),(1,'Z','partial-one','',1);"
      "INSERT INTO chart_meta VALUES('partial-zero','partial-zero-md5');",
      nullptr, nullptr, nullptr) == SQLITE_OK);
  sqlite3_close(chartDb);
  State legacyRange;
  auto rangeConfig = orderedConfig;
  rangeConfig.assignments[0].levels = {{"https://table","intro",""},{"https://table","Z",""}};
  rangeConfig.assignments[0].targetLamp = 400;
  rangeConfig.assignments[0].scoreNumerator = 0;
  rangeConfig.assignments[0].eligibleLamp = -1;
  configure(legacyRange, rangeConfig, importedDay);
  Play partialPlay{.day=importedDay, .chart="partial-zero", .levels={{"https://table","intro",""}}, .lamp=300};
  CHECK(record(legacyRange, partialPlay, 0) == 0);
  partialPlay.chart = "partial-one"; partialPlay.levels[0].level = "Z";
  CHECK(record(legacyRange, partialPlay, 0) == 0);
  save(db, legacyRange);
  CHECK(sqlite3_prepare_v2(db, "SELECT body FROM kombo_state", -1, &query, nullptr) == SQLITE_OK);
  CHECK(sqlite3_step(query) == SQLITE_ROW);
  auto versionFour = nlohmann::json::parse(reinterpret_cast<const char *>(sqlite3_column_text(query, 0)));
  sqlite3_finalize(query);
  versionFour["version"] = 4;
  for (auto &entry : versionFour["days"]) for (auto &p : entry.at(1)["assignments"]) {
    p.erase("originalRequirement"); p.erase("bonusDifficultyApplied"); p.erase("bonusChartCount");
    for (auto &achievement : p["chartProgress"]) achievement.erase("levels");
  }
  replaceBody(versionFour);
  CHECK(load(db).days.at(importedDay).assignments[0].chartProgress.at("partial-zero").levels.empty());
  savedPlay = {.chart="goal", .lamp=400};
  recordSavedPlay(db, chartPath, savedPlay, "", "2023-11-14 22:13:20", 1);
  legacyRange = load(db);
  const auto &migratedRange = legacyRange.days.at(importedDay).assignments[0];
  CHECK(!migratedRange.chartProgress.contains("partial-zero"));
  CHECK(migratedRange.chartProgress.contains("partial-one"));
  CHECK(migratedRange.requirement.levels[0].level == "Z");
  savedPlay.chart = "partial-zero"; savedPlay.previousLamp = 300;
  recordSavedPlay(db, chartPath, savedPlay, "partial-zero-md5", "2023-11-14 22:14:20", 1);
  CHECK(load(db).days.at(importedDay).points == 1);
  savedPlay.chart = "partial-one";
  recordSavedPlay(db, chartPath, savedPlay, "", "2023-11-14 22:15:20", 1);
  CHECK(load(db).days.at(importedDay).points == 2);
  std::filesystem::remove(chartPath);

  // Unsupported or damaged state must be reported, never silently replaced.
  CHECK(sqlite3_exec(db, "UPDATE kombo_state SET body = '{\"version\":99}'", nullptr, nullptr, nullptr) == SQLITE_OK);
  bool rejected = false;
  try { (void)load(db); } catch (const std::exception &) { rejected = true; }
  CHECK(rejected);
  sqlite3_close(db);
  std::cout << "kombo store tests passed\n";
}
