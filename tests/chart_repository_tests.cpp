#include "../src/repositories/ChartRepository.h"
#include "../src/LongNoteModeUtils.h"
#include "../src/repositories/ChartStorageIdentity.h"
#include "../src/repositories/ScoreRepository.h"
#include "../src/repositories/ScoreCacheQueries.h"
#include "../src/repositories/SqliteRAII.h"
#include "../src/ir/IrScoreHistoryProjection.h"
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

int observeAuthorization(void *, int action, const char *first, const char *,
                         const char *, const char *) {
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
      ScanBatchSqlObservation *scanObservation = nullptr) {
    assert(connectionCount == nullptr);
    assert(scanBatchSqlObservation == nullptr);
    connectionCount = &count;
    scanBatchSqlObservation = scanObservation;
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
  assert(queryInt(inspection.get(), "PRAGMA user_version") == 3);
  SqliteStatementHandle journalMode;
  assert(prepareSqliteStatement(inspection.get(), "PRAGMA journal_mode",
                                journalMode) == SQLITE_OK);
  assert(sqlite3_step(journalMode.get()) == SQLITE_ROW);
  assert(sqliteColumnString(journalMode.get(), 0) == "wal");
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
                   "PRAGMA user_version=4"));
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
             {"beta.bms", "gamma.bms", "alpha.bms", "delta.bms"}, 4,
             0);

  auto prepared = scores.PrepareScoreQueryDatabase(*session);
  assert(!prepared.error().has_value());
  auto projectedClearRanks = scores.LoadBestClearRanks(
      *session, score_cache_queries::kScoreDatabaseSchema);
  auto projectedBestScores = scores.LoadBestScores(
      *session, score_cache_queries::kScoreDatabaseSchema);
  ir::IrRemoteScore remoteOnly{
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
  ir::projectIrRemoteScores(std::span{&remoteOnly, 1}, projectedClearRanks,
                            projectedBestScores);
  const auto folderData =
      session->LoadFolderClearDataByLongNoteMode(projectedClearRanks);
  ChartMetaQuery hardQuery;
  hardQuery.clearMarkFilter = true;
  hardQuery.clearMarkRank = kClearTypeHardClearRank;
  hardQuery.selectedLongNoteMode = 1;
  std::vector<ChartMetaRecord> openedHardFolder;
  session->QueryChartMeta(
      ir::chartMetaQueryWithoutProjectedScoreCriteria(hardQuery),
      openedHardFolder);
  ir::applyProjectedScoreQuery(hardQuery, projectedClearRanks,
                               projectedBestScores, openedHardFolder);
  const auto &allCounts =
      folderData.clearMarkCounts[long_note_mode::kLnValue].at("all");
  const auto hardCount = allCounts.find(kClearTypeHardClearRank);
  assert(hardCount != allCounts.end() &&
         hardCount->second == static_cast<int>(openedHardFolder.size()));
  assert(std::ranges::any_of(openedHardFolder, [](const auto &record) {
    return chart_storage_identity::StoredPathText(record.meta.BmsPath) ==
           "delta.bms";
  }));
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
  for (const int inputVersion : {0, 1, 2, 3}) {
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
    assert(queryInt(database.get(), "PRAGMA user_version") == 3);
    assert(queryInt(database.get(), "SELECT COUNT(*) FROM chart_meta") ==
           (inputVersion == 3 ? 1 : 0));
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
    assert(inputVersion == 3 ? rebuildRequired == 0
                             : rebuildRowExists && rebuildRequired == 1);
    if (inputVersion == 3) {
      assert(queryInt(database.get(), "SELECT total FROM chart_meta") == 234);
      assert(queryInt(database.get(),
                      "SELECT has_total FROM chart_meta") == 1);
    }
  }
}

} // namespace

int main() {
  testScanBatchCommitAndRollback();
  testScanBatchRetainsSessionStorage();
  testScanBatchReusesPreparedInsertAndTransaction();
  testSessionRoundTripAndReadinessCost();
  testRejectedFamiliesRemainUnchanged();
  testChartQueryBehaviorMatrix();
  testChartMigrationCompatibilityMatrix();
  return 0;
}
