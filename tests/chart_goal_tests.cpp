#include "goals/ChartGoal.h"
#include "goals/ChartGoalStore.h"
#include "goals/GoalRecommendationQuery.h"
#include "goals/GoalResultPresentation.h"
#include "repositories/SqliteRAII.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while (false)
using namespace chart_goals;
template<class F> bool rejects(F f) { try { f(); return false; } catch (const std::exception &) { return true; } }
int main() {
  {
    sqlite3 *recommendations = nullptr;
    CHECK(sqlite3_open(":memory:", &recommendations) == SQLITE_OK);
    CHECK(sqlite3_exec(recommendations,
        "ATTACH ':memory:' AS score_db;"
        "CREATE TABLE score_db.score_sha256_best_score_cache(chart_sha256 TEXT,ln_mode INTEGER,score INTEGER,max_score INTEGER);"
        "CREATE TABLE chart_meta(sha256 TEXT,path TEXT,ln_mode INTEGER,total_long_notes INTEGER,total_backspin_notes INTEGER);"
        "CREATE INDEX chart_hash ON chart_meta(sha256);"
        "INSERT INTO chart_meta VALUES('near','a',0,0,0),('near','duplicate',0,0,0),('met','b',0,0,0),('far','c',0,0,0);"
        "INSERT INTO score_db.score_sha256_best_score_cache VALUES('near',0,1738,2000),('met',0,1778,2000),('far',0,1737,2000),('missing',0,1777,2000);",
        nullptr, nullptr, nullptr) == SQLITE_OK);
    SqliteStatementHandle query;
    CHECK(prepareSqliteStatement(recommendations, recommendationCandidatesSql() + " SELECT sha256, target FROM candidates", query) == SQLITE_OK);
    CHECK(sqlite3_step(query) == SQLITE_ROW);
    CHECK(sqliteColumnString(query, 0) == "near" && sqlite3_column_int(query, 1) == 24);
    CHECK(sqlite3_step(query) == SQLITE_DONE);
    query.reset();
    CHECK(prepareSqliteStatement(recommendations, "EXPLAIN QUERY PLAN " + recommendationCandidatesSql() +
        " SELECT sha256 FROM candidates", query) == SQLITE_OK);
    bool indexedLibraryLookup = false;
    while (sqlite3_step(query) == SQLITE_ROW) {
      const auto plan = sqliteColumnString(query, 3);
      if (plan.find("INDEX chart_hash") != std::string::npos) indexedLibraryLookup = true;
    }
    CHECK(indexedLibraryLookup);
    query.reset();
    CHECK(sqlite3_exec(recommendations,
        "DELETE FROM score_db.score_sha256_best_score_cache; DELETE FROM chart_meta;",
        nullptr, nullptr, nullptr) == SQLITE_OK);
    for (int target : {6,9,12,15,18,21,24,26,27}) {
      const auto setup = "INSERT INTO chart_meta VALUES('rank" + std::to_string(target) + "','chart',0,0,0);"
          "INSERT INTO score_db.score_sha256_best_score_cache VALUES('rank" + std::to_string(target) +
          "',0," + std::to_string(target*100-54) + ",2700);";
      CHECK(sqlite3_exec(recommendations, setup.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
    }
    CHECK(prepareSqliteStatement(recommendations, recommendationCandidatesSql() +
        " SELECT COUNT(*), MIN(remaining), MAX(remaining) FROM candidates", query) == SQLITE_OK);
    CHECK(sqlite3_step(query) == SQLITE_ROW && sqlite3_column_int(query,0) == 9);
    CHECK(sqlite3_column_int(query,1) == 54 && sqlite3_column_int(query,2) == 54);
    query.reset();
    CHECK(sqlite3_exec(recommendations,
        "DELETE FROM score_db.score_sha256_best_score_cache; DELETE FROM chart_meta;"
        "INSERT INTO chart_meta VALUES('fixed','fixed',2,10,0),('selectable','selectable',0,10,0);"
        "INSERT INTO score_db.score_sha256_best_score_cache VALUES"
        "('fixed',1,1777,2000),('fixed',2,1777,2000),('fixed',3,1777,2000),"
        "('selectable',0,1777,2000),('selectable',1,1777,2000),('selectable',3,1777,2000);",
        nullptr, nullptr, nullptr) == SQLITE_OK);
    CHECK(prepareSqliteStatement(recommendations, recommendationCandidatesSql() +
        " SELECT COUNT(*) FROM candidates", query) == SQLITE_OK);
    CHECK(sqlite3_step(query) == SQLITE_ROW && sqlite3_column_int(query,0) == 3);
    query.reset();
    sqlite3_close(recommendations);
  }
  GoalResultPresentation celebration;
  CHECK(!celebration.arm(false) && !celebration.active());
  CHECK(celebration.arm(true) && celebration.active());
  CHECK(!celebration.start(100, true) && !celebration.visible());
  CHECK(celebration.start(200, false) && celebration.visible());
  CHECK(!celebration.start(250, false) && !celebration.arm(true));
  CHECK(!celebration.finish(4699));
  CHECK(celebration.finish(4700) && !celebration.active());
  CHECK(!celebration.arm(true)); // Rebuild/resume must not replay the celebration.
  CHECK(parseRate("99.99") == 9999);
  CHECK(parseRate("100") == 10000);
  CHECK(parseRate("0.01") == 1);
  CHECK(parseRate("88.8") == 8880);
  for (const auto *value : {"", "0", "100.01", "1.001", "nan", "1e2", "-2", "5%"})
    CHECK(!parseRate(value));
  Target aaa{.scoreNumerator=24};
  ScoreBestSnapshot score{.score=1777, .maxScore=2000};
  auto p = evaluate(aaa, score, {});
  CHECK(!p.achieved && p.remainingEx == 1);
  score.score = 1778;
  CHECK(evaluate(aaa, score, {}).achieved);
  score.score = 1999;
  CHECK(evaluate({.scoreNumerator=9999, .scoreDenominator=10000}, score, {}).remainingEx == 1);
  score.maxScore = 0;
  CHECK(!evaluate(aaa, score, {}).achieved && !evaluate(aaa, score, {}).remainingEx);
  score.maxScore = -1;
  CHECK(!evaluate(aaa, score, {}).achieved);
  CHECK(!evaluate(aaa, {}, {}).remainingEx);
  score = {.score=1800, .maxScore=2000, .clearType=200};
  ScoreBestSnapshot lamp{.score=1400, .maxScore=2000, .clearType=400};
  CHECK(evaluate({.lamp=400, .scoreNumerator=24}, score, lamp).achieved);
  CHECK(!evaluate({.lamp=400, .scoreNumerator=24}, score, score).achieved);
  CHECK(evaluate({.lamp=400}, {}, lamp).achieved);
  CHECK(rejects([] { evaluate({}, {}, {}); }));
  CHECK(rejects([] { evaluate({.lamp=0}, {}, {}); }));
  CHECK(rejects([] { evaluate({.scoreNumerator=28}, {}, {}); }));
  CHECK(rejects([] { evaluate({.scoreNumerator=1, .scoreDenominator=0}, {}, {}); }));

  SqliteConnectionHandle db;
  sqlite3 *raw = nullptr;
  CHECK(sqlite3_open(":memory:", &raw) == SQLITE_OK); db.reset(raw);
  CHECK(loadPage(db.get(), 0).goals.empty());
  CHECK(loadIndex(db.get()).modes.empty());
  Goal goal;
  goal.chart.SHA256 = std::string(64, 'A'); goal.chart.MD5 = std::string(32, 'B');
  goal.setAtUnixMillis = 1730000000000;
  goal.achievedAtUnixMillis = 1730000001234;
  goal.chart.Title = "Goal chart"; goal.chart.SubTitle = "Another";
  goal.chart.TotalLongNotes = 1; goal.longNoteMode = 1; goal.target = aaa;
  save(db.get(), goal);
  CHECK(load(db.get(), std::string(64, 'a'), 1)->chart.Title == "Goal chart");
  CHECK(load(db.get(), goal.chart.SHA256, 1)->setAtUnixMillis == 1730000000000);
  CHECK(load(db.get(), goal.chart.SHA256, 1)->achievedAtUnixMillis == 1730000001234);
  CHECK(rejects([&] { auto invalid = goal; invalid.achievedAtUnixMillis = 1; save(db.get(), invalid); }));
  goal.chart.Title = "Updated"; save(db.get(), goal);
  CHECK(loadPage(db.get(), 0).goals.size() == 1);
  goal.longNoteMode = 2; save(db.get(), goal);
  CHECK(loadPage(db.get(), 0).goals.size() == 2);
  auto index = loadIndex(db.get());
  CHECK(index.hashes() == std::vector<std::string>{std::string(64, 'a')});
  CHECK(index.modeFor(goal.chart.SHA256, 2) == 2);
  CHECK(index.modeFor(goal.chart.SHA256, 3) == 1); // A highlighted flag always opens a saved goal.
  index.achievedModes[std::string(64, 'a')] = 1u << 1;
  CHECK(!index.allAchieved(goal.chart.SHA256));
  CHECK(index.activeHashes().size() == 1); // A pending goal in another mode keeps the chart.
  index.achievedModes[std::string(64, 'a')] |= 1u << 2;
  CHECK(index.allAchieved(goal.chart.SHA256) && index.activeHashes().empty());
  CHECK(index.hashes().size() == 1 && index.modeFor(goal.chart.SHA256, 2) == 2);

  auto other = goal.chart; other.SHA256 = std::string(64, 'b');
  CHECK(!index.modeFor(other.SHA256, 1));
  other.SHA256 = ""; CHECK(!index.modeFor(other.SHA256, 1));
  CHECK(rejects([&] { auto invalid = goal; invalid.chart.SHA256 = "bad"; save(db.get(), invalid); }));
  std::string error;
  { SqliteTransactionHandle tx(db.get(), "BEGIN", error);
    goal.chart.Title = "Rolled back"; save(db.get(), goal); }
  CHECK(load(db.get(), goal.chart.SHA256, 2)->chart.Title == "Updated");
  for (int i = 0; i < 19; ++i) {
    goal.chart.SHA256 = std::string(62, 'c') + char('a' + i / 16) + "0123456789abcdef"[i % 16];
    save(db.get(), goal);
  }
  CHECK(loadPage(db.get(), 0).goals.size() == 20 && loadPage(db.get(), 0).hasMore);
  CHECK(loadPage(db.get(), 20).goals.size() == 1 && !loadPage(db.get(), 20).hasMore);
  remove(db.get(), goal.chart.SHA256, 2);
  CHECK(!load(db.get(), goal.chart.SHA256, 2));
  CHECK(!loadIndex(db.get()).modeFor(goal.chart.SHA256, 2));
  CHECK(!loadPage(db.get(), 0).hasMore);
  CHECK(sqlite3_exec(db.get(), "UPDATE chart_goals SET body='{}'", nullptr, nullptr, nullptr) == SQLITE_OK);
  CHECK(rejects([&] { loadPage(db.get(), 0); }));
  goal.chart.SHA256 = std::string(64, 'a'); goal.longNoteMode = 1;
  CHECK(rejects([&] { save(db.get(), goal); }));
  CHECK(sqlite3_exec(db.get(), "UPDATE chart_goals SET body='{\"version\":999}'", nullptr, nullptr, nullptr) == SQLITE_OK);
  CHECK(rejects([&] { save(db.get(), goal); }));
  CHECK(rejects([&] { remove(db.get(), goal.chart.SHA256, 1); }));

  auto path = std::filesystem::temp_directory_path() / ("aso-goals-" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");
  CHECK(sqlite3_open(path.string().c_str(), &raw) == SQLITE_OK); db.reset(raw);
  CHECK(loadPage(db.get(), 0).goals.empty());
  save(db.get(), goal); db.reset();
  CHECK(sqlite3_open(path.string().c_str(), &raw) == SQLITE_OK); db.reset(raw);
  CHECK(load(db.get(), goal.chart.SHA256, 1)->chart.SubTitle == "Another");
  CHECK(sqlite3_exec(db.get(), "PRAGMA query_only=ON", nullptr, nullptr, nullptr) == SQLITE_OK);
  goal.chart.Title = "Must not persist";
  CHECK(rejects([&] { save(db.get(), goal); }));
  CHECK(load(db.get(), goal.chart.SHA256, 1)->chart.Title != goal.chart.Title);
  db.reset(); std::filesystem::remove(path);
  std::cout << "chart goal tests passed\n";
}
