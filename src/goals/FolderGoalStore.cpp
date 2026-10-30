#include "FolderGoalStore.h"
#include "../ClearLampOrder.h"
#include "../repositories/SqliteRAII.h"
#include "../repositories/ChartSqlExpressions.h"
#include "../../yoga/lib/nlohmann/json.hpp"
#include <limits>
#include <algorithm>
#include <stdexcept>

namespace folder_goals {
namespace {
void require(bool ok, sqlite3 *db) {
  if (!ok) throw std::runtime_error("Folder goal storage: " + sqliteDatabaseError(db));
}
bool exists(sqlite3 *db) {
  SqliteStatementHandle q;
  require(prepareSqliteStatement(db, "SELECT 1 FROM sqlite_master WHERE name='folder_goals' AND type='table'", q)==SQLITE_OK, db);
  int rc=sqlite3_step(q); require(rc==SQLITE_ROW || rc==SQLITE_DONE, db);
  return rc==SQLITE_ROW;
}
std::int64_t integer(const nlohmann::json &j, const char *name, std::int64_t low, std::int64_t high) {
  const auto &v=j.at(name);
  if (!v.is_number_integer() || v<low || v>high) throw std::runtime_error("Invalid folder goal data.");
  return v.get<std::int64_t>();
}
}
std::vector<Goal> load(sqlite3 *db) {
  std::vector<Goal> out;
  if (!exists(db)) return out;
  SqliteStatementHandle q;
  require(prepareSqliteStatement(db, "SELECT id,body FROM folder_goals ORDER BY id", q)==SQLITE_OK, db);
  int rc;
  while ((rc=sqlite3_step(q))==SQLITE_ROW) {
    if (sqlite3_column_bytes(q,1)>65536) throw std::runtime_error("Invalid folder goal data size.");
    auto j=nlohmann::json::parse(sqliteColumnString(q,1));
    const auto version=integer(j,"version",1,2147483647);
    if (version!=1 && version!=2 && version!=3) throw std::runtime_error("These folder goals need a different app version.");
    Goal g;
    g.source={static_cast<SourceKind>(integer(j,"kind",0,2)), j.at("identity").get<std::string>(),
        j.at("level").get<std::string>(), j.at("name").get<std::string>()};
    g.longNoteMode=integer(j,"mode",1,3);
    g.target={static_cast<Type>(integer(j,"type",0,version==3 ? 2 : 1)), normalizeClearLamp(integer(j,"lamp",0,600)),
        static_cast<int>(integer(j,"percent",1,10000))};
    if (version==3) g.target.scoreRank=integer(j,"scoreRank",0,27);
    if (g.target.type==Type::ExCoverage) g.target.exPercent=integer(j,"exPercent",1,10000);
    g.revision=integer(j,"revision",1,std::numeric_limits<std::int64_t>::max());
    g.setAt=integer(j,"setAt",0,253402300799999LL);
    g.achievedAt=integer(j,"achievedAt",0,253402300799999LL);
    g.id=sqliteColumnString(q,0);
    validate(g);
    if (g.id.empty() || (version==1 ? scopeKey(g) : j.at("id").get<std::string>())!=g.id)
      throw std::runtime_error("Invalid folder goal identity.");
    out.push_back(std::move(g));
  }
  require(rc==SQLITE_DONE,db);
  return out;
}
void save(sqlite3 *db, Goal &g) {
  validate(g);
  const bool creating=g.id.empty();
  if (!exists(db)) {
    if (!creating) throw std::runtime_error("This folder goal no longer exists.");
    require(sqlite3_exec(db,"CREATE TABLE folder_goals(id TEXT PRIMARY KEY,body TEXT NOT NULL) WITHOUT ROWID",nullptr,nullptr,nullptr)==SQLITE_OK,db);
  }
  // Keep the existing table and legacy keys. A table rename would reparse
  // unrelated triggers, including old ON score_db.scores definitions.
  auto id=g.id;
  if (creating) {
    SqliteStatementHandle generated;
    require(prepareSqliteStatement(db,"SELECT 'goal:' || lower(hex(randomblob(16)))",generated)==SQLITE_OK &&
        sqlite3_step(generated)==SQLITE_ROW,db);
    id=sqliteColumnString(generated,0);
  }
  nlohmann::json j={{"version",g.target.scoreRank>0 || g.target.type==Type::ExCoverage ? 3 : 2},{"id",id},{"kind",static_cast<int>(g.source.kind)}, {"identity",g.source.identity},
      {"level",g.source.level},{"name",g.source.name},{"mode",g.longNoteMode},{"type",static_cast<int>(g.target.type)},
      {"lamp",g.target.lamp},{"percent",g.target.percent},{"revision",g.revision},{"setAt",g.setAt},{"achievedAt",g.achievedAt}};
  if (g.target.scoreRank>0 || g.target.type==Type::ExCoverage) j["scoreRank"]=g.target.scoreRank;
  if (g.target.type==Type::ExCoverage) j["exPercent"]=g.target.exPercent;
  const auto body=j.dump();
  if (body.size()>65536) throw std::runtime_error("Invalid folder goal data size.");
  SqliteStatementHandle q;
  require(prepareSqliteStatement(db,creating ? "INSERT INTO folder_goals(id,body) VALUES(?1,?2)"
      : "UPDATE folder_goals SET body=?2 WHERE id=?1",q)==SQLITE_OK,db);
  require(bindSqliteText(q,1,id) && bindSqliteText(q,2,body) && sqlite3_step(q)==SQLITE_DONE,db);
  if (sqlite3_changes(db)!=1) throw std::runtime_error("This folder goal no longer exists.");
  g.id=std::move(id);
}
void remove(sqlite3 *db,const Goal &g) {
  validate(g);
  if (g.id.empty()) throw std::runtime_error("Choose a saved folder goal.");
  if (!exists(db)) return;
  SqliteStatementHandle q;
  require(prepareSqliteStatement(db,"DELETE FROM folder_goals WHERE id=?",q)==SQLITE_OK,db);
  require(bindSqliteText(q,1,key(g)) && sqlite3_step(q)==SQLITE_DONE,db);
}
Evidence loadEvidence(sqlite3 *db) {
  Evidence out;
  SqliteStatementHandle q;
  require(prepareSqliteStatement(db,"SELECT chart_sha256,ln_mode,rank FROM score_sha256_clear_rank_cache",q)==SQLITE_OK,db);
  int rc;
  while ((rc=sqlite3_step(q))==SQLITE_ROW) {
    int mode=sqlite3_column_int(q,1);
    if (mode>=0 && mode<=3) out[sqliteColumnString(q,0)][mode].lamp=normalizeClearLamp(sqlite3_column_int(q,2));
  }
  require(rc==SQLITE_DONE,db); q.reset();
  require(prepareSqliteStatement(db,"SELECT chart_sha256,ln_mode,score,max_score FROM score_sha256_best_score_cache",q)==SQLITE_OK,db);
  while ((rc=sqlite3_step(q))==SQLITE_ROW) {
    int mode=sqlite3_column_int(q,1);
    if (mode<0 || mode>3) continue;
    auto &e=out[sqliteColumnString(q,0)][mode];
    e.hasScore=true; e.score=sqlite3_column_int(q,2); e.maxScore=sqlite3_column_int(q,3);
  }
  require(rc==SQLITE_DONE,db);
  return out;
}
void Projector::refresh() {
  SqliteStatementHandle q;
  require(prepareSqliteStatement(charts_,"PRAGMA data_version",q)==SQLITE_OK && sqlite3_step(q)==SQLITE_ROW,charts_);
  int version=sqlite3_column_int(q,0);
  if (version!=dataVersion_) { cache_.clear(); dataVersion_=version; }
}
const Projector::Membership &Projector::members(const Source &source) {
  auto id=sourceKey(source);
  if (auto it=cache_.find(id); it!=cache_.end()) return it->second;
  Membership result;
  SqliteStatementHandle q;
  std::string sql;
  if (source.kind==SourceKind::Directory) {
    // Folder identities have the same stored path representation as chart_meta.
    auto forward=source.identity;
    std::replace(forward.begin(),forward.end(),'\\','/');
    if (!forward.ends_with('/')) forward+='/';
    auto backward=forward; std::replace(backward.begin(),backward.end(),'/','\\');
    auto forwardUpper=forward, backwardUpper=backward;
    ++forwardUpper.back(); ++backwardUpper.back();
    sql="SELECT sha256,path,ln_mode,total_long_notes+total_backspin_notes,total_notes FROM chart_meta "
        "WHERE (path>=?1 AND path<?2) OR (path>=?3 AND path<?4) ORDER BY path";
    require(prepareSqliteStatement(charts_,sql,q)==SQLITE_OK,charts_);
    require(bindSqliteText(q,1,forward) && bindSqliteText(q,2,forwardUpper) &&
        bindSqliteText(q,3,backward) && bindSqliteText(q,4,backwardUpper),charts_);
  } else {
    SqliteStatementHandle found;
    require(prepareSqliteStatement(charts_,"SELECT id FROM difficulty_tables WHERE source_url=?",found)==SQLITE_OK &&
        bindSqliteText(found,1,source.identity),charts_);
    int rc=sqlite3_step(found); require(rc==SQLITE_ROW || rc==SQLITE_DONE,charts_);
    if (rc==SQLITE_DONE) return cache_.emplace(id,std::move(result)).first->second;
    const int table=sqlite3_column_int(found,0);
    result.exists=source.kind==SourceKind::Table;
    sql="SELECT COALESCE(NULLIF(cm.sha256,''),NULLIF(e.sha256,''),'md5:'||e.md5),cm.path,"
        "cm.ln_mode,cm.total_long_notes+cm.total_backspin_notes,cm.total_notes "
        "FROM difficulty_table_entries e LEFT JOIN chart_meta cm ON cm.path=" +
        asobmshow::chart_sql::matchedChartPathSubquery("e") + " WHERE e.table_id=?1";
    if (source.kind==SourceKind::Level) sql+=" AND e.level=?2";
    require(prepareSqliteStatement(charts_,sql,q)==SQLITE_OK && sqlite3_bind_int(q,1,table)==SQLITE_OK,charts_);
    if (source.kind==SourceKind::Level) require(bindSqliteText(q,2,source.level),charts_);
  }
  int rc;
  while ((rc=sqlite3_step(q))==SQLITE_ROW) {
    result.exists=true;
    result.charts.push_back({.hash=sqliteColumnString(q,0),
        .available=!sqliteColumnString(q,1).empty(), .mode=sqlite3_column_int(q,2),
        .longNotes=sqlite3_column_int(q,3), .notes=sqlite3_column_int(q,4)});
  }
  require(rc==SQLITE_DONE,charts_);
  if (!result.exists && source.kind==SourceKind::Directory) {
    q.reset();
    require(prepareSqliteStatement(charts_,"SELECT 1 FROM folder WHERE path=?1 OR path=?2",q)==SQLITE_OK,charts_);
    auto path=source.identity; while (path.size()>1 && path.back()=='/') path.pop_back();
    require(bindSqliteText(q,1,path) && bindSqliteText(q,2,path+"/"),charts_);
    rc=sqlite3_step(q); require(rc==SQLITE_ROW || rc==SQLITE_DONE,charts_); result.exists=rc==SQLITE_ROW;
  }
  return cache_.emplace(id,std::move(result)).first->second;
}
bool Projector::contains(const Source &source,std::string_view hash) {
  const auto &m=members(source);
  return std::any_of(m.charts.begin(),m.charts.end(),[&](const auto &c){ return c.available && c.hash==hash; });
}
Aggregate Projector::summarize(const Goal &goal,const Evidence &evidence) {
  const auto &membership=members(goal.source);
  std::vector<Member> values;
  values.reserve(membership.charts.size());
  for (const auto &c:membership.charts) {
    Member m{.identity=c.hash,.available=c.available};
    const int mode=c.longNotes==0 ? 0 : c.mode>=1 && c.mode<=3 ? c.mode : goal.longNoteMode;
    if (auto found=evidence.find(c.hash); found!=evidence.end()) {
      const auto &modes=found->second;
      m.lamp=modes[mode].lamp;
      if (m.lamp<0 && mode!=0) m.lamp=modes[1].lamp>=0 ? modes[1].lamp : modes[0].lamp;
      const auto &best=mode==1 && !modes[1].hasScore ? modes[0] : modes[mode];
      if (best.hasScore) { m.score=best.score; m.maxScore=best.maxScore; }
    }
    if (m.maxScore<=0 && c.notes>0 && c.notes<=std::numeric_limits<int>::max()/2) m.maxScore=c.notes*2;
    values.push_back(std::move(m));
  }
  auto result=aggregate(values); result.counts.sourceAvailable=membership.exists;
  return result;
}
Progress Projector::project(const Goal &goal,const Evidence &evidence) {
  return projectAll(std::span(&goal,1),evidence).front();
}
std::vector<Progress> Projector::projectAll(std::span<const Goal> goals,const Evidence &evidence) {
  // Evidence is one score snapshot. Keep only one scope aggregate in memory,
  // and never retain it across operations.
  std::map<std::pair<std::string,int>,std::vector<std::size_t>> groups;
  for (std::size_t i=0;i<goals.size();++i)
    groups[{sourceKey(goals[i].source),goals[i].longNoteMode}].push_back(i);
  std::vector<Progress> out(goals.size());
  for (const auto &[scope,indices]:groups) {
    const auto summary=summarize(goals[indices.front()],evidence);
    for (auto i:indices) {
      auto progress=evaluateAggregate(goals[i].target,summary);
      if (!progress.sourceAvailable) progress.achieved=false;
      out[i]=progress;
    }
  }
  return out;
}
} // namespace folder_goals
