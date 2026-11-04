#include "recap/PlayRecapQuery.h"
#include <cassert>
#include <cstdlib>
#include <iostream>

int main() {
  sqlite3 *db = nullptr;
  assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
  auto sql = [&](const char *query) { assert(sqlite3_exec(db, query, nullptr, nullptr, nullptr) == SQLITE_OK); };
  sql("CREATE TABLE scores(id INTEGER PRIMARY KEY, chart_sha256 TEXT, ln_mode INTEGER, chart_title TEXT, score INTEGER, clear_type INTEGER, combo_break INTEGER, max_score INTEGER, pgreat INTEGER, great INTEGER, good INTEGER, bad INTEGER, poor INTEGER, provenance_json TEXT, eligibility INTEGER, score_source INTEGER, created_at TEXT)");
  sql("INSERT INTO scores VALUES(1,'a',0,'Chart A',100,150,10,200,90,0,0,0,10,'{}',0,0,'2026-11-01 10:00:00'),"
      "(2,'a',0,'Chart A',120,100,8,200,92,0,0,0,8,'{}',0,0,'2026-11-02 10:00:00'),"
      "(3,'a',0,'Chart A',140,200,6,200,94,0,0,0,6,'{}',0,0,'2026-11-02 10:30:00'),"
      "(4,'a',0,'Chart A',20,0,0,200,10,0,0,0,0,'{}',0,0,'2026-11-02 10:40:00'),"
      "(5,'a',0,'Chart A',190,500,0,200,100,0,0,0,0,'{}',0,1,'2026-11-02 10:45:00'),"
      "(6,'a',0,'Chart A',180,400,1,200,99,0,0,0,1,'{}',1,0,'2026-11-02 10:50:00'),"
      "(7,'a',1,'Chart A',80,100,20,200,80,0,0,0,20,'{}',0,0,'2026-11-02 11:00:00'),"
      "(8,'a',0,'Chart A',180,400,1,200,99,0,0,0,1,'{}',0,0,'2026-11-03 00:00:00')");
  const auto start = recap::parseLocalDate("2026-11-02");
  const auto end = recap::parseLocalDate("2026-11-03");
  assert(start && end && !recap::parseLocalDate("2026-02-30") && !recap::parseLocalDate("2026-11-02x"));
  auto result = recap::load(db, {*start, *end}, "s.eligibility=0", "s.eligibility=0", "CASE s.clear_type WHEN 150 THEN 50 ELSE s.clear_type END");
  assert(result.error.empty());
  assert(result.sections[0].size() == 2 && result.sections[1].size() == 2 && result.sections[2].size() == 2);
  assert(result.sections[0][0].before == 50 && result.sections[0][0].after == 200);
  assert(result.sections[1][0].before == 100 && result.sections[1][0].after == 140);
  assert(result.sections[2][0].before == 10 && result.sections[2][0].after == 6);
  assert(!result.sections[2][1].before && result.sections[2][1].after == 20);
  auto session = recap::sessionRange(db, *end);
  assert(session && session->start == *start + 10*3600 && session->end == *start + 11*3600 + 1);
  auto previous = recap::sessionRange(db, session->start);
  assert(previous && previous->start == *start - 14*3600);
  assert(!recap::sessionRange(db, previous->start));
  assert(recap::load(db, {*end, *start}, "1", "1", "s.clear_type").error.size());
  auto empty = recap::load(db, {*end+86400, *end+172800}, "1", "1", "s.clear_type");
  assert(empty.error.empty() && empty.sections[0].empty());
  // Lamp eligibility is independent from score eligibility (assisted lamps).
  sql("INSERT INTO scores VALUES(9,'b',0,'Chart B',199,50,0,200,100,0,0,0,0,'{}',1,0,'2026-11-02 10:01:00')");
  auto assisted = recap::load(db, {*start,*end}, "s.eligibility=0", "s.eligibility=0 OR s.clear_type=50", "s.clear_type");
  assert(assisted.sections[0].size()==3 && assisted.sections[1].size()==2 && assisted.sections[2].size()==2);
  // Equal records are not improvements; an upgrade at the end is exclusive.
  auto equal = recap::load(db, {*start+10*3600+30*60+1,*start+11*3600}, "s.eligibility=0", "s.eligibility=0", "s.clear_type");
  assert(equal.sections[0].empty() && equal.sections[1].empty() && equal.sections[2].empty());
  // A session break at exactly 60 minutes starts a new session.
  sql("INSERT INTO scores VALUES(10,'c',0,'',10,0,5,200,0,0,0,0,0,'{}',0,0,'2026-11-02 12:00:00')");
  auto separate = recap::sessionRange(db,*end);
  assert(separate && separate->start==*start+12*3600);
  auto aborted = recap::load(db, {*start+12*3600,*end}, "s.eligibility=0", "s.eligibility=0", "s.clear_type");
  assert(aborted.sections[2].empty() && aborted.sections[1][0].title=="c");
  auto monday = recap::calendarRange(*start+12*3600,true);
  assert(monday.start==*start && monday.end==*start+7*86400);
#ifndef _WIN32
  setenv("TZ", "America/New_York", 1); tzset();
  const auto spring = recap::parseLocalDate("2026-03-08");
  assert(spring && recap::calendarRange(*spring,false).end-*spring==23*3600);
  const auto fall = recap::parseLocalDate("2026-11-01");
  assert(fall && recap::calendarRange(*fall,false).end-*fall==25*3600);
  setenv("TZ", "UTC", 1); tzset();
#endif
  sql("DELETE FROM scores");
  sql("INSERT INTO scores VALUES(1,'legacy',-1,'Legacy',150,200,3,200,97,0,0,0,3,'{}',0,0,'2026-11-01 10:00:00'),"
      "(2,'legacy',1,'Legacy',100,100,10,200,90,0,0,0,10,'{}',0,0,'2026-11-02 10:00:00'),"
      "(3,'',0,'Unknown A',150,100,3,200,97,0,0,0,3,'{}',0,0,'2026-11-02 10:00:00'),"
      "(4,'',0,'Unknown B',190,200,0,200,100,0,0,0,0,'{}',0,0,'2026-11-02 11:00:00')");
  auto legacy = recap::load(db, {*start,*end}, "1", "1", "s.clear_type");
  assert(legacy.sections[0].empty() && legacy.sections[1].empty() && legacy.sections[2].empty());
  sql("DELETE FROM scores");
  sql("INSERT INTO scores VALUES(1,'pms',0,'PMS',100,0,8,200,90,0,0,10,10,'{\"ruleset\":{\"id\":\"beatoraja\"},\"gaugeProfile\":\"standard-9-keys\"}',0,0,'2026-11-02 10:00:00'),"
      "(2,'other',0,'Other',100,0,8,200,90,0,0,10,10,'broken',0,0,'2026-11-02 10:00:00')");
  auto pms = recap::load(db, {*start,*end}, "1", "1", "s.clear_type");
  assert(pms.error.empty() && pms.sections[2].size()==1 && pms.sections[2][0].hash=="pms");
  sql("DROP TABLE scores");
  assert(!recap::load(db, {*start,*end}, "1", "1", "s.clear_type").error.empty());
  sqlite3_close(db);
  std::cout << "Play recap tests passed\n";
}
