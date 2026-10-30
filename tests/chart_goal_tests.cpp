#include "goals/ChartGoal.h"
#include "goals/GoalChallenge.h"
#include "scene/play/Pacemaker.h"
#include "goals/ChartGoalStore.h"
#include "goals/GoalRecommendationQuery.h"
#include "goals/GoalRecommendationIndex.h"
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
    for (auto shift : {GaugeAutoShiftMode::None, GaugeAutoShiftMode::Continue,
                      GaugeAutoShiftMode::SelectToUnder, GaugeAutoShiftMode::SurvivalToGroove}) {
      CHECK(challengeGauge(GaugeType::Easy, shift, kClearTypeHardClearRank) == GaugeType::Hard);
      CHECK(challengeGauge(GaugeType::ExHard, shift, kClearTypeHardClearRank) == GaugeType::ExHard);
    }
    CHECK(challengeGauge(GaugeType::Easy, GaugeAutoShiftMode::BestClear, kClearTypeExHardClearRank) == GaugeType::Easy);
    CHECK(challengeGauge(GaugeType::Hard, GaugeAutoShiftMode::None, kClearTypeFullComboRank) == GaugeType::Hazard);
    CHECK(challengeGauge(GaugeType::AssistedEasy, GaugeAutoShiftMode::None, 150) == GaugeType::AssistedEasy);
    CHECK(challengeGauge(GaugeType::Normal, GaugeAutoShiftMode::None, -1) == GaugeType::Normal);
    std::vector<GoalProgress> goals{
      {.goal={.longNoteMode=1, .target={.scoreNumerator=24}, .id=1}},
      {.goal={.longNoteMode=1, .target={.scoreNumerator=8777, .scoreDenominator=10000}, .id=2}},
      {.goal={.longNoteMode=2, .target={.scoreNumerator=18}, .id=3}},
      {.goal={.longNoteMode=1, .target={.lamp=400}, .id=4}},
    };
    CHECK(scoreTargetForGoals(goals, 1, 1)->scoreNumerator == 24);
    CHECK(scoreTargetForGoals(goals, 1, 0)->scoreNumerator == 8777);
    CHECK(!scoreTargetForGoals(goals, 1, 4)); // Explicit lamp-only goal falls back to BEST.
    CHECK(scoreTargetForGoals(goals, 1, 999)->scoreNumerator == 8777);
    CHECK(scoreTargetForGoals(goals, 2, 1)->scoreNumerator == 18);
    goals[1].progress.scoreMet=true;
    CHECK(scoreTargetForGoals(goals, 1, 0)->scoreNumerator == 24);
    goals[0].progress.scoreMet=true;
    CHECK(!scoreTargetForGoals(goals, 1, 0));
    CHECK(scoreTargetForGoals(goals, 1, 1)->scoreNumerator == 24); // Explicit achieved goals remain usable.
    bms_parser::ChartMeta meta; meta.TotalNotes=1000;
    const auto target=pacemaker::targetFromGoal(meta, goals[1].goal.target);
    CHECK(target.enabled && target.label=="GOAL" && target.finalScore==1756);
    CHECK(pacemaker::targetScoreAtPlayedNotes(target,500)==878);
    CHECK(pacemaker::normalizeTargetId("goal")=="GOAL");
    ScoreBestSnapshot best; best.score=1234; best.maxScore=2000;
    CHECK(pacemaker::targetFromSelection(meta,"GOAL",best).label=="BEST");
    CHECK(pacemaker::targetFromSelection(meta,"GOAL",best,nullptr,&goals[0].goal.target).finalScore==1778);
  }
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
    CHECK(sqlite3_exec(recommendations,
        "DELETE FROM score_db.score_sha256_best_score_cache; DELETE FROM chart_meta;"
        "INSERT INTO chart_meta VALUES('closest','a',0,0,0),('smaller-percent','b',0,0,0),('farther','c',0,0,0);"
        "INSERT INTO score_db.score_sha256_best_score_cache VALUES"
        "('closest',0,1777,2000),('smaller-percent',0,8887,10000),('farther',0,1775,2000);",
        nullptr, nullptr, nullptr) == SQLITE_OK);
    CHECK(prepareSqliteStatement(recommendations, recommendationPageSql(), query) == SQLITE_OK);
    CHECK(sqlite3_bind_int(query, 1, 24) == SQLITE_OK && sqlite3_bind_int(query, 2, 0) == SQLITE_OK);
    for (const auto *hash : {"closest", "smaller-percent", "farther"}) {
      CHECK(sqlite3_step(query) == SQLITE_ROW && sqliteColumnString(query, 0) == hash);
    }
    CHECK(sqlite3_step(query) == SQLITE_DONE);
    query.reset();
    CHECK(sqlite3_exec(recommendations,
        "CREATE TABLE score_db.goal_combo_best(chart_sha256 TEXT,ln_mode INTEGER,combo_break INTEGER);"
        "CREATE TABLE score_db.goal_gauge_best(chart_sha256 TEXT,ln_mode INTEGER,target INTEGER,gauge_value REAL,border REAL,death_notes INTEGER,total_notes INTEGER,gap REAL);"
        "CREATE INDEX score_db.goal_gauge_proximity ON goal_gauge_best(target,gap,death_notes,chart_sha256,ln_mode);"
        "CREATE TABLE score_db.score_sha256_clear_rank_cache(chart_sha256 TEXT,ln_mode INTEGER,rank INTEGER,PRIMARY KEY(chart_sha256,ln_mode));"
        "INSERT INTO chart_meta VALUES('closest','duplicate',0,0,0);"
        "INSERT INTO score_db.goal_gauge_best VALUES('closest',0,400,0,0,90,100,0.1),"
        "('smaller-percent',0,400,10,0,-1,100,0),('farther',0,400,0,0,89,100,0.11),"
        "('missing',0,400,0,0,99,100,0.01),('closest',1,400,0,0,99,100,0.01),"
        "('closest',0,300,75,80,0,100,5),('farther',0,300,74.9,80,0,100,5.1);",
        nullptr,nullptr,nullptr)==SQLITE_OK);
    CHECK(prepareSqliteStatement(recommendations, gaugeRecommendationPageSql(), query)==SQLITE_OK);
    CHECK(sqlite3_bind_int(query,1,400)==SQLITE_OK && sqlite3_bind_int(query,2,0)==SQLITE_OK);
    CHECK(sqlite3_step(query)==SQLITE_ROW && sqliteColumnString(query,0)=="smaller-percent");
    CHECK(sqlite3_step(query)==SQLITE_ROW && sqliteColumnString(query,0)=="closest");
    CHECK(sqlite3_step(query)==SQLITE_DONE);
    query.reset();
    // A chart below the first global page must still appear in its own recommendations.
    for (int i=0; i<21; ++i) {
      const auto hash = "other-" + std::to_string(i);
      const auto setup = "INSERT INTO chart_meta VALUES('" + hash + "','other',0,0,0);"
          "INSERT INTO score_db.score_sha256_best_score_cache VALUES('" + hash + "',0,1777,2000);"
          "INSERT INTO score_db.goal_gauge_best VALUES('" + hash + "',0,400,10,0,-1,100,0);";
      CHECK(sqlite3_exec(recommendations, setup.c_str(), nullptr,nullptr,nullptr)==SQLITE_OK);
    }
    for (const bool gauge : {false, true}) {
      CHECK(prepareSqliteStatement(recommendations, gauge ? gaugeRecommendationPageSql(0,false,false,true)
          : recommendationPageSql(0,false,false,true), query)==SQLITE_OK);
      CHECK(sqlite3_bind_int(query,1,gauge ? 400 : 24)==SQLITE_OK);
      CHECK(sqlite3_bind_int(query,2,0)==SQLITE_OK);
      CHECK(sqlite3_bind_text(query,3,gauge ? "closest" : "farther",-1,SQLITE_STATIC)==SQLITE_OK);
      CHECK(sqlite3_step(query)==SQLITE_ROW);
      CHECK(sqliteColumnString(query,0)==(gauge ? "closest" : "farther"));
      CHECK(sqlite3_step(query)==SQLITE_DONE);
      query.reset();
    }
    CHECK(sqlite3_exec(recommendations,
        "DELETE FROM chart_meta WHERE path='other';"
        "DELETE FROM score_db.score_sha256_best_score_cache WHERE chart_sha256 LIKE 'other-%';"
        "DELETE FROM score_db.goal_gauge_best WHERE chart_sha256 LIKE 'other-%';",nullptr,nullptr,nullptr)==SQLITE_OK);
    CHECK(sqlite3_exec(recommendations,
        "INSERT INTO chart_meta VALUES('lamp-only','lamp',0,0,0);"
        "INSERT INTO score_db.goal_combo_best VALUES('lamp-only',0,1);", nullptr,nullptr,nullptr)==SQLITE_OK);
    CHECK(prepareSqliteStatement(recommendations, recommendationChartHashesSql(), query)==SQLITE_OK);
    std::unordered_set<std::string> recommendedHashes;
    while (sqlite3_step(query)==SQLITE_ROW) CHECK(recommendedHashes.insert(sqliteColumnString(query,0)).second);
    CHECK((recommendedHashes==std::unordered_set<std::string>{"closest","smaller-percent","farther","lamp-only"}));
    query.reset();
    CHECK(sqlite3_exec(recommendations,
        "DELETE FROM chart_meta WHERE sha256='lamp-only'; DELETE FROM score_db.goal_combo_best WHERE chart_sha256='lamp-only';",
        nullptr,nullptr,nullptr)==SQLITE_OK);
    CHECK(sqlite3_exec(recommendations,"INSERT INTO score_db.score_sha256_clear_rank_cache VALUES('closest',0,400)",nullptr,nullptr,nullptr)==SQLITE_OK);
    CHECK(prepareSqliteStatement(recommendations,gaugeRecommendationCandidatesSql()+" SELECT COUNT(*) FROM candidates",query)==SQLITE_OK);
    CHECK(sqlite3_step(query)==SQLITE_ROW && sqlite3_column_int(query,0)==1); // Reached lamps, including easier lamps, disappear.
    query.reset();
    // A selectable chart appears once per target, using its closest mode.
    CHECK(sqlite3_exec(recommendations,
        "UPDATE chart_meta SET total_long_notes=10 WHERE sha256='closest';"
        "INSERT INTO score_db.score_sha256_best_score_cache VALUES('closest',1,1777,2000),('closest',2,1776,2000),('closest',3,1777,2000);"
        "INSERT INTO score_db.goal_gauge_best VALUES('closest',2,400,0,0,95,100,0.05),('closest',3,400,0,0,99,100,0.01);",
        nullptr,nullptr,nullptr)==SQLITE_OK);
    CHECK(prepareSqliteStatement(recommendations,recommendationPageSql(),query)==SQLITE_OK);
    CHECK(sqlite3_bind_int(query,1,24)==SQLITE_OK && sqlite3_bind_int(query,2,0)==SQLITE_OK);
    int matchingChart = 0;
    while (sqlite3_step(query)==SQLITE_ROW) if (sqliteColumnString(query,0)=="closest") {
      ++matchingChart;
      CHECK(sqlite3_column_int(query,1)==1);
    }
    CHECK(matchingChart==1);
    query.reset();
    CHECK(prepareSqliteStatement(recommendations,gaugeRecommendationPageSql(),query)==SQLITE_OK);
    CHECK(sqlite3_bind_int(query,1,400)==SQLITE_OK && sqlite3_bind_int(query,2,0)==SQLITE_OK);
    matchingChart = 0;
    while (sqlite3_step(query)==SQLITE_ROW) if (sqliteColumnString(query,0)=="closest") {
      ++matchingChart;
      CHECK(sqlite3_column_int(query,1)==1);
    }
    CHECK(matchingChart==1);
    query.reset();
    // Suppress only targets already covered in the same effective LN mode.
    CHECK(sqlite3_exec(recommendations,
        "CREATE TABLE score_db.chart_goals(sha256 TEXT,ln_mode INTEGER,body TEXT,PRIMARY KEY(sha256,ln_mode));"
        "INSERT INTO score_db.chart_goals VALUES('closest',1,'{\"lamp\":400,\"numerator\":24,\"denominator\":27}');",
        nullptr,nullptr,nullptr)==SQLITE_OK);
    CHECK(prepareSqliteStatement(recommendations,recommendationPageSql(0,false,true),query)==SQLITE_OK);
    CHECK(sqlite3_bind_int(query,1,24)==SQLITE_OK && sqlite3_bind_int(query,2,0)==SQLITE_OK);
    int keptScoreMode = 0;
    while (sqlite3_step(query)==SQLITE_ROW) if (sqliteColumnString(query,0)=="closest") {
      ++keptScoreMode; CHECK(sqlite3_column_int(query,1)==3);
    }
    CHECK(keptScoreMode == 1);
    query.reset();
    CHECK(prepareSqliteStatement(recommendations,gaugeRecommendationPageSql(0,false,true),query)==SQLITE_OK);
    CHECK(sqlite3_bind_int(query,1,400)==SQLITE_OK && sqlite3_bind_int(query,2,0)==SQLITE_OK);
    int keptLampMode = 0;
    while (sqlite3_step(query)==SQLITE_ROW) if (sqliteColumnString(query,0)=="closest") {
      ++keptLampMode; CHECK(sqlite3_column_int(query,1)==3);
    }
    CHECK(keptLampMode == 1);
    query.reset();
    CHECK(sqlite3_exec(recommendations,"DELETE FROM score_db.chart_goals",nullptr,nullptr,nullptr)==SQLITE_OK);
    // Full combo proximity follows the fewest breaks, not where Hazard died.
    CHECK(sqlite3_exec(recommendations,
        "DELETE FROM chart_meta; DELETE FROM score_db.goal_gauge_best; DELETE FROM score_db.score_sha256_clear_rank_cache;"
        "INSERT INTO chart_meta VALUES('one','a',0,0,0),('five','b',0,0,0),('six','c',0,0,0),"
        "('hazard-only','d',0,0,0),('earned','e',0,0,0),('modes','f',0,10,0),('modes','duplicate',0,10,0);"
        "INSERT INTO score_db.goal_combo_best VALUES('one',0,1),('five',0,5),('six',0,6),('earned',0,2),"
        "('missing',0,1),('modes',1,3),('modes',2,2),('modes',3,4),('modes',0,1);"
        "INSERT INTO score_db.goal_gauge_best VALUES('hazard-only',0,600,0,0,99,100,0.01);"
        "INSERT INTO score_db.score_sha256_clear_rank_cache VALUES('earned',0,600);",
        nullptr,nullptr,nullptr)==SQLITE_OK);
    CHECK(prepareSqliteStatement(recommendations,gaugeRecommendationPageSql(),query)==SQLITE_OK);
    CHECK(sqlite3_bind_int(query,1,600)==SQLITE_OK && sqlite3_bind_int(query,2,0)==SQLITE_OK);
    for (const auto *hash : {"one", "modes", "five"}) {
      CHECK(sqlite3_step(query)==SQLITE_ROW && sqliteColumnString(query,0)==hash);
      if (sqliteColumnString(query,0)=="modes") CHECK(sqlite3_column_int(query,1)==2);
    }
    CHECK(sqlite3_step(query)==SQLITE_DONE);
    query.reset();
    CHECK(sqlite3_exec(recommendations,
        "DELETE FROM chart_meta; DELETE FROM score_db.goal_gauge_best; DELETE FROM score_db.goal_combo_best;"
        "DELETE FROM score_db.score_sha256_clear_rank_cache;"
        "INSERT INTO chart_meta VALUES('light','a',0,0,0),('assist','b',0,0,0);"
        "INSERT INTO score_db.goal_gauge_best VALUES('light',0,100,75,80,0,100,5),('assist',0,50,75,80,0,100,5);"
        "INSERT INTO score_db.score_sha256_clear_rank_cache VALUES('light',0,150),('assist',0,100);",
        nullptr,nullptr,nullptr)==SQLITE_OK);
    CHECK(prepareSqliteStatement(recommendations,gaugeRecommendationCandidatesSql()+
        " SELECT chart_sha256,target FROM candidates",query)==SQLITE_OK);
    CHECK(sqlite3_step(query)==SQLITE_ROW && sqliteColumnString(query,0)=="light" && sqlite3_column_int(query,1)==100);
    CHECK(sqlite3_step(query)==SQLITE_DONE);
    query.reset();
    sqlite3_close(recommendations);
  }
  RecommendationIndex recommendations;
  int recommendationLoads = 0;
  const std::string recommendedHash(64, 'a');
  auto loadRecommendations = [&] {
    ++recommendationLoads;
    return std::unordered_set<std::string>{recommendedHash};
  };
  for (int selection=0; selection<100; ++selection) {
    recommendations.refresh("profile", 1, 1, loadRecommendations);
    CHECK(recommendations.contains(recommendedHash));
  }
  CHECK(recommendationLoads==1 && recommendations.contains(std::string(64,'A')));
  CHECK(!recommendations.contains("") && !recommendations.contains(std::string(64,'z')));
  recommendations.refresh("profile", 2, 1, loadRecommendations);
  recommendations.refresh("profile", 2, 2, loadRecommendations);
  CHECK(recommendationLoads==3);
  recommendations.refresh("other-profile", 2, 2, [] { return std::unordered_set<std::string>{}; });
  CHECK(!recommendations.contains(recommendedHash));
  recommendations.refresh("profile", 2, 2, loadRecommendations);
  CHECK(rejects([&] { recommendations.refresh("profile", 3, 2, []() -> std::unordered_set<std::string> {
    throw std::runtime_error("unavailable");
  }); }));
  CHECK(!recommendations.contains(recommendedHash));
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
  for (int light : {50, 150}) {
    lamp.clearType = light;
    CHECK(!evaluate({.lamp=100}, {}, lamp).achieved);
    CHECK(evaluate({.lamp=50}, {}, lamp).achieved);
  }
  lamp.clearType = 100;
  CHECK(evaluate({.lamp=50}, {}, lamp).achieved);
  CHECK(evaluate({.lamp=100}, {}, lamp).achieved);
  CHECK(rejects([] { evaluate({}, {}, {}); }));
  CHECK(rejects([] { evaluate({.lamp=0}, {}, {}); }));
  CHECK(rejects([] { evaluate({.scoreNumerator=28}, {}, {}); }));
  CHECK(rejects([] { evaluate({.scoreNumerator=1, .scoreDenominator=0}, {}, {}); }));

  // Independent targets in the same chart/mode must survive together.
  {
    sqlite3 *multiple = nullptr;
    CHECK(sqlite3_open(":memory:", &multiple) == SQLITE_OK);
    Goal first;
    first.chart.SHA256 = std::string(64, 'e');
    first.target.lamp = 400;
    save(multiple, first);
    auto second = first;
    second.target = {.scoreNumerator=24};
    save(multiple, second);
    const auto goals = loadForChart(multiple, first.chart.SHA256);
    CHECK(goals.size() == 2);
    CHECK(goals[0].target.lamp == 400 && goals[1].target.scoreNumerator == 24);
    CHECK(goals[0].id > 0 && goals[1].id != goals[0].id);
    auto edited = goals[0];
    edited.target.lamp = 500;
    CHECK(save(multiple, edited) == edited.id);
    CHECK(load(multiple, first.chart.SHA256, 0, goals[1].id)->target.scoreNumerator == 24);
    CHECK(rejects([&] { remove(multiple, first.chart.SHA256, 0); }));
    remove(multiple, first.chart.SHA256, 0, edited.id);
    CHECK(loadForChart(multiple, first.chart.SHA256).size() == 1);
    CHECK(rejects([&] { save(multiple, edited); })); // A stale editor cannot recreate a deleted goal.
    // Recreate the old schema and verify IDs, dates and body bytes survive migration.
    CHECK(sqlite3_exec(multiple,
        "CREATE TABLE legacy(sha256 TEXT NOT NULL,ln_mode INTEGER NOT NULL,body TEXT NOT NULL,PRIMARY KEY(sha256,ln_mode));"
        "INSERT INTO legacy(rowid,sha256,ln_mode,body) SELECT rowid,sha256,ln_mode,body FROM chart_goals;"
        "DROP TABLE chart_goals; ALTER TABLE legacy RENAME TO chart_goals;",
        nullptr, nullptr, nullptr) == SQLITE_OK);
    const auto legacy = loadForChart(multiple, first.chart.SHA256).front();
    first.setAtUnixMillis = 1730000000000;
    first.id = save(multiple, first);
    CHECK(first.id != legacy.id);
    const auto migrated = load(multiple, first.chart.SHA256, 0, legacy.id);
    CHECK(migrated && migrated->target == legacy.target && migrated->setAtUnixMillis == legacy.setAtUnixMillis);
    for (int i = 0; i < 5; ++i) { auto extra = first; extra.id = 0; save(multiple, extra); }
    CHECK(loadForChart(multiple, first.chart.SHA256).size() == 7);
    CHECK(loadPage(multiple, 0).goals.size() == 7);
    sqlite3_close(multiple);
  }
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
  goal.id = save(db.get(), goal);
  CHECK(load(db.get(), std::string(64, 'a'), 1)->chart.Title == "Goal chart");
  CHECK(load(db.get(), goal.chart.SHA256, 1)->setAtUnixMillis == 1730000000000);
  CHECK(load(db.get(), goal.chart.SHA256, 1)->achievedAtUnixMillis == 1730000001234);
  CHECK(rejects([&] { auto invalid = goal; invalid.achievedAtUnixMillis = 1; save(db.get(), invalid); }));
  goal.target.lamp = 50;
  save(db.get(), goal);
  CHECK(load(db.get(), goal.chart.SHA256, 1)->target.lamp == 50);
  CHECK(sqlite3_exec(db.get(), "UPDATE chart_goals SET body=replace(body, '\"lamp\":50', '\"lamp\":150')",
      nullptr, nullptr, nullptr) == SQLITE_OK);
  CHECK(load(db.get(), goal.chart.SHA256, 1)->target.lamp == 50);
  CHECK(loadPage(db.get(), 0).goals.front().target.lamp == 50);
  CHECK(loadForChart(db.get(), goal.chart.SHA256).front().target.lamp == 50);
  goal = *load(db.get(), goal.chart.SHA256, 1);
  save(db.get(), goal);
  SqliteStatementHandle storedGoal;
  CHECK(prepareSqliteStatement(db.get(), "SELECT json_extract(body,'$.lamp') FROM chart_goals", storedGoal) == SQLITE_OK);
  CHECK(sqlite3_step(storedGoal) == SQLITE_ROW && sqlite3_column_int(storedGoal, 0) == 50);
  storedGoal.reset();
  CHECK(rejects([&] { auto invalid = goal; invalid.target.lamp = 150; save(db.get(), invalid); }));
  goal.chart.Title = "Updated"; save(db.get(), goal);
  CHECK(loadPage(db.get(), 0).goals.size() == 1);
  goal.longNoteMode = 2; goal.id = 0; goal.id = save(db.get(), goal);
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
    goal.id = 0;
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
  goal.id = 0; goal.id = save(db.get(), goal); db.reset();
  CHECK(sqlite3_open(path.string().c_str(), &raw) == SQLITE_OK); db.reset(raw);
  CHECK(load(db.get(), goal.chart.SHA256, 1)->chart.SubTitle == "Another");
  CHECK(sqlite3_exec(db.get(), "PRAGMA query_only=ON", nullptr, nullptr, nullptr) == SQLITE_OK);
  goal.chart.Title = "Must not persist";
  CHECK(rejects([&] { save(db.get(), goal); }));
  CHECK(load(db.get(), goal.chart.SHA256, 1)->chart.Title != goal.chart.Title);
  db.reset(); std::filesystem::remove(path);
  std::cout << "chart goal tests passed\n";
}
