#pragma once
#include "repositories/SqliteRAII.h"
#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

// Pinned release DDL, rather than relabeling today's schema with an old version.
namespace testflight166 {
inline void exec(sqlite3 *db,const std::string &sql) {
  if (auto error=executeSqlite(db,sql.c_str())) { std::cerr<<*error<<'\n'; std::abort(); }
}
inline std::string identifier(const std::string &name) {
  std::string result="\"";
  for (char c:name) { result+=c; if (c=='"') result+=c; }
  return result+'"';
}
inline std::vector<std::string> strings(sqlite3 *db,const std::string &sql) {
  SqliteStatementHandle q;
  assert(prepareSqliteStatement(db,sql,q)==SQLITE_OK);
  std::vector<std::string> out; int rc;
  while ((rc=sqlite3_step(q))==SQLITE_ROW) out.push_back(sqliteColumnString(q,0));
  assert(rc==SQLITE_DONE); return out;
}
inline void install(const std::filesystem::path &path,const char *schema) {
  sqlite3 *raw=nullptr;
  const auto rebuilt=path.string()+".release166";
  assert(!std::filesystem::exists(rebuilt));
  assert(sqlite3_open(rebuilt.c_str(),&raw)==SQLITE_OK);
  SqliteConnectionHandle db(raw);
  const auto fixture=std::filesystem::path(__FILE__).parent_path().parent_path()/"fixtures/testflight166"/schema;
  std::ifstream input(fixture); assert(input);
  exec(db.get(),std::string(std::istreambuf_iterator<char>(input),{}));
  const auto triggers=strings(db.get(),"SELECT sql FROM sqlite_master WHERE type='trigger'");
  for (const auto &name:strings(db.get(),"SELECT name FROM sqlite_master WHERE type='trigger'"))
    exec(db.get(),"DROP TRIGGER "+identifier(name));
  {
    SqliteStatementHandle attach;
    assert(prepareSqliteStatement(db.get(),"ATTACH DATABASE ? AS source",attach)==SQLITE_OK);
    assert(bindSqliteText(attach,1,path.string()) && sqlite3_step(attach)==SQLITE_DONE);
  }
  exec(db.get(),"BEGIN");
  for (const auto &table:strings(db.get(),"SELECT name FROM main.sqlite_master WHERE type='table'")) {
    std::string columns;
    for (const auto &column:strings(db.get(),"SELECT name FROM pragma_table_info("+identifier(table)+")")) {
      if (!columns.empty()) columns+=',';
      columns+=identifier(column);
    }
    exec(db.get(),"DELETE FROM main."+identifier(table));
    auto sql="INSERT INTO main."+identifier(table)+"("+columns+") SELECT "+columns+" FROM source."+identifier(table);
    if (table=="sqlite_sequence") sql+=" WHERE name IN(SELECT name FROM main.sqlite_master WHERE type='table')";
    exec(db.get(),sql);
  }
  exec(db.get(),"COMMIT; DETACH DATABASE source");
  for (const auto &sql:triggers) exec(db.get(),sql);
  db.reset();
  // Callers close all repository handles before installing the release fixture.
  std::filesystem::remove(path);
  std::filesystem::rename(rebuilt,path);
}
using Rows=std::map<std::string,std::vector<std::string>>;
inline Rows snapshot(sqlite3 *db,bool canonicalLamps=false) {
  Rows result;
  for (const auto &table:strings(db,"SELECT name FROM sqlite_master WHERE type='table' ORDER BY name")) {
    std::string columns;
    for (const auto &column:strings(db,"SELECT name FROM pragma_table_info("+identifier(table)+")")) {
      auto expression=identifier(column);
      if (canonicalLamps && ((table=="ir_remote_scores" && column=="lamp_rank") ||
          ((table=="scores" || table=="course_scores") && column=="clear_type")))
        expression="CASE WHEN "+expression+"=150 THEN 50 ELSE "+expression+" END";
      if (!columns.empty()) columns+="||'|'||";
      // quote preserves NULLs, strings (including JSON bytes), blobs and numbers.
      columns+="quote("+expression+")";
    }
    auto &rows=result[table]; rows=strings(db,"SELECT "+columns+" FROM "+identifier(table));
    std::sort(rows.begin(),rows.end());
  }
  return result;
}
inline void preserved(const Rows &before,const Rows &after,const std::set<std::string> &derived={}) {
  for (const auto &[table,rows]:before) {
    if (derived.contains(table)) continue;
    const auto found=after.find(table);
    if (found==after.end() || found->second!=rows) {
      std::cerr<<"Release migration changed rows in "<<table<<'\n'; std::abort();
    }
  }
}
inline std::string schemaSnapshot(sqlite3 *db) {
  std::string out;
  for (const auto &sql:strings(db,"SELECT type||'|'||name||'|'||COALESCE(sql,'') FROM sqlite_master ORDER BY name")) out+=sql+'\n';
  return out;
}
} // namespace testflight166
