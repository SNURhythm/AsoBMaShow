#include "../src/repositories/ChartRepository.h"
#include "../src/LongNoteModeUtils.h"
#include "../src/repositories/ChartStorageIdentity.h"
#include "../src/repositories/ScoreCacheQueries.h"
#include "../src/repositories/ScoreRepository.h"
#include "../src/repositories/SqliteRAII.h"
#include "../src/targets.h"
#include "RepositorySqliteTestSupport.h"

#include <array>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

namespace {

static_assert(std::is_move_constructible_v<ChartRepository::Session>);
static_assert(std::is_move_assignable_v<ChartRepository::Session>);
static_assert(!std::is_copy_constructible_v<ChartRepository::Session>);
static_assert(!std::is_copy_assignable_v<ChartRepository::Session>);
static_assert(std::is_move_constructible_v<ChartRepository::Session::ScanBatch>);
static_assert(std::is_move_assignable_v<ChartRepository::Session::ScanBatch>);
static_assert(
    !std::is_copy_constructible_v<ChartRepository::Session::ScanBatch>);
static_assert(!std::is_copy_assignable_v<ChartRepository::Session::ScanBatch>);

class TempDirectory {
public:
  TempDirectory() {
    static std::atomic<unsigned long long> sequence{0};
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("asobmashow-chart-repository-" + std::to_string(nonce) + "-" +
             std::to_string(sequence.fetch_add(1)));
    std::filesystem::create_directories(path_);
  }

  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

struct DatabaseCloser {
  void operator()(sqlite3 *database) const {
    closeSqliteDatabase(database);
  }
};
using Database = std::unique_ptr<sqlite3, DatabaseCloser>;

Database openDatabase(const std::filesystem::path &path) {
  std::filesystem::create_directories(path.parent_path());
  std::string error;
  return Database(openSqliteDatabase(path, error));
}

bool execute(sqlite3 *database, const std::string &sql) {
  return !executeSqlite(database, sql.c_str()).has_value();
}

int queryInt(sqlite3 *database, const char *sql) {
  SqliteStatementHandle statement;
  assert(prepareSqliteStatement(database, sql, statement) == SQLITE_OK);
  assert(sqlite3_step(statement.get()) == SQLITE_ROW);
  return sqlite3_column_int(statement.get(), 0);
}

std::string queryString(sqlite3 *database, const char *sql) {
  SqliteStatementHandle statement;
  assert(prepareSqliteStatement(database, sql, statement) == SQLITE_OK);
  assert(sqlite3_step(statement.get()) == SQLITE_ROW);
  return sqliteColumnString(statement.get(), 0);
}

void seedChartScore(const std::filesystem::path &path,
                    std::string_view chartPath, std::string_view chartMd5,
                    std::string_view chartSha256, int longNoteMode,
                    int clearRank, int score) {
  Database database = openDatabase(path);
  assert(database);
  assert(execute(
      database.get(),
      "INSERT INTO scores (chart_path,chart_md5,chart_sha256,ln_mode,"
      "chart_title,chart_artist,score,max_score,max_combo,"
      "combo_break,pgreat,great,good,bad,poor,kpoor,fast,slow,"
      "final_gauge,clear_type) VALUES ('" +
          std::string(chartPath) + "','" + std::string(chartMd5) + "','" +
          std::string(chartSha256) + "'," + std::to_string(longNoteMode) +
          ",'Chart','Artist'," + std::to_string(score) +
          ",1000,50,1,10,9,8,7,6,5,4,3,0.75," +
          std::to_string(clearRank) + ")"));
}

std::atomic<int> *connectionCount = nullptr;
struct ScanBatchSqlObservation {
  std::atomic<int> chartMetaInsertPrepares{0};
  std::atomic<int> begins{0};
  std::atomic<int> commits{0};
  std::mutex mutex;
  std::vector<std::string> chartMetaInsertExecutions;
};
ScanBatchSqlObservation *scanBatchSqlObservation = nullptr;
std::atomic<int> chartMetadataReleasesToDeny{0};
std::mutex traceMutex;
std::vector<std::string> tracedStatements;

int traceStatement(unsigned mask, void *, void *statement, void *) {
  if ((mask & SQLITE_TRACE_STMT) == 0 || statement == nullptr) {
    return 0;
  }
  const char *sql = sqlite3_sql(static_cast<sqlite3_stmt *>(statement));
  const std::string_view sqlText = sql != nullptr ? sql : "";
  if (scanBatchSqlObservation != nullptr) {
    if (sqlText.starts_with("BEGIN")) {
      scanBatchSqlObservation->begins.fetch_add(1, std::memory_order_relaxed);
    } else if (sqlText.starts_with("COMMIT")) {
      scanBatchSqlObservation->commits.fetch_add(1,
                                                  std::memory_order_relaxed);
    } else if (sqlText.starts_with("REPLACE INTO chart_meta")) {
      std::lock_guard observationLock(scanBatchSqlObservation->mutex);
      scanBatchSqlObservation->chartMetaInsertExecutions.emplace_back(sqlText);
    }
  }
  std::lock_guard lock(traceMutex);
  tracedStatements.emplace_back(sqlText);
  return 0;
}

int observeAuthorization(void *, int action, const char *first,
                         const char *second,
                         const char *, const char *) {
  if (action == SQLITE_SAVEPOINT && first != nullptr && second != nullptr &&
      std::string_view(first) == "RELEASE" &&
      std::string_view(second) == "chart_metadata_rebuild_migration") {
    int remaining = chartMetadataReleasesToDeny.load(std::memory_order_relaxed);
    while (remaining > 0 &&
           !chartMetadataReleasesToDeny.compare_exchange_weak(
               remaining, remaining - 1, std::memory_order_relaxed)) {
    }
    if (remaining > 0) {
      return SQLITE_DENY;
    }
  }
  if (scanBatchSqlObservation != nullptr && action == SQLITE_INSERT &&
      first != nullptr && std::string_view(first) == "chart_meta") {
    scanBatchSqlObservation->chartMetaInsertPrepares.fetch_add(
        1, std::memory_order_relaxed);
  }
  return SQLITE_OK;
}

int observeConnection(sqlite3 *database, char **,
                      const sqlite3_api_routines *) {
  assert(connectionCount != nullptr);
  connectionCount->fetch_add(1, std::memory_order_relaxed);
  sqlite3_trace_v2(database, SQLITE_TRACE_STMT, traceStatement, nullptr);
  sqlite3_set_authorizer(database, observeAuthorization, nullptr);
  return SQLITE_OK;
}

class ScopedConnectionObserver {
public:
  explicit ScopedConnectionObserver(
      std::atomic<int> &count,
      ScanBatchSqlObservation *scanObservation = nullptr,
      int deniedChartMetadataReleases = 0) {
    assert(connectionCount == nullptr);
    assert(scanBatchSqlObservation == nullptr);
    connectionCount = &count;
    scanBatchSqlObservation = scanObservation;
    chartMetadataReleasesToDeny.store(deniedChartMetadataReleases,
                                      std::memory_order_relaxed);
    {
      std::lock_guard lock(traceMutex);
      tracedStatements.clear();
    }
    sqlite3_reset_auto_extension();
    assert(sqlite3_auto_extension(
               reinterpret_cast<void (*)()>(observeConnection)) == SQLITE_OK);
  }

  ~ScopedConnectionObserver() {
    sqlite3_reset_auto_extension();
    connectionCount = nullptr;
    scanBatchSqlObservation = nullptr;
    chartMetadataReleasesToDeny.store(0, std::memory_order_relaxed);
  }
};

bool traced(std::string_view expected) {
  std::lock_guard lock(traceMutex);
  for (const auto &statement : tracedStatements) {
    if (statement.find(expected) != std::string::npos) {
      return true;
    }
  }
  return false;
}

std::string tracedStatementContaining(std::string_view expected) {
  std::lock_guard lock(traceMutex);
  for (const auto &statement : tracedStatements) {
    if (statement.find(expected) != std::string::npos) {
      return statement;
    }
  }
  return {};
}

bms_parser::ChartMeta chartMeta(const std::filesystem::path &root) {
  bms_parser::ChartMeta meta;
  meta.BmsPath = root / "chart.bms";
  meta.Folder = root;
  meta.MD5 = "11111111111111111111111111111111";
  meta.SHA256 =
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  meta.Title = "Session Chart";
  meta.Artist = "Repository Test";
  meta.TotalNotes = 100;
  return meta;
}

void testScanBatchCommitAndRollback() {
  TempDirectory temporary;
  ChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());

  auto meta = chartMeta(temporary.path());
  const ChartScanCheckpoint checkpoint{
      .found = true,
      .scanSignature = "repository-test",
      .phase = "individual",
      .nextIndex = 1,
      .lastPath = meta.BmsPath,
  };
  auto batch = session->BeginScanBatch();
  assert(batch.has_value());
  assert(batch->UpsertChart(meta, std::nullopt));
  assert(batch->CheckpointAndContinue(checkpoint));
  assert(batch->Commit());
  assert(session->CountAllChartMeta() == 1);

  auto rollback = session->BeginScanBatch();
  assert(rollback.has_value());
  assert(rollback->DeleteChart(meta.BmsPath));
  rollback.reset();
  assert(session->CountAllChartMeta() == 1);
}

void testScanBatchRetainsSessionStorage() {
  TempDirectory temporary;
  ChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());
  auto batch = session->BeginScanBatch();
  assert(batch.has_value());
  session.reset();

  auto meta = chartMeta(temporary.path());
  assert(batch->UpsertChart(meta, std::nullopt));
  assert(batch->Commit());

  auto verification = repository.OpenSession();
  assert(verification.has_value());
  assert(verification->CountAllChartMeta() == 1);
}

void testScanBatchReusesPreparedInsertAndTransaction() {
  TempDirectory temporary;
  std::atomic<int> connections{0};
  ScanBatchSqlObservation observation;
  ScopedConnectionObserver observer(connections, &observation);

  ChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());

  const int prepareBaseline =
      observation.chartMetaInsertPrepares.load(std::memory_order_relaxed);
  const int beginBaseline = observation.begins.load(std::memory_order_relaxed);
  const int commitBaseline =
      observation.commits.load(std::memory_order_relaxed);
  std::size_t executionBaseline = 0;
  {
    std::lock_guard lock(observation.mutex);
    executionBaseline = observation.chartMetaInsertExecutions.size();
  }

  auto batch = session->BeginScanBatch();
  assert(batch.has_value());
  for (int i = 0; i < 100; ++i) {
    auto meta = chartMeta(temporary.path());
    meta.BmsPath = temporary.path() / ("batch-" + std::to_string(i) + ".bms");
    assert(batch->UpsertChart(meta, std::nullopt));
  }
  assert(batch->ChangedCount() == 100);
  assert(batch->Commit());

  assert(observation.chartMetaInsertPrepares.load(std::memory_order_relaxed) -
             prepareBaseline ==
         1);
  assert(observation.begins.load(std::memory_order_relaxed) - beginBaseline ==
         1);
  assert(observation.commits.load(std::memory_order_relaxed) -
             commitBaseline ==
         1);
  {
    std::lock_guard lock(observation.mutex);
    assert(observation.chartMetaInsertExecutions.size() - executionBaseline ==
           100);
    const std::string &expected =
        observation.chartMetaInsertExecutions[executionBaseline];
    for (std::size_t i = executionBaseline;
         i < observation.chartMetaInsertExecutions.size(); ++i) {
      assert(observation.chartMetaInsertExecutions[i] == expected);
    }
  }
  assert(session->CountAllChartMeta() == 100);
}

void testSessionRoundTripAndReadinessCost() {
  TempDirectory temporary;
  const auto path = temporary.path() / "chart.db";
  std::atomic<int> connections{0};
  ScopedConnectionObserver observer(connections);

  ChartRepository repository(path);
  assert(repository.DatabasePath() == path);
  assert(repository.EnsureReady());
  const int readyConnections = connections.load(std::memory_order_relaxed);
  assert(readyConnections > 0);
  assert(repository.EnsureReady());
  assert(connections.load(std::memory_order_relaxed) == readyConnections);
  assert(traced("PRAGMA synchronous=NORMAL"));

  auto first = repository.OpenSession();
  auto second = repository.OpenSession();
  assert(first.has_value());
  assert(second.has_value());
  auto meta = chartMeta(temporary.path());
  assert(first->InsertChartMeta(meta));
  assert(first->CountAllChartMeta() == 1);
  assert(second->CountAllChartMeta() == 1);

  ChartMetaQuery query;
  query.limit = 64;
  std::vector<ChartMetaRecord> page;
  first->QueryChartMeta(query, page);
  assert(page.size() == 1);
  assert(first->FindChartMetaIndex(query, meta.BmsPath) == 0);
  first.reset();
  second.reset();

  const int beforeShortSessions =
      connections.load(std::memory_order_relaxed);
  for (int i = 0; i < 10; ++i) {
    auto session = repository.OpenSession();
    assert(session.has_value());
  }
  assert(connections.load(std::memory_order_relaxed) ==
         beforeShortSessions + 10);

  Database inspection = openDatabase(path);
  assert(inspection);
  assert(queryInt(inspection.get(), "PRAGMA user_version") == 4);
  SqliteStatementHandle journalMode;
  assert(prepareSqliteStatement(inspection.get(), "PRAGMA journal_mode",
                                journalMode) == SQLITE_OK);
  assert(sqlite3_step(journalMode.get()) == SQLITE_ROW);
  assert(sqliteColumnString(journalMode.get(), 0) == "wal");
}

void testSelectChartMetaByPathsHydratesInInputOrder() {
  TempDirectory temporary;
  const auto databasePath = temporary.path() / "chart.db";
  ChartRepository repository(databasePath);
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());

  auto first = chartMeta(temporary.path());
  first.BmsPath = temporary.path() / "first.bms";
  first.StageFile = "first-stage.png";
  first.Title = "First title";
  first.SubTitle = "First subtitle";
  first.Artist = "First artist";
  first.KeyMode = 7;
  first.TotalNotes = 701;
  first.MD5 = "11111111111111111111111111111111";
  first.SHA256 =
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  auto second = chartMeta(temporary.path());
  second.BmsPath = temporary.path() / "second.bms";
  second.StageFile = "second-stage.png";
  second.Title = "Second title";
  second.SubTitle = "Second subtitle";
  second.Artist = "Second artist";
  second.KeyMode = 14;
  second.TotalNotes = 1402;
  second.MD5 = "22222222222222222222222222222222";
  second.SHA256 =
      "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  assert(session->InsertChartMeta(first));
  assert(session->InsertChartMeta(second));

  const auto missing = temporary.path() / "missing.bms";
  const std::array requestedPaths{first.BmsPath, second.BmsPath,
                                  first.BmsPath, missing};
  const auto loaded = session->SelectChartMetaByPaths(requestedPaths);
  assert(loaded.status == ChartMetaPathBatchReadStatus::Loaded);
  assert(loaded.records.size() == 2);
  assert(loaded.records[0].meta.BmsPath == first.BmsPath);
  assert(loaded.records[0].meta.StageFile == first.StageFile);
  assert(loaded.records[0].meta.Title == first.Title);
  assert(loaded.records[0].meta.SubTitle == first.SubTitle);
  assert(loaded.records[0].meta.Artist == first.Artist);
  assert(loaded.records[0].meta.KeyMode == first.KeyMode);
  assert(loaded.records[0].meta.MD5 == first.MD5);
  assert(loaded.records[0].meta.SHA256 == first.SHA256);
  assert(loaded.records[0].meta.TotalNotes == first.TotalNotes);
  assert(loaded.records[1].meta.BmsPath == second.BmsPath);
  assert(loaded.records[1].meta.StageFile == second.StageFile);
  assert(loaded.records[1].meta.TotalNotes == second.TotalNotes);
  assert(loaded.missingPaths == 1);

  const auto empty = session->SelectChartMetaByPaths({});
  assert(empty.status == ChartMetaPathBatchReadStatus::Loaded);
  assert(empty.records.empty());
  assert(empty.missingPaths == 0);

  std::vector<std::filesystem::path> oversizedPaths;
  oversizedPaths.reserve(16'385);
  for (int index = 0; index < 16'385; ++index) {
    oversizedPaths.push_back(temporary.path() /
                             ("oversized-" + std::to_string(index) + ".bms"));
  }
  const auto oversized = session->SelectChartMetaByPaths(oversizedPaths);
  assert(oversized.status == ChartMetaPathBatchReadStatus::Invalid);
  assert(oversized.records.empty());

  Database database = openDatabase(databasePath);
  assert(database);
  assert(execute(database.get(), "DROP TABLE chart_meta"));
  const std::array failurePath{first.BmsPath};
  const auto storageFailure = session->SelectChartMetaByPaths(failurePath);
  assert(storageFailure.status == ChartMetaPathBatchReadStatus::StorageFailure);
  assert(storageFailure.records.empty());
}

void testSelectChartMetaByHashUsesDurableIndexedIdentity() {
  TempDirectory temporary;
  ChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());

  const std::string shaA(64, 'a');
  const std::string shaB(64, 'b');
  const std::string md5A(32, '1');
  const std::string md5B(32, '2');

  auto second = chartMeta(temporary.path());
  second.BmsPath = temporary.path() / "z-second.bms";
  second.SHA256 = shaA;
  second.MD5 = md5A;
  auto first = second;
  first.BmsPath = temporary.path() / "a-first.bms";
  auto md5Fallback = chartMeta(temporary.path());
  md5Fallback.BmsPath = temporary.path() / "md5-fallback.bms";
  md5Fallback.SHA256 = shaB;
  md5Fallback.MD5 = md5B;
  assert(session->InsertChartMeta(second));
  assert(session->InsertChartMeta(first));
  assert(session->InsertChartMeta(md5Fallback));

  const auto shaMatches = session->SelectChartMetaByHash(
      "  " + std::string(64, 'A') + "\n", md5B);
  assert(shaMatches.size() == 2);
  assert(shaMatches[0].BmsPath == first.BmsPath);
  assert(shaMatches[1].BmsPath == second.BmsPath);

  const auto md5Matches = session->SelectChartMetaByHash({},
                                                         " " + md5B + " ");
  assert(md5Matches.size() == 1);
  assert(md5Matches.front().BmsPath == md5Fallback.BmsPath);

  assert(session->SelectChartMetaByHash(std::string(64, 'c'), md5B).empty());
  assert(session->SelectChartMetaByHash("invalid", "also-invalid").empty());
  assert(session->SelectChartMetaByHash({}, {}).empty());
}

void testRejectedFamiliesRemainUnchanged() {
  TempDirectory temporary;

  const auto futurePath = temporary.path() / "future" / "chart.db";
  {
    Database database = openDatabase(futurePath);
    assert(database);
    assert(execute(database.get(),
                   "CREATE TABLE sentinel(value TEXT);"
                   "INSERT INTO sentinel VALUES('unchanged');"
                   "PRAGMA user_version=5"));
  }
  const auto futureBefore =
      repository_test::rawDatabaseFamilySnapshot(futurePath);
  ChartRepository future(futurePath);
  assert(!future.EnsureReady());
  assert(repository_test::rawDatabaseFamilySnapshot(futurePath) ==
         futureBefore);

  const auto corruptPath = temporary.path() / "corrupt" / "chart.db";
  std::filesystem::create_directories(corruptPath.parent_path());
  {
    std::array<char, 100> bytes{};
    constexpr std::string_view header("SQLite format 3\0", 16);
    std::copy(header.begin(), header.end(), bytes.begin());
    std::ofstream output(corruptPath, std::ios::binary);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }
  const auto corruptBefore =
      repository_test::rawDatabaseFamilySnapshot(corruptPath);
  ChartRepository corrupt(corruptPath);
  assert(!corrupt.EnsureReady());
  assert(repository_test::rawDatabaseFamilySnapshot(corruptPath) ==
         corruptBefore);
}

void testChartQueryBehaviorMatrix() {
  TempDirectory temporary;
  const auto chartPath = temporary.path() / "chart.db";
  const auto scorePath = temporary.path() / "score.db";
  ChartRepository charts(chartPath);
  assert(charts.EnsureReady());
  ScoreRepository scores(scorePath);
  scores.SetChartDatabasePath(chartPath);
  assert(scores.EnsureSchema());

  constexpr std::string_view md5A = "11111111111111111111111111111111";
  constexpr std::string_view md5B = "22222222222222222222222222222222";
  constexpr std::string_view md5C = "33333333333333333333333333333333";
  constexpr std::string_view md5D = "44444444444444444444444444444444";
  constexpr std::string_view shaA =
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  constexpr std::string_view shaB =
      "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  constexpr std::string_view shaC =
      "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
  constexpr std::string_view shaD =
      "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";

  seedChartScore(scorePath, "alpha.bms", md5A, shaA, 1, 1, 100);
  seedChartScore(scorePath, "beta.bms", md5B, shaB, 1, 3, 300);
  seedChartScore(scorePath, "gamma.bms", md5C, shaC, 1, 2, 200);
  {
    Database database = openDatabase(chartPath);
    assert(database);
    assert(execute(
        database.get(),
        "INSERT INTO chart_meta(path,md5,sha256,title,subtitle,genre,"
        "artist,sub_artist,level,bpm,min_bpm,max_bpm,ln_mode,"
        "total_long_notes,total_backspin_notes,source_priority,"
        "source_archive_size) VALUES"
        "('alpha.bms','" +
            std::string(md5A) + "','" + std::string(shaA) +
            "','Alpha','','','Artist A','',2,120,120,120,0,1,0,0,0),"
            "('beta.bms','" +
            std::string(md5B) + "','" + std::string(shaB) +
            "','Beta','','','Artist B','',7,180,180,180,0,1,0,0,0),"
            "('gamma.bms','" +
            std::string(md5C) + "','" + std::string(shaC) +
            "','Gamma','','','Artist C','',12,240,240,240,0,1,0,0,0),"
            "('delta.bms','" +
            std::string(md5D) + "','" + std::string(shaD) +
            "','Delta','','','Artist D','',9,90,90,90,0,1,0,0,0);"
            "INSERT INTO chart_favorites(chart_path,chart_md5,"
            "chart_sha256) VALUES('gamma.bms','" +
            std::string(md5C) + "','" + std::string(shaC) + "')"));
  }

  const ir::IrRemoteScore remoteOnly{
      .remoteUserId = 42,
      .game = "bms-7k",
      .remoteScoreId = "remote-only-hard",
      .remoteChartId = "remote-delta",
      .chartMd5 = std::string(md5D),
      .chartSha256 = std::string(shaD),
      .title = "Delta",
      .artist = "Artist D",
      .service = "Bokutachi",
      .noteCount = 100,
      .score = 180,
      .lampRank = kClearTypeHardClearRank,
      .timeAddedUnixMillis = 1'000,
  };
  assert(scores
             .ReplaceImportedIrScores("tachi", "https://boku.tachi.ac", 1,
                                      std::span{&remoteOnly, 1})
             .status == ImportedIrScoreProjectionStatus::Applied);

  auto session = charts.OpenSession(&scores);
  assert(session.has_value());
  const auto checkQuery = [&](const ChartMetaQuery &query,
                              const std::vector<std::string> &expectedPaths,
                              int expectedCount, int expectedStartIndex) {
    std::vector<ChartMetaRecord> records;
    session->QueryChartMeta(query, records);
    std::vector<std::string> actualPaths;
    actualPaths.reserve(records.size());
    for (const auto &record : records) {
      actualPaths.push_back(
          chart_storage_identity::StoredPathText(record.meta.BmsPath));
    }
    if (actualPaths != expectedPaths) {
      std::cerr << "Unexpected chart query paths:";
      for (const auto &path : actualPaths) {
        std::cerr << ' ' << path;
      }
      std::cerr << std::endl;
    }
    assert(actualPaths == expectedPaths);
    assert(session->CountChartMeta(query) == expectedCount);
    for (std::size_t i = 0; i < records.size(); ++i) {
      assert(session->FindChartMetaIndex(query, records[i].meta.BmsPath) ==
             expectedStartIndex + static_cast<int>(i));
    }
  };

  ChartMetaQuery query;
  checkQuery(query,
             {"alpha.bms", "beta.bms", "delta.bms", "gamma.bms"}, 4,
             0);

  query.limit = 1;
  query.offset = 1;
  checkQuery(query, {"beta.bms"}, 4, 1);

  query = {};
  query.keyword = "Artist B";
  checkQuery(query, {"beta.bms"}, 1, 0);

  query = {};
  query.bpmMin = 170.0;
  query.bpmMax = 200.0;
  checkQuery(query, {"beta.bms"}, 1, 0);

  query = {};
  query.favoritesOnly = true;
  checkQuery(query, {"gamma.bms"}, 1, 0);

  query = {};
  query.clearMarkFilter = true;
  query.clearMarkRank = 3;
  query.selectedLongNoteMode = 1;
  checkQuery(query, {"beta.bms"}, 1, 0);

  query = {};
  query.sortCriterion = ChartRecordSortCriterion::Score;
  query.sortDirection = ChartRecordSortDirection::Descending;
  query.selectedLongNoteMode = 1;
  checkQuery(query,
             {"beta.bms", "gamma.bms", "delta.bms", "alpha.bms"}, 4,
             0);

  chart_library::FolderClearDataByLongNoteMode folderData;
  {
    auto prepared = scores.PrepareScoreQueryDatabase(*session);
    assert(!prepared.error().has_value());
    const auto projectedClearRanks = scores.LoadBestClearRanks(
        *session, score_cache_queries::kScoreDatabaseSchema);
    const auto localClearRanks = scores.LoadLocalBestClearRanks(
        *session, score_cache_queries::kScoreDatabaseSchema);
    folderData = session->LoadFolderClearDataByLongNoteMode(
        projectedClearRanks, localClearRanks);
  }
  ChartMetaQuery hardQuery;
  hardQuery.clearMarkFilter = true;
  hardQuery.clearMarkRank = kClearTypeHardClearRank;
  hardQuery.selectedLongNoteMode = 1;
  checkQuery(hardQuery, {"delta.bms"}, 1, 0);
  const auto &allCounts =
      folderData.clearMarkCounts[long_note_mode::kLnValue].at("all");
  const auto hardCount = allCounts.find(kClearTypeHardClearRank);
  assert(hardCount != allCounts.end() && hardCount->second == 1);
}

void testExactFolderQuery() {
  TempDirectory temporary;
  std::atomic<int> connections{0};
  ScopedConnectionObserver observer(connections);
  const auto chartPath = temporary.path() / "chart.db";
  ChartRepository charts(chartPath);
  assert(charts.EnsureReady());

  {
    Database database = openDatabase(chartPath);
    assert(database);
    assert(execute(
        database.get(),
        "INSERT INTO chart_meta(path,md5,sha256,title,subtitle,genre,artist,"
        "sub_artist,folder,level,source_priority,source_archive_size) VALUES"
        "('library/A/one.bms','md5-one','sha-one','One','','','','',"
        "'library/A',1,0,0),"
        "('library/A/two.bms','md5-two','sha-two','Two','','','','',"
        "'library/A',2,0,0),"
        "('library/A/no-folder.bms','md5-no-folder','sha-no-folder',"
        "'No Folder','','','','','',8,0,0),"
        "('library/A/nested/three.bms','md5-three','sha-three','Three','','',"
        "'','','library/A/nested',3,0,0),"
        "('library/A/nested/no-folder.bms','md5-nested-no-folder',"
        "'sha-nested-no-folder','Nested No Folder','','','','','',9,0,0),"
        "('library/B/four.bms','md5-four','sha-four','Four','','','','',"
        "'library/B',4,0,0),"
        "('packs/pack.zip/A/five.bms','md5-five','sha-five','Five','','',"
        "'','','packs/pack.zip/A',5,0,0),"
        "('packs/pack.zip/A/six.bms','md5-six','sha-six','Six','','','','',"
        "'packs/pack.zip/A',6,0,0),"
        "('packs/pack.zip/A/no-folder.bms','md5-archive-no-folder',"
        "'sha-archive-no-folder','Archive No Folder','','','','','',10,0,0),"
        "('packs/pack.zip/B/seven.bms','md5-seven','sha-seven','Seven','','',"
        "'','','packs/pack.zip/B',7,0,0),"
        "('C:\\library\\A\\windows.bms','md5-windows','sha-windows',"
        "'Windows','','','','','',11,0,0),"
        "('C:\\library\\A\\nested\\deep.bms','md5-windows-nested',"
        "'sha-windows-nested','Windows Nested','','','','','',12,0,0)"));
  }

  auto session = charts.OpenSession();
  assert(session.has_value());
  auto aliased = chartMeta("library/C/../C");
  aliased.BmsPath = "library/C/aliased.bms";
  aliased.MD5 = "md5-aliased";
  aliased.SHA256 = "sha-aliased";
  aliased.Title = "Aliased";
  assert(session->InsertChartMeta(aliased));
  auto trailing = chartMeta("library/C/");
  trailing.BmsPath = "library/C/trailing.bms";
  trailing.MD5 = "md5-trailing";
  trailing.SHA256 = "sha-trailing";
  trailing.Title = "Trailing";
  assert(session->InsertChartMeta(trailing));
  const auto queryPaths = [&](const ChartMetaQuery &query) {
    std::vector<ChartMetaRecord> records;
    session->QueryChartMeta(query, records);
    std::vector<std::string> paths;
    paths.reserve(records.size());
    for (const auto &record : records) {
      paths.push_back(
          chart_storage_identity::StoredPathText(record.meta.BmsPath));
    }
    return paths;
  };

  ChartMetaQuery query;
  query.exactFolder = std::filesystem::path("library/A");
  assert(queryPaths(query) ==
         std::vector<std::string>({"library/A/no-folder.bms",
                                   "library/A/one.bms",
                                   "library/A/two.bms"}));
  assert(session->CountChartMeta(query) == 3);
  assert(session->FindChartMetaIndex(query, "library/A/no-folder.bms") == 0);
  assert(session->FindChartMetaIndex(query, "library/A/one.bms") == 1);
  assert(session->FindChartMetaIndex(query, "library/A/two.bms") == 2);
  assert(session->FindChartMetaIndex(query,
                                     "library/A/nested/three.bms") == -1);
  assert(session->FindChartMetaIndex(
             query, "library/A/nested/no-folder.bms") == -1);

  query.limit = 1;
  query.offset = 1;
  assert(queryPaths(query) ==
         std::vector<std::string>({"library/A/one.bms"}));
  assert(session->CountChartMeta(query) == 3);

  query = {};
  query.exactFolder = std::filesystem::path("library/A/nested");
  assert(queryPaths(query) ==
         std::vector<std::string>({"library/A/nested/no-folder.bms",
                                   "library/A/nested/three.bms"}));
  assert(session->CountChartMeta(query) == 2);

  query = {};
  query.exactFolder = std::filesystem::path("packs/pack.zip/A");
  assert(queryPaths(query) ==
         std::vector<std::string>({"packs/pack.zip/A/no-folder.bms",
                                   "packs/pack.zip/A/five.bms",
                                   "packs/pack.zip/A/six.bms"}));
  assert(session->CountChartMeta(query) == 3);
  assert(session->FindChartMetaIndex(query,
                                     "packs/pack.zip/B/seven.bms") == -1);

  query.sortCriterion = ChartRecordSortCriterion::Title;
  query.sortDirection = ChartRecordSortDirection::Descending;
  assert(queryPaths(query) ==
         std::vector<std::string>({"packs/pack.zip/A/six.bms",
                                   "packs/pack.zip/A/five.bms",
                                   "packs/pack.zip/A/no-folder.bms"}));
  assert(session->FindChartMetaIndex(query,
                                     "packs/pack.zip/A/five.bms") == 1);

  query = {};
  query.exactFolder = std::filesystem::path(R"(C:\library\A)");
  assert(queryPaths(query) ==
         std::vector<std::string>({R"(C:\library\A\windows.bms)"}));
  assert(session->CountChartMeta(query) == 1);
  assert(session->FindChartMetaIndex(
             query, std::filesystem::path(R"(C:\library\A\windows.bms)")) ==
         0);
  assert(session->FindChartMetaIndex(
             query,
             std::filesystem::path(R"(C:\library\A\nested\deep.bms)")) ==
         -1);

  query = {};
  query.exactFolder = std::filesystem::path("library/C");
  assert(queryPaths(query) ==
         std::vector<std::string>({"library/C/aliased.bms",
                                   "library/C/trailing.bms"}));
  assert(session->CountChartMeta(query) == 2);

  assert(!traced("chart_normalize_stored_folder(cm.folder)"));
  assert(traced("cm.folder = @exact_folder"));

  Database database = openDatabase(chartPath);
  assert(database);
  const std::string countSql = tracedStatementContaining(
      "SELECT COUNT(*) FROM chart_meta cm WHERE 1 = 1 AND (cm.folder = "
      "@exact_folder");
  assert(!countSql.empty());
  const auto plan = repository_test::explainPlan(database.get(), countSql);
  assert(repository_test::planContains(plan, "MULTI-INDEX OR"));
  assert(repository_test::planContains(plan, "idx_chart_meta_folder"));
  assert(!repository_test::planContains(plan, "SCAN cm"));
}

void testChartMigrationCompatibilityMatrix() {
  constexpr std::string_view lowerMd5 =
      "abcdefabcdefabcdefabcdefabcdefab";
  constexpr std::string_view lowerSha =
      "abcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcd";
  std::string upperMd5(lowerMd5);
  std::string upperSha(lowerSha);
  std::ranges::transform(upperMd5, upperMd5.begin(), [](unsigned char value) {
    return static_cast<char>(std::toupper(value));
  });
  std::ranges::transform(upperSha, upperSha.begin(), [](unsigned char value) {
    return static_cast<char>(std::toupper(value));
  });

  TempDirectory temporary;
  for (const int inputVersion : {0, 1, 2, 3, 4}) {
    const auto path =
        temporary.path() / ("migration-v" + std::to_string(inputVersion) +
                            ".db");
    {
      ChartRepository baseline(path);
      assert(baseline.EnsureReady());
    }
    {
      Database database = openDatabase(path);
      assert(database);
      const std::string favoriteMd5 =
          inputVersion <= 1 ? upperMd5 : std::string(lowerMd5);
      const std::string favoriteSha =
          inputVersion <= 1 ? upperSha : std::string(lowerSha);
      assert(execute(
          database.get(),
          "INSERT INTO chart_meta("
          "path,md5,sha256,title,subtitle,genre,artist,sub_artist,"
          "folder,stage_file,banner,back_bmp,preview,level,"
          "difficulty,total,has_total,bpm,max_bpm,min_bpm,length,"
          "rank,player,keys,total_notes,total_long_notes,"
          "total_scratch_notes,total_backspin_notes,ln_mode,"
          "source_priority,source_archive_size) VALUES("
          "'migration.bms','" +
              std::string(lowerMd5) + "','" + std::string(lowerSha) +
              "','Migration','Sub','Genre','Artist','Sub Artist',"
              "'folder','','','','',12,3,234,1,180,200,120,90,2,1,"
              "7,1000,20,5,2,1,0,0);"
              "INSERT INTO chart_favorites("
              "chart_path,chart_md5,chart_sha256) VALUES("
              "'migration.bms','" +
              favoriteMd5 + "','" + favoriteSha + "');"
              "PRAGMA user_version=" + std::to_string(inputVersion)));
    }

    ChartRepository migrated(path);
    assert(migrated.EnsureReady());
    Database database = openDatabase(path);
    assert(database);
    assert(queryInt(database.get(), "PRAGMA user_version") == 4);
    assert(queryInt(database.get(), "SELECT COUNT(*) FROM chart_meta") ==
           (inputVersion == 4 ? 1 : 0));
    assert(queryInt(database.get(),
                    "SELECT COUNT(*) FROM chart_favorites") == 1);
    assert(queryString(database.get(),
                       "SELECT chart_md5 FROM chart_favorites") == lowerMd5);
    assert(queryString(database.get(),
                       "SELECT chart_sha256 FROM chart_favorites") ==
           lowerSha);
    const bool rebuildTableExists =
        queryInt(database.get(),
                 "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND "
                 "name='chart_meta_rebuild_state'") == 1;
    const bool rebuildRowExists =
        rebuildTableExists &&
        queryInt(database.get(),
                 "SELECT COUNT(*) FROM chart_meta_rebuild_state WHERE id=1") ==
            1;
    const int rebuildRequired =
        rebuildRowExists
            ? queryInt(database.get(),
                       "SELECT required FROM chart_meta_rebuild_state "
                       "WHERE id=1")
            : 0;
    assert(inputVersion == 4 ? rebuildRequired == 0
                             : rebuildRowExists && rebuildRequired == 1);
    if (inputVersion == 4) {
      assert(queryInt(database.get(), "SELECT total FROM chart_meta") == 234);
      assert(queryInt(database.get(),
                      "SELECT has_total FROM chart_meta") == 1);
      assert(queryInt(database.get(),
                      "SELECT has_document FROM chart_meta") == 0);
    }
  }
}

void testChartMigrationReleaseFailureDoesNotReportSuccess() {
  TempDirectory temporary;
  const auto path = temporary.path() / "release-failure.db";
  {
    Database database = openDatabase(path);
    assert(database);
    assert(execute(database.get(),
                   "CREATE TABLE chart_meta(path TEXT PRIMARY KEY);"
                   "INSERT INTO chart_meta(path) VALUES ('legacy.bms');"
                   "PRAGMA user_version=2"));
  }

  ChartRepository repository(path);
  const std::uint64_t revisionBefore = repository.GetLibraryRevision();
  std::atomic<int> connections{0};
  {
    ScopedConnectionObserver observer(connections, nullptr, 1);
    assert(!repository.EnsureReady());
  }

  {
    Database database = openDatabase(path);
    assert(database);
    assert(queryInt(database.get(), "PRAGMA user_version") == 2);
    assert(queryInt(database.get(), "SELECT COUNT(*) FROM chart_meta") == 1);
    assert(queryInt(database.get(),
                    "SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
                    "AND name='chart_meta_rebuild_state'") == 0);
  }
  assert(repository.GetLibraryRevision() == revisionBefore);

  assert(repository.EnsureReady());
  {
    Database database = openDatabase(path);
    assert(database);
    assert(queryInt(database.get(), "PRAGMA user_version") == 4);
    assert(queryInt(database.get(), "SELECT COUNT(*) FROM chart_meta") == 0);
    assert(queryInt(database.get(),
                    "SELECT required FROM chart_meta_rebuild_state "
                    "WHERE id=1") == 1);
  }
  assert(repository.GetLibraryRevision() == revisionBefore + 2);
}

void testLegacyIosContainerPathRebasesToCurrentDocuments() {
  const std::filesystem::path currentDocuments =
      "/private/var/mobile/Containers/Data/Application/"
      "b5702f7e-8d09-4559-b7c1-a9131a684b8a/Documents";
  const std::filesystem::path legacyPath =
      "/var/mobile/Containers/Data/Application/"
      "FEA6861E-8321-4800-8A2B-F79AC5C8E564/Documents/BMS";
  const auto rebased =
      chart_storage_identity::RebaseLegacyIOSDocumentsPath(legacyPath,
                                                            currentDocuments);
  assert(rebased == currentDocuments / "BMS");

  const std::filesystem::path externalPath =
      "/private/var/mobile/Containers/Shared/AppGroup/"
      "2887ECDB-CE93-49A5-97F6-A75107EDD35D/File Provider Storage/BMSFILES";
  assert(!chart_storage_identity::RebaseLegacyIOSDocumentsPath(
      externalPath, currentDocuments));
}

const ChartEntry *entryAtPath(const std::vector<ChartEntry> &entries,
                              const std::filesystem::path &path) {
  const auto it = std::find_if(
      entries.begin(), entries.end(), [&path](const ChartEntry &entry) {
        return std::filesystem::path(entry.path).lexically_normal() ==
               path.lexically_normal();
      });
  return it == entries.end() ? nullptr : &*it;
}

void testFindBmsDownloadEntrySelectionLifecycle() {
  TempDirectory temporary;
  ChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session);

  const auto fallback = ChartRepository::DefaultBmsFolderPath();
  const auto first = temporary.path() / "first";
  const auto second = temporary.path() / "second";
  assert(session->InsertEntry(fallback));
  assert(!session->SelectPrimaryStorageEntry());

  assert(session->InsertEntry(first, "first-bookmark"));
  auto selected = session->SelectPrimaryStorageEntry();
  assert(selected && std::filesystem::path(selected->path) == first);
  assert(selected->primaryStorageFolder);
  assert(selected->primaryStorageEligible);

  assert(session->InsertEntry(first, "updated-bookmark"));
  assert(session->InsertEntry(second, "second-bookmark"));
  selected = session->SelectPrimaryStorageEntry();
  assert(selected && std::filesystem::path(selected->path) == first);
  assert(selected->iosBookmark == "updated-bookmark");

  assert(session->SetPrimaryStorageEntry(second));
  selected = session->SelectPrimaryStorageEntry();
  assert(selected && std::filesystem::path(selected->path) == second);

  int removedChartCount = -1;
  assert(session->DeleteEntryAndChartMetaInDirectory(second,
                                                     removedChartCount));
  selected = session->SelectPrimaryStorageEntry();
  assert(selected && std::filesystem::path(selected->path) == first);

  assert(session->DeleteEntryAndChartMetaInDirectory(first,
                                                     removedChartCount));
  assert(!session->SelectPrimaryStorageEntry());
  const auto entries = session->SelectEffectiveEntries();
  const auto *fallbackEntry = entryAtPath(entries, fallback);
#if !TARGET_OS_ANDROID
  assert(fallbackEntry != nullptr);
  assert(fallbackEntry->removable);
#endif
  if (fallbackEntry != nullptr) {
    assert(!fallbackEntry->primaryStorageFolder);
    assert(!fallbackEntry->primaryStorageEligible);
  }
}

void testFindBmsDownloadEntryRejectsIneligiblePaths() {
  TempDirectory temporary;
  ChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session);

  const std::filesystem::path virtualTree =
      std::filesystem::path("@androidtree@") / "tree-id" / "Charts";
  const auto normal = temporary.path() / "normal";
  assert(session->InsertEntry(virtualTree, "content://tree/example"));
  assert(!session->SelectPrimaryStorageEntry());
  assert(!session->SetPrimaryStorageEntry(virtualTree));
  assert(!session->SetPrimaryStorageEntry(temporary.path() / "missing"));

  assert(session->InsertEntry(normal));
  const auto entries = session->SelectAllEntries();
  const auto *virtualEntry = entryAtPath(entries, virtualTree);
  assert(virtualEntry != nullptr);
  assert(!virtualEntry->primaryStorageEligible);
  assert(!virtualEntry->primaryStorageFolder);
  assert(session->SelectPrimaryStorageEntry());
}

void testFindBmsDownloadEntryMigratesLegacyAndNormalizesDuplicates() {
  TempDirectory temporary;
  const auto databasePath = temporary.path() / "chart.db";
  const auto first = temporary.path() / "legacy-first";
  const auto second = temporary.path() / "legacy-second";
  {
    Database database = openDatabase(databasePath);
    assert(database);
    assert(execute(database.get(),
                   "CREATE TABLE entries (path TEXT PRIMARY KEY, "
                   "ios_bookmark TEXT DEFAULT '')"));
    assert(execute(database.get(),
                   "INSERT INTO entries(path) VALUES ('" +
                       first.generic_string() + "')"));
    assert(execute(database.get(),
                   "INSERT INTO entries(path) VALUES ('" +
                       second.generic_string() + "')"));
  }

  ChartRepository repository(databasePath);
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session);
  auto selected = session->SelectPrimaryStorageEntry();
  assert(selected && std::filesystem::path(selected->path) == first);

  {
    Database database = openDatabase(databasePath);
    assert(database);
    assert(execute(database.get(),
                   "UPDATE entries SET primary_storage_folder = 1"));
  }
  selected = session->SelectPrimaryStorageEntry();
  assert(selected && std::filesystem::path(selected->path) == first);

  Database verification = openDatabase(databasePath);
  assert(verification);
  assert(queryInt(verification.get(),
                  "SELECT COUNT(*) FROM entries "
                  "WHERE primary_storage_folder = 1") == 1);
}

void testEntryMutationsPreserveOriginalDatabasePathKey() {
  TempDirectory temporary;
  const auto databasePath = temporary.path() / "chart.db";
  const auto first = temporary.path() / "first";
  const auto second = temporary.path() / "second";
  const auto storedSecond = temporary.path() / "alias" / ".." / "second";
  {
    Database database = openDatabase(databasePath);
    assert(database);
    assert(execute(database.get(),
                   "CREATE TABLE entries (path TEXT PRIMARY KEY, "
                   "ios_bookmark TEXT DEFAULT '', "
                   "primary_storage_folder INTEGER NOT NULL DEFAULT 0)"));
    assert(execute(database.get(),
                   "INSERT INTO entries(path) VALUES ('" +
                       first.generic_string() + "')"));
    assert(execute(database.get(),
                   "INSERT INTO entries(path) VALUES ('" +
                       storedSecond.generic_string() + "')"));
  }

  ChartRepository repository(databasePath);
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session);
  auto selected = session->SelectPrimaryStorageEntry();
  assert(selected && std::filesystem::path(selected->path) == first);

  assert(session->SetPrimaryStorageEntry(second));
  selected = session->SelectPrimaryStorageEntry();
  assert(selected &&
         std::filesystem::path(selected->path).lexically_normal() == second);

  assert(session->DeleteEntry(second));
  const auto entries = session->SelectAllEntries();
  assert(entryAtPath(entries, second) == nullptr);
}

void testEntryUpsertPreservesOriginalDatabasePathKey() {
  TempDirectory temporary;
  const auto databasePath = temporary.path() / "chart.db";
  const auto resolvedPath = temporary.path() / "charts";
  const auto storedPath = temporary.path() / "alias" / ".." / "charts";
  {
    Database database = openDatabase(databasePath);
    assert(database);
    assert(execute(database.get(),
                   "CREATE TABLE entries (path TEXT PRIMARY KEY, "
                   "ios_bookmark TEXT DEFAULT '', "
                   "primary_storage_folder INTEGER NOT NULL DEFAULT 0)"));
    assert(execute(database.get(),
                   "INSERT INTO entries(path, ios_bookmark) VALUES ('" +
                       storedPath.generic_string() + "', 'old-bookmark')"));
  }

  ChartRepository repository(databasePath);
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session);
  assert(session->InsertEntry(resolvedPath, "updated-bookmark"));

  const auto entries = session->SelectAllEntries();
  assert(entries.size() == 1);
  assert(entryAtPath(entries, resolvedPath) != nullptr);
  assert(entries.front().iosBookmark == "updated-bookmark");

  Database verification = openDatabase(databasePath);
  assert(verification);
  assert(queryInt(verification.get(), "SELECT COUNT(*) FROM entries") == 1);
  assert(queryString(verification.get(), "SELECT path FROM entries") ==
         storedPath.generic_string());
}

} // namespace

int main() {
  testScanBatchCommitAndRollback();
  testScanBatchRetainsSessionStorage();
  testScanBatchReusesPreparedInsertAndTransaction();
  testSessionRoundTripAndReadinessCost();
  testSelectChartMetaByPathsHydratesInInputOrder();
  testSelectChartMetaByHashUsesDurableIndexedIdentity();
  testRejectedFamiliesRemainUnchanged();
  testChartQueryBehaviorMatrix();
  testExactFolderQuery();
  testChartMigrationCompatibilityMatrix();
  testChartMigrationReleaseFailureDoesNotReportSuccess();
  testLegacyIosContainerPathRebasesToCurrentDocuments();
  testFindBmsDownloadEntrySelectionLifecycle();
  testFindBmsDownloadEntryRejectsIneligiblePaths();
  testFindBmsDownloadEntryMigratesLegacyAndNormalizesDuplicates();
  testEntryMutationsPreserveOriginalDatabasePathKey();
  testEntryUpsertPreservesOriginalDatabasePathKey();
  return 0;
}
