#include "goals/FolderGoal.h"
#include "goals/FolderGoalStore.h"
#include "goals/FolderGoalStatusIndex.h"
#include "repositories/SqliteRAII.h"
#include <cmath>
#include <cstdlib>
#include <iostream>
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while(false)
using namespace folder_goals;
std::string queryText(sqlite3 *db,const char *sql) {
  SqliteStatementHandle q;
  CHECK(prepareSqliteStatement(db,sql,q)==SQLITE_OK && sqlite3_step(q)==SQLITE_ROW);
  return sqliteColumnString(q,0);
}
void testIndependentStorage() {
  sqlite3 *db=nullptr;
  CHECK(sqlite3_open(":memory:",&db)==SQLITE_OK);
  Goal first{.source={.kind=SourceKind::Level,.identity="https://table",.level="1"},
      .longNoteMode=2,.target={.type=Type::Lamp,.lamp=200,.percent=8000},.setAt=100};
  auto second=first; second.target.percent=9000;
  save(db,first); save(db,second);
  CHECK(load(db).size()==2);
  CHECK(key(first)!=key(second));
  first.target.percent=8500; save(db,first);
  auto rows=load(db);
  CHECK(rows.size()==2);
  for (const auto &g:rows) CHECK(g.target.percent==(key(g)==key(first) ? 8500 : 9000));
  remove(db,first);
  CHECK(load(db).size()==1 && key(load(db).front())==key(second));
  bool rejected=false; try { save(db,first); } catch (...) { rejected=true; }
  CHECK(rejected && load(db).size()==1);
  sqlite3_close(db);
}
void testLegacyStorageWithAttachedTrigger() {
  for (bool editFirst:{false,true}) {
    sqlite3 *source=nullptr,*db=nullptr;
    CHECK(sqlite3_open(":memory:",&source)==SQLITE_OK);
    CHECK(sqlite3_open(":memory:",&db)==SQLITE_OK);
    // Reproduce the persisted, connection-specific ON score_db.scores from
    // the old selector. Copying that schema keeps it invalid in main.
    CHECK(sqlite3_exec(source,
        "ATTACH DATABASE ':memory:' AS score_db;"
        "CREATE TABLE score_db.scores(id INTEGER);"
        "CREATE TRIGGER score_db.score_cache_summary_insert AFTER INSERT ON score_db.scores BEGIN SELECT 1; END;"
        "CREATE TABLE score_db.folder_goals(id TEXT PRIMARY KEY,body TEXT NOT NULL) WITHOUT ROWID;"
        R"(INSERT INTO score_db.folder_goals VALUES('2:13:https://table:1:2:0',
        '{"version":1,"kind":2,"identity":"https://table","level":"1","name":"Old level","mode":2,"type":0,"lamp":200,"percent":8000,"revision":7,"setAt":100,"achievedAt":200}');)",
        nullptr,nullptr,nullptr)==SQLITE_OK);
    auto *backup=sqlite3_backup_init(db,"main",source,"score_db");
    CHECK(backup && sqlite3_backup_step(backup,-1)==SQLITE_DONE);
    CHECK(sqlite3_backup_finish(backup)==SQLITE_OK); sqlite3_close(source);
    // Prove this fixture catches the earlier rename-trigger failure.
    char *error=nullptr;
    CHECK(sqlite3_exec(db,"ALTER TABLE folder_goals RENAME TO replaced",nullptr,nullptr,&error)!=SQLITE_OK);
    CHECK(error && std::string(error).find("score_db")!=std::string::npos);
    sqlite3_free(error);
    const auto oldBody=queryText(db,"SELECT body FROM folder_goals");
    const auto schema=queryText(db,"PRAGMA schema_version");
    auto original=load(db).front();
    CHECK(queryText(db,"SELECT body FROM folder_goals")==oldBody);
    const auto oldKey=key(original);
    CHECK(oldKey=="2:13:https://table:1:2:0");
    Goal added{.source=original.source,.longNoteMode=2,
        .target={.type=Type::Lamp,.lamp=400,.percent=9000},.setAt=300};
    if (editFirst) save(db,original);
    save(db,added);
    if (!editFirst) {
      CHECK(queryText(db,"SELECT body FROM folder_goals WHERE id='2:13:https://table:1:2:0'")==oldBody);
      save(db,original);
    }
    CHECK(queryText(db,"PRAGMA schema_version")==schema);
    const auto rows=load(db);
    CHECK(rows.size()==2 && key(added)!=oldKey);
    bool found=false;
    for (const auto &g:rows) if (key(g)==oldKey) {
      found=true;
      CHECK(g.revision==7 && g.setAt==100 && g.achievedAt==200);
      CHECK(g.target.percent==8000 && g.source.name=="Old level");
    }
    CHECK(found);
    remove(db,added);
    CHECK(load(db).size()==1 && key(load(db).front())==oldKey);
    sqlite3_close(db);
  }
}
void testExRateCoverage() {
  Target target{.type=Type::ExCoverage,.percent=5000,.scoreRank=24};
  std::vector<Member> members{
      {.identity="a",.score=1600,.maxScore=1800}, // Exactly AAA.
      {.identity="b",.score=88888,.maxScore=100000}, // Below AAA despite nearby display.
      {.identity="c",.score=200,.maxScore=200}, {.identity="unplayed"},
      {.identity="missing",.available=false}, {.identity="a",.score=1600,.maxScore=1800}};
  auto progress=evaluate(target,members);
  CHECK(progress.available==4 && progress.missing==1 && progress.qualifying==2);
  CHECK(progress.required==2 && progress.percent==50 && progress.achieved);
  target.percent=5001; CHECK(!evaluate(target,members).achieved && evaluate(target,members).required==3);
  CHECK(targetPercent(target)==50.01);
  target.scoreRank=0; target.exPercent=8888;
  CHECK(evaluate(target,members).qualifying==3 && evaluate(target,members).achieved);
  target.exPercent=8889; CHECK(evaluate(target,members).qualifying==1 && !evaluate(target,members).achieved);
  CHECK(!evaluate(target,{}).achieved);
  target.exPercent=0;
  bool rejected=false;try {validate(target);}catch(...){rejected=true;}CHECK(rejected);
  sqlite3 *db=nullptr;CHECK(sqlite3_open(":memory:",&db)==SQLITE_OK);
  Goal goal{.source={.identity="folder"},.target={.type=Type::ExCoverage,.percent=8000,.scoreRank=24}};
  save(db,goal);CHECK(load(db).front().target==goal.target);
  goal.target.scoreRank=0;goal.target.exPercent=8777;
  save(db,goal);CHECK(load(db).front().target==goal.target);
  sqlite3_close(db);
}
void testRankAverageTargets() {
  Target target{.type=Type::AverageEx,.scoreRank=24};
  CHECK(std::abs(targetPercent(target)-800.0/9)<1e-10);
  std::vector<Member> members{{.identity="a",.score=1600,.maxScore=1800}};
  CHECK(evaluate(target,members).achieved); // Exact AAA must not require rounded 88.89%.
  members[0].score=1599; CHECK(!evaluate(target,members).achieved);
  target.scoreRank=26; members[0].score=2600; members[0].maxScore=2700;
  CHECK(evaluate(target,members).achieved);
  target.scoreRank=27; CHECK(!evaluate(target,members).achieved);
  members[0].score=2700; CHECK(evaluate(target,members).achieved);
  target.scoreRank=25;
  bool rejected=false; try { validate(target); } catch (...) { rejected=true; }
  CHECK(rejected);
  target.scoreRank=24; target.type=Type::Lamp;
  rejected=false; try { validate(target); } catch (...) { rejected=true; }
  CHECK(rejected);
  sqlite3 *db=nullptr; CHECK(sqlite3_open(":memory:",&db)==SQLITE_OK);
  Goal g{.source={.identity="folder"},.target={.type=Type::AverageEx,.scoreRank=24},.setAt=100};
  save(db,g);
  const auto originalId=g.id;
  CHECK(load(db).front().target==g.target);
  CHECK(queryText(db,"SELECT json_extract(body,'$.version') FROM folder_goals")=="3");
  g.target.scoreRank=0; g.target.percent=8777; save(db,g);
  CHECK(load(db).front().target==g.target && g.id==originalId);
  CHECK(queryText(db,"SELECT json_extract(body,'$.version') FROM folder_goals")=="2");
  sqlite3_close(db);
}
int main() {
  testExRateCoverage();
  testRankAverageTargets();
  testIndependentStorage();
  testLegacyStorageWithAttachedTrigger();
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
      "CREATE TABLE chart_meta(path TEXT PRIMARY KEY,sha256 TEXT,md5 TEXT,ln_mode INTEGER,total_long_notes INTEGER,total_backspin_notes INTEGER,total_notes INTEGER,source_priority INTEGER,source_archive_size INTEGER);"
      "CREATE INDEX hashes ON chart_meta(sha256); CREATE INDEX md5s ON chart_meta(md5);"
      "CREATE TABLE folder(path TEXT);"
      "CREATE TABLE difficulty_tables(id INTEGER PRIMARY KEY,source_url TEXT);"
      "CREATE TABLE difficulty_table_entries(table_id INTEGER,level TEXT,sha256 TEXT,md5 TEXT);"
      "INSERT INTO difficulty_tables VALUES(1,'https://table');"
      "INSERT INTO difficulty_table_entries VALUES(1,'1','a',''),(1,'1','a',''),(1,'1','missing',''),(1,'1','b','');"
      "INSERT INTO chart_meta(path,sha256,md5,ln_mode,total_long_notes,total_backspin_notes,total_notes) VALUES('/charts/a','a','',0,0,0,100),('/charts/a2','a','',0,0,0,100),('/charts/b','b','',0,1,0,500);"
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
  CHECK(sqlite3_exec(db,"INSERT INTO chart_meta(path,sha256,md5,ln_mode,total_long_notes,total_backspin_notes,total_notes) VALUES('C:\\charts\\nested\\a.bms','win','',0,0,0,100)",nullptr,nullptr,nullptr)==SQLITE_OK);
  goal.source={.kind=SourceKind::Directory,.identity="C:\\charts",.name="Windows charts"};
  CHECK(projector.project(goal,evidence).available==1);
  CHECK(sqlite3_exec(db,
      "INSERT INTO difficulty_tables VALUES(2,'https://fallback');"
      "INSERT INTO difficulty_table_entries VALUES(2,'1','a',''),(2,'1','advertised','match-md5'),(2,'1','advertised','match-md5');"
      "INSERT INTO chart_meta VALUES('/a/archive','archive','match-md5',0,0,0,100,2,500),"
      "('/z/direct','resolved','match-md5',0,0,0,100,0,NULL);",nullptr,nullptr,nullptr)==SQLITE_OK);
  evidence["archive"][0].lamp=200;
  for (const auto kind : {SourceKind::Table,SourceKind::Level}) {
    Goal fallback{.source={.kind=kind,.identity="https://fallback",.level=kind==SourceKind::Level ? "1" : ""},
        .target={.type=Type::Lamp,.lamp=200,.percent=10000}};
    const auto progress=projector.project(fallback,evidence);
    CHECK(progress.sourceAvailable && progress.available==2 && progress.missing==0);
    CHECK(progress.qualifying==1 && !progress.achieved);
    CHECK(projector.contains(fallback.source,"resolved") && !projector.contains(fallback.source,"archive"));
    fallback.target={.type=Type::AverageEx,.percent=7000};
    CHECK(projector.project(fallback,evidence).percent==25);
  }
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
  auto lampGoal=goal;
  lampGoal.target={.type=Type::Lamp,.lamp=400,.percent=8000};
  auto exGoal=goal;
  exGoal.target={.type=Type::AverageEx,.percent=8500};
  CHECK(index.refresh("other-profile",3,1,[&]() -> std::optional<std::vector<GoalProgress>> {
    return std::vector<GoalProgress>{{lampGoal,{.percent=60}},{exGoal,{.percent=72.5}}};
  }));
  auto status=index.status(goal.source,goal.longNoteMode);
  CHECK(status.present && !status.achieved && status.pendingLamp && status.pendingEx);
  CHECK(status.pendingLamp->percent==60 && status.pendingLamp->target.percent==8000);
  CHECK(status.pendingEx->percent==72.5 && status.pendingEx->target.percent==8500);
  CHECK(!index.status(goal.source,2).present);
  CHECK(index.refresh("other-profile",4,1,[&]() -> std::optional<std::vector<GoalProgress>> {
    return std::vector<GoalProgress>{{lampGoal,{.percent=100,.achieved=true}},{exGoal,{.percent=80}}};
  }));
  status=index.status(goal.source,goal.longNoteMode);
  CHECK(status.present && !status.achieved && !status.pendingLamp && status.pendingEx);
  CHECK(status.pendingEx->percent==80);
  auto harderLamp=lampGoal; harderLamp.id="harder"; harderLamp.target.percent=9500;
  CHECK(index.refresh("other-profile",5,1,[&]() -> std::optional<std::vector<GoalProgress>> {
    return std::vector<GoalProgress>{{lampGoal,{.percent=60}},{harderLamp,{.percent=60}},{exGoal,{.percent=80}}};
  }));
  status=index.status(goal.source,goal.longNoteMode);
  CHECK(!status.achieved && status.pendingLampCount==2 && status.pendingExCount==1);
  CHECK(index.refresh("other-profile",6,1,[&]() -> std::optional<std::vector<GoalProgress>> {
    return std::vector<GoalProgress>{{lampGoal,{.percent=80,.achieved=true}},{harderLamp,{.percent=80}},{exGoal,{.percent=85,.achieved=true}}};
  }));
  status=index.status(goal.source,goal.longNoteMode);
  CHECK(!status.achieved && status.pendingLampCount==1 && status.pendingExCount==0);
  CHECK(status.pendingLamp->target.percent==9500);
  CHECK(index.refresh("other-profile",7,1,[]() -> std::optional<std::vector<GoalProgress>> { return {}; })==false);
  CHECK(!index.status(goal.source,goal.longNoteMode).present);
  std::cout << "folder goal tests passed\n";
}
