#include "../src/repositories/ChartRepository.h"
#include "../src/repositories/SqliteRAII.h"
#include "../src/music_select/MusicSelectSongIndex.h"
#include "RepositorySqliteTestSupport.h"

#include <array>
#include <cassert>
#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

sqlite3 **openingSession = nullptr;

int captureConnection(sqlite3 *database, char **, const sqlite3_api_routines *) {
  if (openingSession) *openingSession = database;
  return SQLITE_OK;
}

struct QueryTrace {
  std::vector<std::string> statements;
  std::size_t richRows = 0;
  int sorts = 0;
  int vmSteps = 0;
  std::stop_source *cancel = nullptr;

  static int callback(unsigned mask, void *context, void *statement, void *) {
    auto &trace = *static_cast<QueryTrace *>(context);
    auto *prepared = static_cast<sqlite3_stmt *>(statement);
    const std::string sql = sqlite3_sql(prepared);
    if (mask == SQLITE_TRACE_STMT) {
      char *expanded = sqlite3_expanded_sql(prepared);
      trace.statements.emplace_back(expanded);
      sqlite3_free(expanded);
      if (trace.cancel) trace.cancel->request_stop();
    }
    if (mask == SQLITE_TRACE_ROW && sql.find("cm.subtitle") != std::string::npos) {
      ++trace.richRows;
    }
    if (mask == SQLITE_TRACE_PROFILE) {
      trace.sorts += sqlite3_stmt_status(prepared, SQLITE_STMTSTATUS_SORT, 0);
      trace.vmSteps += sqlite3_stmt_status(prepared, SQLITE_STMTSTATUS_VM_STEP, 0);
    }
    return 0;
  }
};

struct Fixture {
  std::filesystem::path root = std::filesystem::temp_directory_path() /
      ("selector-sql-" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()));
  ChartRepository repository{root / "charts.db"};
  std::optional<ChartRepository::Session> session;
  sqlite3 *database = nullptr;
  sqlite3 *sessionDatabase = nullptr;

  Fixture() {
    std::filesystem::create_directories(root);
    assert(repository.EnsureReady());
    reopen();
    std::string error;
    database = openSqliteDatabase(root / "charts.db", error);
    assert(database);
  }

  void reopen() {
    session.reset();
    openingSession = &sessionDatabase;
    assert(sqlite3_auto_extension(reinterpret_cast<void (*)()>(captureConnection)) == SQLITE_OK);
    session = repository.OpenSession();
    sqlite3_cancel_auto_extension(reinterpret_cast<void (*)()>(captureConnection));
    openingSession = nullptr;
    assert(session && sessionDatabase);
  }

  void trace(QueryTrace *trace) {
    sqlite3_trace_v2(sessionDatabase,
        trace ? SQLITE_TRACE_STMT | SQLITE_TRACE_ROW | SQLITE_TRACE_PROFILE : 0,
        trace ? QueryTrace::callback : nullptr, trace);
  }

  ~Fixture() {
    sqlite3_close(database);
    session.reset();
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }

  void execute(const std::string &sql) {
    const auto error = executeSqlite(database, sql.c_str());
    if (error) throw std::runtime_error(*error);
  }

  std::string sqlRoot() const {
    auto text = fspath_to_utf8(root);
    std::ranges::replace(text, '\\', '/');
    std::size_t position = 0;
    while ((position = text.find('\'', position)) != std::string::npos) {
      text.insert(position, 1, '\'');
      position += 2;
    }
    return text;
  }

  void seed(int count) {
    execute("WITH RECURSIVE numbers(value) AS (SELECT 0 UNION ALL SELECT "
            "value + 1 FROM numbers WHERE value + 1 < " +
            std::to_string(count) + ") INSERT INTO chart_meta "
            "(path,folder,sha256,md5,title,artist,difficulty,level,keys,"
            "total_notes,total_long_notes,total_scratch_notes,"
            "total_backspin_notes,ln_mode,min_bpm,max_bpm,length,"
            "has_scroll_change,has_bpm_stop) SELECT '" + sqlRoot() +
            "/songs/' || CASE WHEN value % 3 = 0 THEN 'nested/' ELSE '' END || "
            "printf('%06d.bms',value), '" + sqlRoot() +
            "/songs' || CASE WHEN value % 3 = 0 THEN '/nested' ELSE '' END, "
            "printf('%064x',value / 2),printf('%032x',value),"
            "CASE WHEN value % 2 = 0 THEN 'a' ELSE 'A' END || "
            "printf('%06d',value / 5), printf('artist%d',value % 7),"
            "value % 6,value % 12,CASE value % 9 WHEN 0 THEN 7 WHEN 1 THEN 14 "
            "WHEN 2 THEN 9 WHEN 3 THEN 5 WHEN 4 THEN 10 WHEN 5 THEN 24 "
            "WHEN 6 THEN 48 ELSE 0 END, "
            "CASE value % 8 WHEN 0 THEN 249 WHEN 1 THEN 250 WHEN 2 THEN 600 "
            "WHEN 3 THEN 1000 WHEN 4 THEN 2000 ELSE value % 3000 END, "
            "value % 41,value % 101,value % 13,value % 4,120,"
            "120 + value % 3,1000000 * (value % 13),value % 11 = 0,"
            "value % 17 = 0 FROM numbers");
  }

  ChartSelectorQuery query() const {
    return {.recursiveFolder = root / "songs"};
  }
};

template <typename Callable> void expectFailure(Callable callable) {
  bool failed = false;
  try { callable(); } catch (const std::runtime_error &) { failed = true; }
  assert(failed);
}

void compareWithIndex(Fixture &fixture, ChartSelectorQuery query) {
  MusicSelectSongIndex reference("test");
  ChartMetaQuery raw;
  raw.recursiveFolder = query.recursiveFolder;
  raw.rawSongData = true;
  std::vector<ChartMetaRecord> records;
  fixture.session->QueryChartMeta(raw, records);
  for (const auto &record : records) {
    const auto score = query.best
        ? query.best->bestFor(record.meta, query.selectedLongNoteMode) : std::nullopt;
    reference.add(record, score,
        query.clears ? query.clears->bestRankFor(record.meta,
                                                query.selectedLongNoteMode)
                     : score ? score->clearType : kNoClearTypeRank);
  }
  reference.finish();
  const auto resolved = reference.configure(query.modeFilter,
      query.difficultyFilter, query.sortId);
  const auto count = fixture.session->ResolveChartSelectorQuery(query);
  assert(count == reference.size());
  assert(query.modeFilter == resolved.first);
  assert(query.difficultyFilter == resolved.second);
  for (std::size_t offset = 0; offset < count; offset += 7) {
    const auto page = fixture.session->SelectChartSelectorPage(query, offset, 7);
    assert(page.size() == std::min<std::size_t>(7, count - offset));
    for (std::size_t index = 0; index < page.size(); ++index) {
      if (page[index].meta.BmsPath != reference.pathAt(offset + index)) {
        std::cerr << query.sortId << " " << query.modeFilter << " "
                  << query.difficultyFilter << " offset=" << offset + index
                  << " expected=" << reference.pathAt(offset + index)
                  << " actual=" << page[index].meta.BmsPath << '\n';
        assert(false);
      }
    }
  }
  if (count) {
    for (const auto position : {std::size_t{0}, count / 2, count - 1}) {
      const auto identity = reference.idAt(position).value.substr(5);
      assert(fixture.session->FindChartSelectorIndex(query, identity) == position);
    }
  }
  assert(!fixture.session->FindChartSelectorIndex(query, "sha256:missing"));
  assert(fixture.session->SelectChartSelectorPage(query, count, 7).empty());
  assert(fixture.session->SelectChartSelectorPage(query, 0, 0).empty());
}

void differentialQueries() {
  Fixture fixture;
  fixture.seed(200);
  fixture.execute("UPDATE chart_meta SET sha256 = '' WHERE md5 IN "
                  "(printf('%032x',0),printf('%032x',2))");
  fixture.execute("UPDATE chart_meta SET sha256 = 'ABC' WHERE md5 = printf('%032x',4)");
  fixture.execute("UPDATE chart_meta SET sha256 = 'abc' WHERE md5 = printf('%032x',6)");
  fixture.execute("UPDATE chart_meta SET folder = NULL WHERE md5 = printf('%032x',8)");
  fixture.execute("UPDATE chart_meta SET folder = replace(folder,'/','\\') "
                  "WHERE md5 = printf('%032x',10)");
  fixture.execute("INSERT INTO review(sha256,favorite) SELECT sha256,4 "
                  "FROM chart_meta WHERE md5 = printf('%032x',12)");
  auto best = std::make_shared<ScoreBestCache>();
  auto clears = std::make_shared<ScoreClearRankCache>();
  ChartMetaQuery all;
  all.recursiveFolder = fixture.root / "songs";
  all.rawSongData = true;
  std::vector<ChartMetaRecord> records;
  fixture.session->QueryChartMeta(all, records);
  for (std::size_t index = 0; index < records.size(); index += 3) {
    for (int mode = 0; mode < 4; ++mode) {
      ScoreBestSnapshot score;
      score.score = static_cast<int>(index * 2 + mode);
      score.maxScore = index % 5 == 0 ? 1 : 1001;
      if (index % 7 != 0) score.averageJudgeMicros = static_cast<int>(index % 13) - 6;
      if (index % 11 != 0) score.badPoints = index % 17;
      score.lastPlayedUnixSeconds = index % 19;
      score.clearType = static_cast<int>(index % 10) - 1;
      if (!(mode == 2 && index % 5 == 0) && !(mode == 1 && index % 7 == 0)) {
        best->scoreBySha256[records[index].meta.SHA256].snapshots[mode] = score;
      }
      if (!(mode == 2 && index % 7 == 0)) {
        clears->rankBySha256[records[index].meta.SHA256].ranks[mode] = static_cast<int>(index % 10) - 1;
      }
    }
  }
  for (const auto sort : {"TITLE", "ARTIST", "BPM", "LENGTH", "LEVEL",
                          "CLEAR", "SCORE", "MISSCOUNT", "DURATION",
                          "LASTUPDATE", "RIVALCOMPARE_CLEAR",
                          "RIVALCOMPARE_SCORE", "unknown"}) {
    auto query = fixture.query();
    query.sortId = sort;
    query.best = best;
    query.clears = clears;
    for (int mode = 0; mode < 4; ++mode) {
      query.selectedLongNoteMode = mode;
      compareWithIndex(fixture, query);
    }
    query.clears.reset();
    compareWithIndex(fixture, query);
  }
  for (const auto mode : {"ALL", "7KEY", "14KEY", "9KEY", "5KEY",
                          "10KEY", "24KEY", "48KEY", "SINGLE", "DOUBLE",
                          "invalid"}) {
    for (const auto difficulty : {"ALL", "BEGINNER", "NORMAL", "HYPER",
        "ANOTHER", "INSANE", "SCRATCH CHART", "LONG NOTE CHART",
        "SPEED CHANGE CHART", "invalid"}) {
      auto query = fixture.query();
      query.modeFilter = mode;
      query.difficultyFilter = difficulty;
      compareWithIndex(fixture, query);
    }
  }
  fixture.execute("UPDATE chart_meta SET keys=7,total_notes=500,"
                  "total_long_notes=0,total_backspin_notes=0,total_scratch_notes=0,"
                  "min_bpm=120,max_bpm=120,has_scroll_change=0,has_bpm_stop=0");
  auto fallback = fixture.query();
  fallback.modeFilter = "14KEY";
  fallback.difficultyFilter = "INSANE";
  compareWithIndex(fixture, fallback);
  fixture.execute("INSERT OR REPLACE INTO review(sha256,favorite) "
                  "SELECT DISTINCT sha256,8 FROM chart_meta");
  compareWithIndex(fixture, fallback);
  fixture.execute("DELETE FROM chart_meta");
  compareWithIndex(fixture, fallback);
}

void durationOverflowCompatibility() {
  Fixture fixture;
  fixture.seed(30);
  fixture.execute("UPDATE chart_meta SET total_long_notes=0,total_backspin_notes=0");
  auto best = std::make_shared<ScoreBestCache>();
  auto query = fixture.query();
  query.best = best;
  query.sortId = "DURATION";
  std::vector<ChartMetaRecord> records;
  ChartMetaQuery raw;
  raw.recursiveFolder = query.recursiveFolder;
  raw.rawSongData = true;
  fixture.session->QueryChartMeta(raw, records);
  const std::array<std::int64_t, 6> values{
      0, 1, std::numeric_limits<std::int32_t>::max(),
      static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max()) + 1,
      std::numeric_limits<std::int64_t>::min(),
      std::numeric_limits<std::int64_t>::max()};
  for (std::size_t index = 0; index < records.size(); ++index) {
    ScoreBestSnapshot score;
    if (index % 7) score.averageJudgeMicros = values[index % values.size()];
    best->scoreBySha256[records[index].meta.SHA256].snapshots[0] = score;
  }
  auto expectCompatibility = [](auto callable) {
    bool failed = false;
    try { callable(); }
    catch (const ChartSelectorDurationCompatibilityRequired &) { failed = true; }
    assert(failed);
  };
  expectCompatibility([&] { fixture.session->ResolveChartSelectorQuery(query); });
  expectCompatibility([&] { fixture.session->SelectChartSelectorPage(query, 0, 3); });
  expectCompatibility([&] { fixture.session->FindChartSelectorIndex(query, "sha256:x"); });
  query.sortId = "TITLE";
  compareWithIndex(fixture, query);
}

void errorsAndCancellation() {
  Fixture fixture;
  fixture.seed(10);
  auto query = fixture.query();
  std::stop_source cancellation;
  cancellation.request_stop();
  expectFailure([&] { fixture.session->ResolveChartSelectorQuery(query, cancellation.get_token()); });
  expectFailure([&] { fixture.session->SelectChartSelectorPage(query, 0, 1, cancellation.get_token()); });
  expectFailure([&] { fixture.session->FindChartSelectorIndex(query, "sha256:x", cancellation.get_token()); });
  fixture.execute("DROP TABLE chart_meta");
  expectFailure([&] { fixture.session->ResolveChartSelectorQuery(query); });
  expectFailure([&] { fixture.session->SelectChartSelectorPage(query, 0, 1); });
  expectFailure([&] { fixture.session->FindChartSelectorIndex(query, "sha256:x"); });
}

void nullableMetadataAndPathIdentity() {
  Fixture fixture;
  fixture.seed(20);
  fixture.execute("UPDATE chart_meta SET sha256=md5,title=NULL,artist=NULL,"
                  "difficulty=NULL,level=NULL,length=NULL,min_bpm=NULL,max_bpm=NULL,"
                  "keys=NULL,total_notes=NULL WHERE md5=printf('%032x',0)");
  fixture.execute("UPDATE chart_meta SET title='',artist='',difficulty=0,"
                  "level=0,length=0,min_bpm=0,max_bpm=0,total_notes=-10 "
                  "WHERE md5=printf('%032x',2)");
  fixture.execute("UPDATE chart_meta SET title='A' || char(0) || 'b', "
                  "artist='A' || char(0) || 'b' WHERE md5=printf('%032x',4)");
  fixture.execute("UPDATE chart_meta SET title='a' || char(0) || 'z', "
                  "artist='a' || char(0) || 'z' WHERE md5=printf('%032x',6)");
  for (const auto sort : {"TITLE", "ARTIST", "LEVEL", "LENGTH", "BPM"}) {
    auto query = fixture.query();
    query.sortId = sort;
    compareWithIndex(fixture, query);
  }
  fixture.execute("UPDATE chart_meta SET sha256='',md5='',title='',"
                  "path=folder || '/extra/../normalized.bms' WHERE md5=printf('%032x',0)");
  compareWithIndex(fixture, fixture.query());
  auto query = fixture.query();
  fixture.session->ResolveChartSelectorQuery(query);
  const auto first = fixture.session->SelectChartSelectorPage(query, 0, 20);
  for (std::size_t index = 0; index < first.size(); ++index) {
    if (first[index].meta.SHA256.empty()) {
      assert(fixture.session->FindChartSelectorIndex(query,
          "path:" + fspath_to_utf8(first[index].meta.BmsPath.lexically_normal())) == index);
    }
  }
}

void boundedPagesAndQueryLifetime() {
  Fixture fixture;
  fixture.seed(10000);
  auto query = fixture.query();
  assert(fixture.session->ResolveChartSelectorQuery(query) == 5000);
  QueryTrace trace;
  fixture.trace(&trace);
  assert(fixture.session->SelectChartSelectorPage(query, 0, 128).size() == 128);
  assert(fixture.session->SelectChartSelectorPage(query, 4992, 128).size() == 8);
  fixture.trace(nullptr);
  assert(trace.richRows == 136);
  assert(trace.sorts == 0);
  assert(trace.vmSteps < 300000);
  for (const auto &sql : trace.statements) {
    if (sql.starts_with("PRAGMA")) continue;
    const auto plan = repository_test::explainPlan(fixture.sessionDatabase, sql);
    assert(repository_test::planContains(plan, "idx_chart_meta_selector_title"));
    assert(repository_test::planContains(plan, "idx_chart_meta_selector_representative"));
    assert(!repository_test::planContains(plan, "USE TEMP B-TREE FOR ORDER BY"));
  }
  auto best = std::make_shared<ScoreBestCache>();
  query.best = best;
  auto noFunctionRemains = [&] {
    SqliteStatementHandle statement;
    assert(prepareSqliteStatement(fixture.sessionDatabase,
        "SELECT selector_score('',0,0,0)", statement) == SQLITE_ERROR);
  };
  for (const auto sort : {"SCORE", "CLEAR", "DURATION", "TITLE"}) {
    query.sortId = sort;
    fixture.session->ResolveChartSelectorQuery(query);
    noFunctionRemains();
    fixture.session->SelectChartSelectorPage(query, 0, 3);
    noFunctionRemains();
    fixture.session->FindChartSelectorIndex(query, "sha256:missing");
    noFunctionRemains();
    for (int operation = 0; operation < 3; ++operation) {
      std::stop_source cancellation;
      trace.cancel = &cancellation;
      fixture.trace(&trace);
      expectFailure([&] {
        if (operation == 0) fixture.session->ResolveChartSelectorQuery(query, cancellation.get_token());
        if (operation == 1) fixture.session->SelectChartSelectorPage(query, 0, 3, cancellation.get_token());
        if (operation == 2) fixture.session->FindChartSelectorIndex(query, "sha256:missing", cancellation.get_token());
      });
      fixture.trace(nullptr);
      noFunctionRemains();
      fixture.session->SelectChartSelectorPage(query, 0, 3);
      noFunctionRemains();
    }
  }
  fixture.execute("UPDATE chart_meta SET folder='" + fixture.sqlRoot() +
                  "/leaf' WHERE md5=printf('%032x',9999)");
  query = {.recursiveFolder = fixture.root / "leaf"};
  QueryTrace narrow;
  fixture.trace(&narrow);
  assert(fixture.session->ResolveChartSelectorQuery(query) == 1);
  assert(fixture.session->SelectChartSelectorPage(query, 0, 128).size() == 1);
  fixture.trace(nullptr);
  assert(narrow.richRows == 1);
  assert(narrow.vmSteps < 20000);
}

void benchmark() {
  Fixture fixture;
  fixture.seed(100000);
  for (int iteration = 0; iteration < 3; ++iteration) {
    fixture.reopen();
    auto query = fixture.query();
    const auto begin = std::chrono::steady_clock::now();
    const auto count = fixture.session->ResolveChartSelectorQuery(query);
    const auto counted = std::chrono::steady_clock::now();
    const auto first = fixture.session->SelectChartSelectorPage(query, 0, 128);
    const auto paged = std::chrono::steady_clock::now();
    const auto wrapped = fixture.session->SelectChartSelectorPage(query, count - 128, 128);
    const auto finished = std::chrono::steady_clock::now();
    assert(count == 50000 && first.size() == 128 && wrapped.size() == 128);
    auto milliseconds = [](auto duration) {
      return std::chrono::duration<double, std::milli>(duration).count();
    };
    std::cout << "SQLite 100k connection-cold/os-warm iteration=" << iteration
              << " count_ms=" << milliseconds(counted - begin)
              << " first128_ms=" << milliseconds(paged - counted)
              << " wrapped128_ms=" << milliseconds(finished - paged)
              << " first_view_ms=" << milliseconds(finished - begin) << '\n';
  }
  QueryTrace trace;
  fixture.trace(&trace);
  auto query = fixture.query();
  fixture.session->ResolveChartSelectorQuery(query);
  fixture.session->SelectChartSelectorPage(query, 0, 128);
  fixture.trace(nullptr);
  assert(trace.richRows == 128);
  for (const auto &sql : trace.statements) {
    for (const auto &detail : repository_test::explainPlan(fixture.sessionDatabase, sql)) {
      std::cout << "PLAN " << detail << '\n';
    }
  }
  std::cout << "SQL sorts=" << trace.sorts << " rich_rows=" << trace.richRows << '\n';
  fixture.execute("UPDATE chart_meta SET folder='" + fixture.sqlRoot() +
                  "/leaf' WHERE md5>=printf('%032x',99900)");
  query = {.recursiveFolder = fixture.root / "leaf"};
  const auto begin = std::chrono::steady_clock::now();
  assert(fixture.session->ResolveChartSelectorQuery(query) == 50);
  const auto counted = std::chrono::steady_clock::now();
  assert(fixture.session->SelectChartSelectorPage(query, 0, 128).size() == 50);
  const auto paged = std::chrono::steady_clock::now();
  std::cout << "SQLite 100k narrow-100-rows-50-SHA-folder count_ms="
            << std::chrono::duration<double, std::milli>(counted - begin).count()
            << " first128_ms="
            << std::chrono::duration<double, std::milli>(paged - counted).count() << '\n';
}

}

int main(int argc, char **) {
  differentialQueries();
  durationOverflowCompatibility();
  errorsAndCancellation();
  nullableMetadataAndPathIdentity();
  boundedPagesAndQueryLifetime();
  if (argc > 1) benchmark();
  std::cout << "chart_selector_query_tests passed\n";
}
