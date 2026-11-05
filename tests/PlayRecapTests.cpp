#include "recap/PlayRecapQuery.h"
#include "recap/ChartMetadata.h"
#include "recap/Filters.h"
#include "recap/ShareText.h"
#include "kombo/KomboForm.h"
#include <cassert>
#include <cstdlib>
#include <iostream>

int main() {
  kombo::Assignment selected;
  const std::vector<kombo::Level> levels{{"a","2","a2"},{"a","10","a10"},{"a","X","aX"}};
  assert(kombo::addLevelRange(selected,levels,0,1));
  assert(kombo::addLevelRange(selected,levels,1,2));
  assert(kombo::addLevelRange(selected,{{"b","2","b2"}},0,0));
  assert(selected.levels.size()==4); // Table order, overlapping ranges, distinct tables.
  recap::Snapshot original;
  original.range={10,20};
  for (auto &section:original.sections) {
    section.push_back({.hash="a",.before=0,.after=1});
    section.push_back({.hash="b",.before=std::nullopt,.after=2});
    section.push_back({.hash="c",.before=1,.after=3});
  }
  auto filtered=original;
  recap::filterSnapshot(filtered,false,std::nullopt);
  assert(filtered.range.start==10 && filtered.sections[0].size()==2);
  filtered=original;
  recap::filterSnapshot(filtered,true,std::unordered_set<std::string>{"a","b"});
  assert(filtered.sections[1].size()==2 && filtered.sections[1][1].hash=="b");
  recap::filterSnapshot(filtered,false,std::unordered_set<std::string>{"a","b"});
  assert(filtered.sections[2].size()==1 && filtered.sections[2][0].hash=="a");
  filtered.sections[1].push_back({.hash="d",.before=10,.after=20});
  const std::array<std::string,3> sectionNames{"Lamp upgrades","Score upgrades","BP upgrades"};
  const auto caption=recap::shareText(filtered,"Play recap","Weekly · 2026-11-02 — 2026-11-08",
      "Difficulty ranges: sl6–sl8 · Include new plays: Off",sectionNames);
  assert(caption=="Play recap\nWeekly · 2026-11-02 — 2026-11-08\nLamp upgrades: 1 · Score upgrades: 2 · BP upgrades: 1\nDifficulty ranges: sl6–sl8 · Include new plays: Off\n#AsoBMaShow");
  recap::filterSnapshot(filtered,true,std::unordered_set<std::string>{});
  assert(filtered.sections[0].empty()); // Active ranges with no matches never mean all.
  assert(recap::shareText(filtered,"요약","오늘","",sectionNames).find("BP upgrades: 0\n#AsoBMaShow")!=std::string::npos);
  bms_parser::ChartMeta meta;
  meta.Title="Song"; meta.SubTitle="[ANOTHER]"; meta.PlayLevel=12;
  meta.Folder="charts/song"; meta.StageFile="stage.png"; meta.Banner="banner.png";
  auto metadata=recap::chartMetadata(meta,"sl6",true);
  assert(metadata.title=="Song [ANOTHER]" && metadata.difficulty=="sl6");
  assert(metadata.jacket==meta.Folder/meta.StageFile);
  recap::Upgrade identity{.title="Song [ANOTHER]"};
  recap::applyChartMetadata(identity,metadata);
  assert(identity.title=="Song [ANOTHER]"); // Replace metadata; never append the subtitle twice.
  meta.SubTitle.clear(); meta.StageFile.clear();
  metadata=recap::chartMetadata(meta,"",true);
  assert(metadata.title=="Song" && metadata.difficulty=="Lv. 12");
  assert(metadata.jacket==meta.Folder/meta.Banner);
  assert(recap::chartMetadata(meta,"",false).jacket.empty());
  meta.Banner.clear();
  assert(recap::chartMetadata(meta,"",true).jacket.empty());
  recap::applyChartMetadata(identity,{});
  assert(identity.title=="Song [ANOTHER]" && identity.jacket.empty()); // Missing library metadata retains the saved title.
  sqlite3 *db = nullptr;
  assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
  auto sql = [&](const std::string &query) { assert(sqlite3_exec(db, query.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK); };
  sql("CREATE TABLE scores(id INTEGER PRIMARY KEY, chart_sha256 TEXT, ln_mode INTEGER, chart_title TEXT, score INTEGER, clear_type INTEGER, combo_break INTEGER, max_score INTEGER, pgreat INTEGER, great INTEGER, good INTEGER, bad INTEGER, poor INTEGER, provenance_json TEXT, eligibility INTEGER, score_source INTEGER, created_at TEXT, kpoor INTEGER NOT NULL DEFAULT 0, bad_points INTEGER)");
  auto insert = [&](const std::string &values) {
    sql("INSERT INTO scores(id,chart_sha256,ln_mode,chart_title,score,clear_type,combo_break,max_score,pgreat,great,good,bad,poor,provenance_json,eligibility,score_source,created_at) VALUES" + values);
  };
  insert("(1,'a',0,'Chart A',100,150,10,200,90,0,0,0,10,'{}',0,0,'2026-11-01 10:00:00'),"
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
  insert("(9,'b',0,'Chart B',199,50,0,200,100,0,0,0,0,'{}',1,0,'2026-11-02 10:01:00')");
  auto assisted = recap::load(db, {*start,*end}, "s.eligibility=0", "s.eligibility=0 OR s.clear_type=50", "s.clear_type");
  assert(assisted.sections[0].size()==3 && assisted.sections[1].size()==2 && assisted.sections[2].size()==2);
  // Equal records are not improvements; an upgrade at the end is exclusive.
  auto equal = recap::load(db, {*start+10*3600+30*60+1,*start+11*3600}, "s.eligibility=0", "s.eligibility=0", "s.clear_type");
  assert(equal.sections[0].empty() && equal.sections[1].empty() && equal.sections[2].empty());
  // A session break at exactly 60 minutes starts a new session.
  insert("(10,'c',0,'',10,0,5,200,0,0,0,0,0,'{}',0,0,'2026-11-02 12:00:00')");
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
  setenv("TZ", "America/Santiago", 1); tzset();
  const auto midnightGap = recap::parseLocalDate("2026-09-06");
  const auto nextMidnight = recap::parseLocalDate("2026-09-07");
  assert(midnightGap && nextMidnight && recap::localTime(*midnightGap).tm_hour==1);
  assert(recap::calendarRange(*midnightGap,false).end==*nextMidnight);
  assert(recap::shiftDays(*midnightGap,1)==*nextMidnight); // Inclusive custom end date.
  assert(recap::localTime(recap::shiftDays(*midnightGap,-1)).tm_hour==0);
  setenv("TZ", "UTC", 1); tzset();
#endif
  sql("DELETE FROM scores");
  insert("(1,'legacy',-1,'Legacy',150,200,3,200,97,0,0,0,3,'{}',0,0,'2026-11-01 10:00:00'),"
      "(2,'legacy',1,'Legacy',100,100,10,200,90,0,0,0,10,'{}',0,0,'2026-11-02 10:00:00'),"
      "(3,'',0,'Unknown A',150,100,3,200,97,0,0,0,3,'{}',0,0,'2026-11-02 10:00:00'),"
      "(4,'',0,'Unknown B',190,200,0,200,100,0,0,0,0,'{}',0,0,'2026-11-02 11:00:00')");
  auto legacy = recap::load(db, {*start,*end}, "1", "1", "s.clear_type");
  assert(legacy.sections[0].empty() && legacy.sections[1].empty() && legacy.sections[2].empty());
  sql("DELETE FROM scores");
  insert("(1,'pms',0,'PMS',100,0,8,200,90,0,0,10,10,'{\"ruleset\":{\"id\":\"beatoraja\"},\"gaugeProfile\":\"standard-9-keys\"}',0,0,'2026-11-02 10:00:00'),"
      "(2,'other',0,'Other',100,0,8,200,90,0,0,10,10,'broken',0,0,'2026-11-02 10:00:00')");
  auto pms = recap::load(db, {*start,*end}, "1", "1", "s.clear_type");
  assert(pms.error.empty() && pms.sections[2].size()==1 && pms.sections[2][0].hash=="pms");
  // BP includes empty POORs, unlike combo breaks. Prefer exact saved BP,
  // including the unpassed-note penalty, even for an unfinished failed play.
  sql("DELETE FROM scores");
  insert("(1,'bp',0,'BP chart',100,100,10,200,90,0,0,0,10,'{}',0,0,'2026-11-01 10:00:00'),"
      "(2,'bp',0,'BP chart',120,100,12,200,88,0,0,0,12,'{}',0,0,'2026-11-02 10:00:00'),"
      "(3,'bp',0,'BP chart',0,0,0,200,0,0,0,0,0,'{}',0,0,'2026-11-02 11:00:00')");
  sql("UPDATE scores SET kpoor=20 WHERE id=1"); // Old BP=30, not CB=10.
  sql("UPDATE scores SET kpoor=3 WHERE id=2");  // New BP=15 despite worse CB.
  sql("UPDATE scores SET bad_points=100 WHERE id=3"); // Aborted, not zero BP.
  auto bp = recap::load(db,{*start,*end},"1","1","s.clear_type");
  assert(bp.sections[2].size()==1 && bp.sections[2][0].before==30 && bp.sections[2][0].after==15);
  sql("UPDATE scores SET bad_points=14 WHERE id=3"); // Exact saved BP can win on a failed play.
  bp = recap::load(db,{*start,*end},"1","1","s.clear_type");
  assert(bp.sections[2][0].after==14);
  sql("UPDATE scores SET bad_points=-1 WHERE id=3"); // Invalid stored values must not become bests.
  bp = recap::load(db,{*start,*end},"1","1","s.clear_type");
  assert(bp.sections[2][0].after==15);
  sql("UPDATE scores SET bad_points=2147483648 WHERE id=3");
  bp = recap::load(db,{*start,*end},"1","1","s.clear_type");
  assert(bp.sections[2][0].after==15);
  sql("UPDATE scores SET bad_points=35,combo_break=1 WHERE id=2"); // CB improved versus another row, but BP did not.
  bp = recap::load(db,{*start,*end},"1","1","s.clear_type");
  assert(bp.sections[2].empty());
  sql("UPDATE scores SET bad_points=0 WHERE id=2"); // Explicit zero is a valid best.
  bp = recap::load(db,{*start,*end},"1","1","s.clear_type");
  assert(bp.sections[2][0].after==0);
  sql("DELETE FROM scores");
  insert("(1,'a',0,'Alpha',100,50,1,200,99,0,0,0,1,'{}',0,0,'2026-11-02 10:00:00'),"
      "(2,'b',0,'Beta',100,400,1,200,99,0,0,0,1,'{}',0,0,'2026-11-02 10:00:00'),"
      "(3,'c',0,'Charlie',100,100,1,200,99,0,0,0,1,'{}',0,0,'2026-11-02 10:00:00'),"
      "(4,'d',0,'Delta',100,400,1,200,99,0,0,0,1,'{}',0,0,'2026-11-02 10:00:00')");
  auto grouped = recap::load(db,{*start,*end},"1","1","s.clear_type");
  assert(grouped.sections[0].size()==4);
  assert(grouped.sections[0][0].title=="Beta" && grouped.sections[0][1].title=="Delta");
  assert(grouped.sections[0][2].after==100 && grouped.sections[0][3].after==50);
  assert(grouped.sections[1][0].title=="Alpha"); // Only lamps are grouped by rank.
  sql("DROP TABLE scores");
  assert(!recap::load(db, {*start,*end}, "1", "1", "s.clear_type").error.empty());
  sqlite3_close(db);
  std::cout << "Play recap tests passed\n";
}
