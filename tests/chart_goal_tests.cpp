#include "goals/ChartGoal.h"
#include "goals/ChartGoalStore.h"
#include "repositories/SqliteRAII.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while (false)
using namespace chart_goals;
template<class F> bool rejects(F f) { try { f(); return false; } catch (const std::exception &) { return true; } }
int main() {
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
  Goal goal;
  goal.chart.SHA256 = std::string(64, 'A'); goal.chart.MD5 = std::string(32, 'B');
  goal.chart.Title = "Goal chart"; goal.chart.SubTitle = "Another";
  goal.chart.TotalLongNotes = 1; goal.longNoteMode = 1; goal.target = aaa;
  save(db.get(), goal);
  CHECK(load(db.get(), std::string(64, 'a'), 1)->chart.Title == "Goal chart");
  goal.chart.Title = "Updated"; save(db.get(), goal);
  CHECK(loadPage(db.get(), 0).goals.size() == 1);
  goal.longNoteMode = 2; save(db.get(), goal);
  CHECK(loadPage(db.get(), 0).goals.size() == 2);
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
