#include "goals/FolderGoal.h"
#include "goals/FolderGoalStore.h"
#include "goals/FolderGoalStatusIndex.h"
#include "repositories/SqliteRAII.h"
#include <cmath>
#include <cstdlib>
#include <iostream>
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while(false)
using namespace folder_goals;
int main() {
  Target lamp{.type=Type::Lamp, .lamp=200, .percent=7500};
  std::vector<Member> members{
      {.identity="a", .lamp=200}, {.identity="b", .lamp=300},
      {.identity="c", .lamp=400}, {.identity="d"},
      {.identity="missing", .available=false}, {.identity="missing2", .available=false},
      {.identity="a", .lamp=200}};
  auto p = evaluate(lamp, members);
  CHECK(p.available == 4 && p.missing == 2 && p.qualifying == 3 && p.achieved);
  CHECK(p.required == 3 && p.percent == 75);
  lamp.percent = 7501;
  CHECK(!evaluate(lamp, members).achieved && evaluate(lamp, members).required == 4);
  lamp.lamp = 0; lamp.percent = 10000; members[3].lamp = 0;
  CHECK(evaluate(lamp, members).achieved);
  lamp.lamp = 100; members[3].lamp = 150;
  CHECK(!evaluate(lamp, members).achieved);
  Target ex{.type=Type::AverageEx, .percent=7000};
  members = {{.identity="a", .score=100, .maxScore=200},
             {.identity="b", .score=900, .maxScore=1000},
             {.identity="missing", .available=false, .score=1000, .maxScore=1000}};
  p = evaluate(ex, members);
  CHECK(p.achieved && std::abs(p.percent-70) < 1e-10);
  ex.percent = 7001; CHECK(!evaluate(ex, members).achieved);
  members.push_back({.identity="unplayed"});
  CHECK(std::abs(evaluate(ex, members).percent-140.0/3) < 1e-10);
  CHECK(!evaluate(ex, {}).achieved);
  members = {{.identity="invalid", .score=1, .maxScore=0}};
  CHECK(evaluate(ex, members).unknownScore == 1);
  bool rejected = false;
  try { ex.percent=0; (void)evaluate(ex, members); } catch (...) { rejected=true; }
  CHECK(rejected);
  sqlite3 *db = nullptr;
  CHECK(sqlite3_open(":memory:", &db) == SQLITE_OK);
  Goal goal{.source={.kind=SourceKind::Level, .identity="https://table", .level="1", .name="Level 1"},
            .target={.type=Type::Lamp, .lamp=200, .percent=10000}, .setAt=100};
  CHECK(load(db).empty());
  save(db, goal);
  CHECK(load(db).size()==1 && load(db)[0].source==goal.source);
  CHECK(sqlite3_exec(db, "UPDATE folder_goals SET body=json_set(body,'$.version',999)", nullptr,nullptr,nullptr)==SQLITE_OK);
  rejected=false; try { (void)load(db); } catch (...) { rejected=true; } CHECK(rejected);
  CHECK(sqlite3_exec(db,"DELETE FROM folder_goals;"
      "CREATE TABLE chart_meta(path TEXT PRIMARY KEY,sha256 TEXT,md5 TEXT,ln_mode INTEGER,total_long_notes INTEGER,total_backspin_notes INTEGER,total_notes INTEGER);"
      "CREATE INDEX hashes ON chart_meta(sha256); CREATE INDEX md5s ON chart_meta(md5);"
      "CREATE TABLE folder(path TEXT);"
      "CREATE TABLE difficulty_tables(id INTEGER PRIMARY KEY,source_url TEXT);"
      "CREATE TABLE difficulty_table_entries(table_id INTEGER,level TEXT,sha256 TEXT,md5 TEXT);"
      "INSERT INTO difficulty_tables VALUES(1,'https://table');"
      "INSERT INTO difficulty_table_entries VALUES(1,'1','a',''),(1,'1','a',''),(1,'1','missing',''),(1,'1','b','');"
      "INSERT INTO chart_meta VALUES('/charts/a','a','',0,0,0,100),('/charts/a2','a','',0,0,0,100),('/charts/b','b','',0,1,0,500);"
      "CREATE TABLE score_sha256_clear_rank_cache(chart_sha256 TEXT,ln_mode INTEGER,rank INTEGER);"
      "CREATE TABLE score_sha256_best_score_cache(chart_sha256 TEXT,ln_mode INTEGER,score INTEGER,max_score INTEGER);"
      "INSERT INTO score_sha256_clear_rank_cache VALUES('a',0,200),('b',1,300),('b',2,0);"
      "INSERT INTO score_sha256_best_score_cache VALUES('a',0,100,200),('b',1,900,1000);",
      nullptr,nullptr,nullptr)==SQLITE_OK);
  Projector projector(db); projector.refresh();
  auto evidence=loadEvidence(db);
  p=projector.project(goal,evidence);
  CHECK(p.sourceAvailable && p.available==2 && p.missing==1 && p.achieved);
  goal.longNoteMode=2;
  CHECK(!projector.project(goal,evidence).achieved); // Explicit CN Failed wins over LN clear.
  goal.target={.type=Type::AverageEx,.percent=7000};
  CHECK(projector.project(goal,evidence).percent==25); // CN scores do not borrow LN scores.
  goal.longNoteMode=1;
  CHECK(projector.project(goal,evidence).achieved);
  CHECK(projector.contains(goal.source,"a") && !projector.contains(goal.source,"missing"));
  goal.source={.kind=SourceKind::Directory,.identity="/charts",.name="Charts"};
  CHECK(projector.project(goal,evidence).available==2);
  CHECK(sqlite3_exec(db,"INSERT INTO chart_meta VALUES('C:\\charts\\nested\\a.bms','win','',0,0,0,100)",nullptr,nullptr,nullptr)==SQLITE_OK);
  goal.source={.kind=SourceKind::Directory,.identity="C:\\charts",.name="Windows charts"};
  CHECK(projector.project(goal,evidence).available==1);
  goal.source={.kind=SourceKind::Table,.identity="gone",.name="Gone"};
  CHECK(!projector.project(goal,evidence).sourceAvailable);
  sqlite3_close(db);
  StatusIndex index;
  int loads=0;
  const auto loader=[&]() -> std::optional<std::vector<GoalProgress>> {
    ++loads; return std::vector<GoalProgress>{{goal,{.achieved=true}}};
  };
  CHECK(index.refresh("profile",1,1,loader));
  for(int i=0;i<100;++i) {
    CHECK(index.refresh("profile",1,1,loader));
    (void)index.status(Source{.identity=std::to_string(i)},1);
  }
  CHECK(loads==1 && index.status(goal.source,goal.longNoteMode).achieved);
  CHECK(index.refresh("profile",2,1,loader) && loads==2);
  CHECK(index.refresh("other-profile",2,1,loader) && loads==3);
  std::cout << "folder goal tests passed\n";
}
