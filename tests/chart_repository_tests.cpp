#include "../src/repositories/ChartRepository.h"
#include "../src/LongNoteModeUtils.h"
#include "../src/repositories/ChartStorageIdentity.h"
#include "../src/repositories/ScoreCacheQueries.h"
#include "../src/repositories/ScoreRepository.h"
#include "../src/repositories/SqliteRAII.h"
#include "../src/targets.h"
#include "RepositorySqliteTestSupport.h"
#include "music_select/MusicSelectRepositoryProjection.h"
#include "music_select/MusicSelectPropertyProjection.h"
#include "music_select/MusicSelectPhysicalDirectory.h"
#include "music_select/MusicSelectPagedSongs.h"
#include "music_select/MusicSelectReplaySlots.h"
#include "music_select/MusicSelectSqlSongs.h"

#include <array>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
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
std::stop_source *readCancellation = nullptr;
std::string cancelReadSql;
int cancelReadAfterRows = 0;
int observedReadRows = 0;
int observedReadVmSteps = 0;
int observedProbeVmSteps = 0;
std::string deniedChartReadColumn;
bool interruptSelectionRead = false;
struct PhysicalDirectoryPageObservation {
  std::size_t limit = 0;
  std::size_t offset = 0;
  std::size_t rows = 0;
};
std::vector<PhysicalDirectoryPageObservation> physicalDirectoryPages;

int traceStatement(unsigned mask, void *, void *statement, void *) {
  if (statement == nullptr) {
    return 0;
  }
  const char *sql = sqlite3_sql(static_cast<sqlite3_stmt *>(statement));
  const std::string_view sqlText = sql != nullptr ? sql : "";
  if (sqlText.find("LIMIT @selector_limit OFFSET @selector_offset") !=
      std::string_view::npos) {
    std::lock_guard lock(traceMutex);
    if (mask == SQLITE_TRACE_STMT) {
      char *expanded = sqlite3_expanded_sql(static_cast<sqlite3_stmt *>(statement));
      assert(expanded != nullptr);
      const std::string text(expanded);
      sqlite3_free(expanded);
      const auto limitPosition = text.rfind(" LIMIT ");
      const auto offsetPosition = text.rfind(" OFFSET ");
      assert(limitPosition != std::string::npos && offsetPosition != std::string::npos);
      physicalDirectoryPages.push_back({
          std::stoull(text.substr(limitPosition + 7)),
          std::stoull(text.substr(offsetPosition + 8)), 0});
    } else if (mask == SQLITE_TRACE_ROW) {
      assert(!physicalDirectoryPages.empty());
      ++physicalDirectoryPages.back().rows;
    }
  }
  if (interruptSelectionRead && mask == SQLITE_TRACE_STMT &&
      sqlText.find("@recursive_folder") != std::string_view::npos) {
    sqlite3_interrupt(sqlite3_db_handle(static_cast<sqlite3_stmt *>(statement)));
  }
  if (mask == SQLITE_TRACE_PROFILE) {
    const int steps = sqlite3_stmt_status(static_cast<sqlite3_stmt *>(statement),
                                          SQLITE_STMTSTATUS_VM_STEP, 0);
    if (sqlText.starts_with("SELECT 1 FROM chart_meta cm")) {
      observedProbeVmSteps = steps;
    }
    if (!cancelReadSql.empty() && sqlText.find(cancelReadSql) != std::string::npos) {
      observedReadVmSteps = steps;
    }
    return 0;
  }
  if (readCancellation && sqlText.find(cancelReadSql) != std::string::npos) {
    if (mask == SQLITE_TRACE_ROW) ++observedReadRows;
    if ((mask == SQLITE_TRACE_STMT && cancelReadAfterRows == 0) ||
        (mask == SQLITE_TRACE_ROW && observedReadRows == cancelReadAfterRows)) {
      readCancellation->request_stop();
    }
  }
  if (mask != SQLITE_TRACE_STMT) return 0;
  if (scanBatchSqlObservation != nullptr) {
    if (sqlText.starts_with("BEGIN")) {
      scanBatchSqlObservation->begins.fetch_add(1, std::memory_order_relaxed);
    } else if (sqlText.starts_with("COMMIT")) {
      scanBatchSqlObservation->commits.fetch_add(1,
                                                  std::memory_order_relaxed);
    } else if (sqlText.starts_with("INSERT INTO chart_meta")) {
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
  if (!deniedChartReadColumn.empty() && action == SQLITE_READ &&
      first != nullptr && second != nullptr &&
      std::string_view(first) == "chart_meta" &&
      deniedChartReadColumn == second) {
    return SQLITE_DENY;
  }
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
  sqlite3_trace_v2(database, SQLITE_TRACE_STMT | SQLITE_TRACE_ROW |
                               SQLITE_TRACE_PROFILE, traceStatement, nullptr);
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

std::string tracedStatementStartingWith(std::string_view expected) {
  std::lock_guard lock(traceMutex);
  for (const auto &statement : tracedStatements) {
    if (statement.starts_with(expected)) return statement;
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
  meta.TotalLandmineNotes = 3;
  meta.RandomValues = {2};
  meta.MostPrevalentBpm = 175.5;
  const ChartScanCheckpoint checkpoint{
      .found = true,
      .scanSignature = "repository-test",
      .phase = "individual",
      .nextIndex = 1,
      .lastPath = meta.BmsPath,
  };
  auto batch = session->BeginScanBatch();
  assert(batch.has_value());
  assert(batch->UpsertChart(meta, std::nullopt, false,
                            {.hasBpmStop = true,
                             .hasScrollChange = true,
                             .hasBga = true}));
  assert(batch->CheckpointAndContinue(checkpoint));
  assert(batch->Commit());
  assert(session->CountAllChartMeta() == 1);
  std::vector<ChartMetaRecord> records;
  session->QueryChartMeta({}, records);
  assert(records.size() == 1);
  assert(records.front().hasBpmStop);
  assert(records.front().hasScrollChange);
  assert(records.front().hasBga);
  assert(records.front().meta.TotalLandmineNotes == 3);
  assert(records.front().hasRandomSequence);
  assert(records.front().meta.MostPrevalentBpm == 175.5);

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

void testScanBatchUpsertPreservesExistingAddDate() {
  TempDirectory temporary;
  const auto databasePath = temporary.path() / "chart.db";
  ChartRepository repository(databasePath);
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());

  auto meta = chartMeta(temporary.path());
  auto first = session->BeginScanBatch();
  assert(first.has_value());
  assert(first->UpsertChart(meta, std::nullopt));
  assert(first->Commit());

  {
    Database database = openDatabase(databasePath);
    assert(database);
    assert(execute(database.get(), "UPDATE chart_meta SET add_date=123456"));
  }
  meta.Title = "Reindexed";
  auto second = session->BeginScanBatch();
  assert(second.has_value());
  assert(second->UpsertChart(meta, std::nullopt));
  assert(second->Commit());

  std::vector<ChartMetaRecord> records;
  session->QueryChartMeta({}, records);
  assert(records.size() == 1);
  assert(records.front().meta.Title == "Reindexed");
  assert(records.front().addDateSeconds == 123456);
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
  assert(queryInt(inspection.get(), "PRAGMA user_version") == 10);
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

  {
    Database reviewDatabase = openDatabase(databasePath);
    assert(reviewDatabase);
    assert(execute(
        reviewDatabase.get(),
        "INSERT INTO review(sha256, favorite) VALUES('" + first.SHA256 +
            "', 9),('" + second.SHA256 + "', 6)"));
  }

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
  assert(loaded.records[0].songReviewFavorite == 9);
  assert(loaded.records[1].meta.BmsPath == second.BmsPath);
  assert(loaded.records[1].meta.StageFile == second.StageFile);
  assert(loaded.records[1].meta.TotalNotes == second.TotalNotes);
  assert(loaded.records[1].songReviewFavorite == 6);
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

void testFavoriteToggleMaintainsSongReviewChartBit() {
  TempDirectory temporary;
  const auto databasePath = temporary.path() / "chart.db";
  ChartRepository repository(databasePath);
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());

  auto meta = chartMeta(temporary.path());
  assert(session->InsertChartMeta(meta));
  assert(session->SetFavorite(meta, true));
  const std::array paths{meta.BmsPath};
  auto loaded = session->SelectChartMetaByPaths(paths);
  assert(loaded.status == ChartMetaPathBatchReadStatus::Loaded);
  assert(loaded.records.size() == 1);
  assert(loaded.records.front().songReviewFavorite == 2);
  ChartMetaQuery query;
  query.rawSongData = true;
  std::vector<ChartMetaRecord> raw;
  session->QueryChartMeta(query, raw);
  assert(raw.size() == 1 && raw.front().favorite);
  assert(loaded.records.front().favorite == raw.front().favorite);

  {
    Database reviewDatabase = openDatabase(databasePath);
    assert(reviewDatabase);
    assert(execute(reviewDatabase.get(),
                   "UPDATE review SET favorite=15 WHERE sha256='" +
                       meta.SHA256 + "'"));
  }
  assert(session->SetFavorite(meta, false));
  loaded = session->SelectChartMetaByPaths(paths);
  assert(loaded.status == ChartMetaPathBatchReadStatus::Loaded);
  assert(loaded.records.size() == 1);
  assert(loaded.records.front().songReviewFavorite == 13);
  raw.clear();
  session->QueryChartMeta(query, raw);
  assert(raw.size() == 1 && !raw.front().favorite);
  assert(loaded.records.front().favorite == raw.front().favorite);
}

void testSongReviewFavoritePersistsExactSourceBitfield() {
  TempDirectory temporary;
  ChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());

  auto meta = chartMeta(temporary.path());
  assert(session->InsertChartMeta(meta));
  assert(session->SetSongReviewFavorite(meta.SHA256, 13));
  const std::array paths{meta.BmsPath};
  auto loaded = session->SelectChartMetaByPaths(paths);
  assert(loaded.status == ChartMetaPathBatchReadStatus::Loaded);
  assert(loaded.records.size() == 1);
  assert(loaded.records.front().songReviewFavorite == 13);

  assert(session->SetSongReviewFavorite(meta.SHA256, 2));
  loaded = session->SelectChartMetaByPaths(paths);
  assert(loaded.records.front().songReviewFavorite == 2);
  assert(session->SetSongReviewFavorite({}, 15));
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
                   "PRAGMA user_version=11"));
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

void testDifficultyEntryDownloadUrlsFollowTheirSourceRows() {
  TempDirectory temporary;
  const auto chartPath = temporary.path() / "chart.db";
  ChartRepository charts(chartPath);
  assert(charts.EnsureReady());
  auto session = charts.OpenSession();
  assert(session.has_value());

  constexpr std::string_view installedMd5 =
      "11111111111111111111111111111111";
  constexpr std::string_view installedSha =
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  auto installed = chartMeta(temporary.path() / "installed");
  installed.MD5 = installedMd5;
  installed.SHA256 = installedSha;
  installed.Title = "Installed";
  assert(session->InsertChartMeta(installed));

  difficulty_table::Document table;
  table.name = "URL table";
  table.symbol = "☆";
  table.sourceUrl = "https://table.example/header.json";
  table.dataUrl = "https://table.example/data.json";
  table.levelOrder = {"1"};
  table.charts = {
      {.level = "1",
       .md5 = std::string(installedMd5),
       .sha256 = std::string(installedSha),
       .title = "Installed table row",
       .url = "https://table.example/installed.zip",
       .urlDiff = "https://table.example/installed-patch.zip"},
      {.level = "1",
       .md5 = "22222222222222222222222222222222",
       .sha256 =
           "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
       .title = "Unavailable table row",
       .url = "https://table.example/unavailable.zip",
       .urlDiff = "https://table.example/unavailable-patch.zip",
       .originalMd5s = std::vector<std::string>{
           std::string(installedMd5)}},
  };
  table.courses = {{
      .name = "URL course",
      .groupName = "Courses",
      .level = "1",
      .trophies = {{.name = "silvermedal",
                    .missRate = 5.0,
                    .scoreRate = 70.0}},
      .charts = {{.level = "1",
                  .md5 = std::string(installedMd5),
                  .sha256 = std::string(installedSha),
                  .title = "Installed course row",
                  .url = "https://course.example/installed.zip",
                  .urlDiff =
                      "https://course.example/installed-patch.zip"}},
  }};
  assert(session->ReplaceDifficultyTable(table));

  const auto tables = session->SelectDifficultyTables();
  assert(tables.size() == 1);
  ChartMetaQuery levelQuery;
  levelQuery.tableId = tables.front().id;
  levelQuery.tableLevel = "1";
  std::vector<ChartMetaRecord> levelRows;
  session->QueryChartMeta(levelQuery, levelRows);
  assert(levelRows.size() == 2);
  assert(levelRows[0].downloadUrl ==
         "https://table.example/installed.zip");
  assert(levelRows[0].appendDownloadUrl ==
         "https://table.example/installed-patch.zip");
  assert(levelRows[1].downloadUrl ==
         "https://table.example/unavailable.zip");
  assert(levelRows[1].appendDownloadUrl ==
         "https://table.example/unavailable-patch.zip");
  assert(levelRows[1].originalMd5s ==
         std::optional<std::vector<std::string>>({std::string(installedMd5)}));

  const auto courses =
      session->SelectDifficultyCourses(tables.front().id, "Courses");
  assert(courses.size() == 1);
  assert(courses.front().trophies.size() == 1);
  assert(courses.front().trophies.front().name == "silvermedal");
  assert(courses.front().trophies.front().missRate == 5.0);
  assert(courses.front().trophies.front().scoreRate == 70.0);
  ChartMetaQuery courseQuery;
  courseQuery.courseId = courses.front().id;
  std::vector<ChartMetaRecord> courseRows;
  session->QueryChartMeta(courseQuery, courseRows);
  assert(courseRows.size() == 1);
  assert(courseRows.front().downloadUrl ==
         "https://course.example/installed.zip");
  assert(courseRows.front().appendDownloadUrl ==
         "https://course.example/installed-patch.zip");

  std::vector<ChartMetaRecord> libraryRows;
  session->QueryChartMeta({}, libraryRows);
  assert(libraryRows.size() == 1);
  assert(libraryRows.front().downloadUrl.empty());
  assert(libraryRows.front().appendDownloadUrl.empty());

  auto copy = installed;
  copy.Folder = temporary.path() / "copy";
  copy.BmsPath = copy.Folder / "chart.bms";
  assert(session->InsertChartMeta(copy));
  const auto scoreFor = [](const bms_parser::ChartMeta &, int) {
    return std::optional<ScoreBestSnapshot>{{.score = 600, .maxScore = 800,
                                            .clearType = kClearTypeHardClearRank}};
  };
  RecentScoreImprovements improvements;
  improvements.lamp[0].insert(std::string(installedSha));
  for (const auto &directory : std::vector<MusicSelectBar>{
           {.kind = skin::MusicSelectBarKind::Folder,
            .directoryPath = temporary.path()},
           {.kind = skin::MusicSelectBarKind::Hash,
            .tableId = tables.front().id, .tableLevel = "1"},
           {.id = {"search:Installed"}, .kind = skin::MusicSelectBarKind::SearchWord},
           {.id = {"command:lamp-update:0"},
            .kind = skin::MusicSelectBarKind::Command}}) {
    const auto status = MusicSelectRepositoryProjection::loadFolderStatus(
        *session, directory,
        {.scoreFor = scoreFor, .recentScoreImprovements = &improvements});
    assert(status.folderLampCounts[6] == 2);
    assert(status.folderRankCounts[20] == 2);
    assert(status.folderLampCounts[0] == 0);
    assert(status.lamp == 6);
  }
  auto sameMd5 = installed;
  sameMd5.Folder = temporary.path() / "different-sha";
  sameMd5.BmsPath = sameMd5.Folder / "chart.bms";
  sameMd5.SHA256 = std::string(64, 'c');
  assert(session->InsertChartMeta(sameMd5));
  const auto hashStatus = MusicSelectRepositoryProjection::loadFolderStatus(
      *session, {.kind = skin::MusicSelectBarKind::Hash,
                 .tableId = tables.front().id, .tableLevel = "1"},
      {.scoreFor = scoreFor});
  assert(hashStatus.folderLampCounts[6] == 2);
  table.charts.front().sha256.clear();
  assert(session->ReplaceDifficultyTable(table));
  const auto md5Status = MusicSelectRepositoryProjection::loadFolderStatus(
      *session, {.kind = skin::MusicSelectBarKind::Hash,
                 .tableId = tables.front().id, .tableLevel = "1"},
      {.scoreFor = scoreFor});
  assert(md5Status.folderLampCounts[6] == 3);
  assert(session->SetSongReviewFavorite(installedSha, 2));
  const auto commandRecords = MusicSelectRepositoryProjection::loadDirectoryRecords(
      *session, {.id = {"command:lamp-update:0"},
                 .kind = skin::MusicSelectBarKind::Command}, 1, &improvements);
  assert(commandRecords.size() == 2);
  assert(std::ranges::all_of(commandRecords, [](const auto &record) {
    return record.songReviewFavorite == 2;
  }));
}

void testDirectoryRecordsIncludeDirectChartsAndRawDescendants() {
  TempDirectory temporary;
  ChartRepository charts(temporary.path() / "chart.db");
  auto session = charts.OpenSession();
  assert(session);
  const auto root = temporary.path() / "library";
  auto direct = chartMeta(root);
  assert(session->InsertChartMeta(direct));
  const MusicSelectBar directory{.id = {"folder:" + root.string()},
                                 .kind = skin::MusicSelectBarKind::Folder,
                                 .directoryPath = root};
  auto records = MusicSelectRepositoryProjection::loadDirectoryRecords(
      *session, directory, 1);
  assert(records.size() == 1);
  assert(records.front().meta.BmsPath == direct.BmsPath);
  MusicSelectRepositoryMetadata directMetadata;
  directMetadata.entries.push_back({.path = fspath_to_path_t(root)});
  const auto directProjection = MusicSelectRepositoryProjection{}.project(
      {.records = records, .metadata = &directMetadata});
  const auto *directFolder = directProjection.find(directory.id);
  assert(directFolder && directFolder->children.size() == 1);
  assert(directFolder->presentation.folderRankCounts[0] == 1);
  const auto *directSong = directProjection.find(directFolder->children.front());
  assert(directSong && directSong->kind == skin::MusicSelectBarKind::Song);
  assert(directSong->chart && directSong->chart->meta.BmsPath == direct.BmsPath);

  auto duplicate = direct;
  duplicate.Folder = root / "nested" / "leaf";
  duplicate.BmsPath = duplicate.Folder / "copy.bms";
  assert(session->InsertChartMeta(duplicate));
  auto immediate = direct;
  immediate.Folder = root / "song";
  immediate.BmsPath = immediate.Folder / "immediate.bms";
  assert(session->InsertChartMeta(immediate));
  auto sibling = direct;
  sibling.Folder = temporary.path() / "library-other";
  sibling.BmsPath = sibling.Folder / "other.bms";
  assert(session->InsertChartMeta(sibling));
  records = MusicSelectRepositoryProjection::loadDirectoryRecords(
      *session, directory, 1);
  assert(records.size() == 3);
  assert(std::ranges::any_of(records, [&](const auto &record) {
    return record.meta.BmsPath == duplicate.BmsPath;
  }));
  const auto status = MusicSelectRepositoryProjection::loadFolderStatus(
      *session, directory, {});
  assert(status.folderRankCounts[0] == 3);
  MusicSelectRepositoryMetadata metadata;
  metadata.entries.push_back({.path = fspath_to_path_t(root)});
  const auto projection = MusicSelectRepositoryProjection{}.project(
      {.records = records, .metadata = &metadata});
  const auto *folder = projection.find(directory.id);
  assert(folder && folder->children.size() == 1);
  assert(folder->presentation.folderRankCounts[0] == 3);
  assert(projection.find(folder->children.front())->kind ==
         skin::MusicSelectBarKind::Song);

  const auto leafRecords = MusicSelectRepositoryProjection::loadDirectoryRecords(
      *session, {.kind = skin::MusicSelectBarKind::Folder,
                 .directoryPath = duplicate.Folder}, 1);
  assert(leafRecords.size() == 1);
  assert(leafRecords.front().meta.BmsPath == duplicate.BmsPath);
  MusicSelectRepositoryMetadata leafMetadata;
  leafMetadata.entries.push_back({.path = fspath_to_path_t(duplicate.Folder)});
  const auto leafProjection = MusicSelectRepositoryProjection{}.project(
      {.records = leafRecords, .metadata = &leafMetadata});
  const auto *leafFolder = leafProjection.find(
      {"folder:" + duplicate.Folder.string()});
  assert(leafFolder && leafFolder->children.size() == 1);
  assert(leafFolder->presentation.folderRankCounts[0] == 1);
  const auto *leafSong = leafProjection.find(leafFolder->children.front());
  assert(leafSong && leafSong->kind == skin::MusicSelectBarKind::Song);
  assert(leafSong->chart && leafSong->chart->meta.BmsPath == duplicate.BmsPath);
  assert(MusicSelectRepositoryProjection::loadDirectoryRecords(
      *session, {.kind = skin::MusicSelectBarKind::Folder,
                 .directoryPath = root / "empty"}, 1).empty());
}

void testRawExactFolderKeepsNonpreferredDuplicate() {
  TempDirectory temporary;
  ChartRepository charts(temporary.path() / "chart.db");
  auto session = charts.OpenSession();
  assert(session);
  auto preferred = chartMeta("/a/song");
  auto duplicate = preferred;
  duplicate.Folder = "/z/song";
  duplicate.BmsPath = duplicate.Folder / "chart.bms";
  assert(session->InsertChartMeta(preferred));
  assert(session->InsertChartMeta(duplicate));

  ChartMetaQuery query;
  std::vector<ChartMetaRecord> records;
  session->QueryChartMeta(query, records);
  assert(records.size() == 1);
  assert(records.front().meta.BmsPath == preferred.BmsPath);

  records.clear();
  query.exactFolder = duplicate.Folder;
  session->QueryChartMeta(query, records);
  assert(records.empty());

  query.rawSongData = true;
  session->QueryChartMeta(query, records);
  assert(records.size() == 1);
  assert(records.front().meta.BmsPath == duplicate.BmsPath);
  assert(records.front().meta.SHA256 == preferred.SHA256);
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
        "'sha-windows-nested','Windows Nested','','','','','',12,0,0),"
        "('C:\\library\\A\\nested\\stored.bms','md5-windows-stored',"
        "'sha-windows-stored','Windows Stored','','','','',"
        "'C:\\library\\A\\nested',13,0,0)"));
  }

  auto session = charts.OpenSession();
  assert(session.has_value());
  const auto folders = session->SelectChartMetaFolders();
  assert(folders == std::vector<std::filesystem::path>({
                        R"(C:\library\A\nested)",
                        "library/A", "library/A/nested", "library/B",
                        "packs/pack.zip/A", "packs/pack.zip/B"}));
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

  query = {};
  query.parentFolder = std::filesystem::path("library");
  auto parentPaths = queryPaths(query);
  std::ranges::sort(parentPaths);
  assert(parentPaths == std::vector<std::string>({
      "library/A/no-folder.bms", "library/A/one.bms", "library/A/two.bms",
      "library/B/four.bms", "library/C/aliased.bms", "library/C/trailing.bms"}));
  assert(session->CountChartMeta(query) == 6);
  assert(session->FindChartMetaIndex(query, "library/A/nested/three.bms") == -1);

  MusicSelectRepositoryMetadata metadata;
  metadata.entries.push_back({.path = fspath_to_path_t("library")});
  MusicSelectBarManager bars(MusicSelectRepositoryProjection{}.projectRoot(
      metadata, {}, 1));
  const auto before = bars.snapshot();
  const auto folder = *std::ranges::find(before.rows, MusicSelectBarId{"folder:library"},
                                        &MusicSelectBar::id);
  assert(!folder.childrenLoaded);
  const auto status = MusicSelectRepositoryProjection::loadFolderStatus(
      *session, folder, {});
  bars.installFolderStatus(folder.id, status);
  assert(bars.select(folder.id));
  const auto after = bars.snapshot();
  assert(!after.rows[after.selectedIndex].childrenLoaded);
  assert(after.rows[after.selectedIndex].presentation.folderRankCounts[0] == 8);
  const auto values = projectMusicSelectProperties(AppSettings{}, after, {});
  assert(values.integers.at(300) == 8);
  assert(values.integers.at(320) == 8);
  assert(values.integers.at(326) == 0);
  const auto flattened = MusicSelectRepositoryProjection::loadDirectoryRecords(
      *session, folder, 1);
  assert(flattened.size() == 8);
  assert(std::ranges::any_of(flattened, [](const auto &record) {
    return record.meta.BmsPath == "library/A/nested/three.bms";
  }));
  const auto flatProjection = MusicSelectRepositoryProjection{}.project(
      {.records = flattened, .metadata = &metadata});
  const auto *flatFolder = flatProjection.find(folder.id);
  assert(flatFolder && flatFolder->children.size() == 8);
  assert(std::ranges::all_of(flatFolder->children, [&](const auto &id) {
    return flatProjection.find(id)->kind == skin::MusicSelectBarKind::Song;
  }));

  query.parentFolder = std::filesystem::path("packs/pack.zip/");
  parentPaths = queryPaths(query);
  std::ranges::sort(parentPaths);
  assert(parentPaths == std::vector<std::string>({
      "packs/pack.zip/A/five.bms", "packs/pack.zip/A/no-folder.bms",
      "packs/pack.zip/A/six.bms", "packs/pack.zip/B/seven.bms"}));

  query.parentFolder = std::filesystem::path(R"(C:\library\A)");
  assert(queryPaths(query) == std::vector<std::string>({
      R"(C:\library\A\nested\deep.bms)",
      R"(C:\library\A\nested\stored.bms)"}));

  query = {};
  query.recursiveFolder = std::filesystem::path("library/A/");
  assert(queryPaths(query).size() == 5);
  assert(session->CountChartMeta(query) == 5);
  assert(session->FindChartMetaIndex(query, "library/B/four.bms") == -1);
  query.recursiveFolder = std::filesystem::path("library/A/nest");
  assert(queryPaths(query).empty());
  query.recursiveFolder = std::filesystem::path("packs/pack.zip/");
  assert(queryPaths(query).size() == 4);
  query.recursiveFolder = std::filesystem::path(R"(C:\library\A)");
  assert(queryPaths(query).size() == 3);
  query.recursiveFolder = std::filesystem::path("/");
  assert(queryPaths(query).empty());

  const auto categoryRecords = MusicSelectRepositoryProjection::loadDirectoryRecords(
      *session, {.kind = skin::MusicSelectBarKind::Folder,
                 .directoryPath = "packs"}, 1);
  assert(categoryRecords.size() == 4);

  assert(session->HasChartMetaForParentFolder("library"));
  const auto probeSql = tracedStatementStartingWith("SELECT 1 FROM chart_meta cm");
  assert(!probeSql.empty());
  assert(probeSql.find("ORDER BY") == std::string::npos);
  assert(probeSql.find("JOIN") == std::string::npos);
  assert(probeSql.find("LIMIT 1") != std::string::npos);

  assert(!traced("chart_normalize_stored_folder(cm.folder)"));
  assert(traced("cm.folder = @exact_folder"));

  Database database = openDatabase(chartPath);
  assert(database);
  const auto probePlan = repository_test::explainPlan(database.get(), probeSql);
  assert(repository_test::planContains(probePlan, "idx_chart_meta_folder"));
  assert(!repository_test::planContains(probePlan, "SCAN cm"));
  assert(!repository_test::planContains(probePlan, "TEMP B-TREE"));
  const std::string countSql = tracedStatementContaining(
      "SELECT COUNT(*) FROM chart_meta cm WHERE 1 = 1 AND (cm.folder = "
      "@exact_folder");
  assert(!countSql.empty());
  const auto plan = repository_test::explainPlan(database.get(), countSql);
  assert(repository_test::planContains(plan, "MULTI-INDEX OR"));
  assert(repository_test::planContains(plan, "idx_chart_meta_folder"));
  assert(!repository_test::planContains(plan, "SCAN cm"));
  const auto parentCountSql = tracedStatementContaining(
      "SELECT COUNT(*) FROM chart_meta cm WHERE 1 = 1 AND ((cm.folder >= ");
  assert(!parentCountSql.empty());
  const auto parentPlan = repository_test::explainPlan(database.get(), parentCountSql);
  assert(repository_test::planContains(parentPlan, "idx_chart_meta_folder"));
  assert(!repository_test::planContains(parentPlan, "SCAN cm"));
  const auto recursiveCountSql = tracedStatementContaining(
      "SELECT COUNT(*) FROM chart_meta cm WHERE 1 = 1 AND (cm.folder = "
      "@recursive_folder");
  assert(!recursiveCountSql.empty());
  const auto recursivePlan = repository_test::explainPlan(database.get(), recursiveCountSql);
  assert(repository_test::planContains(recursivePlan, "idx_chart_meta_folder"));
  assert(!repository_test::planContains(recursivePlan, "SCAN cm"));
}

void testStreamingSelectionMatchesRawRowsWithNarrowPayload() {
  TempDirectory temporary;
  std::atomic<int> connections{0};
  ScopedConnectionObserver observer(connections);
  ChartRepository charts(temporary.path() / "chart.db");
  assert(charts.EnsureReady());
  auto database = openDatabase(charts.DatabasePath());
  assert(database);
  assert(execute(database.get(),
      "INSERT INTO chart_meta(path,folder,md5,sha256,title) VALUES "
      "('library/A/z.bms','library/A','duplicate','same','alpha'),"
      "('library/A/a.bms','library/A','duplicate','same','ALPHA'),"
      "('library/A/empty.bms','','md5-only','','Beta'),"
      "('library/A/null.bms',NULL,'','',NULL),"
      "('library/A/nested/chart.bms','library/A/nested','deep','deep','gamma'),"
      "('library/A/nested/leaf/chart.bms',NULL,'leaf','leaf','delta'),"
      "('library/AB/chart.bms','library/AB','sibling','sibling','outside'),"
      "('C:\\library\\A\\own.bms','C:\\library\\A','win','win','alpha'),"
      "('C:\\library\\A\\empty.bms','','win-empty','win-empty','Beta'),"
      "('C:\\library\\A\\nested\\null.bms',NULL,'win-null','win-null','gamma'),"
      "('C:\\library\\A\\nested\\stored.bms','C:\\library\\A\\nested',"
      "'win-stored','win-stored','delta'),"
      "('C:\\library\\AB\\chart.bms','C:\\library\\AB','other','other','outside'),"
      "('/absolute/song.bms','/absolute','absolute','absolute','absolute')"));
  assert(execute(database.get(),
      "UPDATE chart_meta SET artist='Artist', difficulty=4, level=12.5, "
      "keys=14, player=2, total_notes=901, total_scratch_notes=23, "
      "total_backspin_notes=17, total_long_notes=41, ln_mode=3, "
      "min_bpm=87.25, max_bpm=231.5, length=5123456789, "
      "has_bpm_stop=1, has_scroll_change=1, subtitle='Subtitle', genre='Genre', "
      "sub_artist='Sub artist', stage_file='stage.png', banner='banner.png', "
      "back_bmp='back.png', preview='preview.ogg', bpm=150, "
      "total=200, has_total=1, rank=1, has_document=1, has_bga=1, "
      "has_random_sequence=1, total_landmine_notes=11, most_prevalent_bpm=155, "
      "add_date=1234"));
  assert(execute(database.get(),
      "UPDATE chart_meta SET artist=NULL, difficulty=NULL, level=NULL, "
      "keys=NULL, total_notes=NULL, total_scratch_notes=NULL, "
      "total_backspin_notes=NULL, total_long_notes=NULL, ln_mode=0, "
      "min_bpm=NULL, max_bpm=NULL, length=NULL, has_bpm_stop=0, "
      "has_scroll_change=0 WHERE path='library/A/null.bms'"));
  assert(execute(database.get(),
      "INSERT INTO review(sha256,favorite) VALUES('same',7)"));
  auto session = charts.OpenSession();
  assert(session);
  for (const auto &folder : std::vector<std::filesystem::path>{
           "library/A", "library/A/", "library/A/nest", "library/A/nested", "absent",
           R"(C:\library\A)", "C:/library/A/", "", "/"}) {
    ChartMetaQuery query;
    query.recursiveFolder = folder;
    query.rawSongData = true;
    std::vector<ChartMetaRecord> expected;
    session->QueryChartMeta(query, expected);
    std::size_t visited = 0;
    std::vector<std::filesystem::path> paths;
    session->VisitChartMetaSelection(folder, [&](const ChartMetaRecord &record) {
      assert(visited < expected.size());
      const auto &rich = expected[visited++];
      paths.push_back(record.meta.BmsPath);
      assert(record.meta.BmsPath == rich.meta.BmsPath);
      assert(record.meta.SHA256 == rich.meta.SHA256);
      assert(record.meta.MD5 == rich.meta.MD5);
      assert(record.meta.Title == rich.meta.Title);
      assert(record.meta.Artist == rich.meta.Artist);
      assert(record.meta.Difficulty == rich.meta.Difficulty);
      assert(record.meta.PlayLevel == rich.meta.PlayLevel);
      assert(record.meta.KeyMode == rich.meta.KeyMode);
      assert(record.meta.IsDP == rich.meta.IsDP);
      assert(record.meta.TotalNotes == rich.meta.TotalNotes);
      assert(record.meta.TotalScratchNotes == rich.meta.TotalScratchNotes);
      assert(record.meta.TotalBackSpinNotes == rich.meta.TotalBackSpinNotes);
      assert(record.meta.TotalLongNotes == rich.meta.TotalLongNotes);
      assert(record.meta.LnMode == rich.meta.LnMode);
      assert(record.meta.MinBpm == rich.meta.MinBpm);
      assert(record.meta.MaxBpm == rich.meta.MaxBpm);
      assert(record.meta.PlayLength == rich.meta.PlayLength);
      assert(record.songReviewFavorite == rich.songReviewFavorite);
      assert(record.hasScrollChange == rich.hasScrollChange);
      assert(record.hasBpmStop == rich.hasBpmStop);
      assert(record.meta.Folder.empty());
      assert(record.meta.SubTitle.empty());
      assert(record.meta.SubArtist.empty());
      assert(record.meta.Genre.empty());
      assert(record.meta.StageFile.empty());
      assert(record.meta.Banner.empty());
      assert(record.meta.BackBmp.empty());
      assert(record.meta.Preview.empty());
      assert(record.meta.Bpm == 0 && record.meta.MostPrevalentBpm == 0);
      assert(record.meta.Total == 100 && !record.meta.HasTotal);
      assert(record.meta.Rank == 3 && record.meta.Player == 1);
      assert(record.meta.TotalLandmineNotes == 0);
      assert(record.meta.RandomValues.empty());
      assert(record.addDateSeconds == 0 && !record.hasDocument && !record.hasBga);
      assert(!record.hasRandomSequence && !record.favorite);
      assert(record.difficultyTableLabels.empty());
      assert(record.downloadUrl.empty() && record.appendDownloadUrl.empty());
      assert(!record.originalMd5s && !record.unavailable && !record.solidArchive);
    });
    assert(visited == expected.size());
    if (folder == "library/A") {
      assert(paths == std::vector<std::filesystem::path>({
          "library/A/null.bms", "library/A/a.bms", "library/A/z.bms",
          "library/A/empty.bms", "library/A/nested/leaf/chart.bms",
          "library/A/nested/chart.bms"}));
      assert(expected[1].songReviewFavorite == 7);
      assert(expected[3].meta.SHA256.empty() && expected[3].meta.MD5 == "md5-only");
    }
    if (folder == R"(C:\library\A)") assert(visited == 4);
    if (folder.empty() || folder == "/") assert(visited == 1);
  }
  for (const auto *column : {"stage_file", "banner", "back_bmp", "preview",
                            "subtitle", "genre", "sub_artist", "bpm",
                            "total_landmine_notes", "has_document", "has_bga"}) {
    deniedChartReadColumn = column;
    int visited = 0;
    session->VisitChartMetaSelection("library/A", [&](const ChartMetaRecord &) {
      ++visited;
    });
    assert(visited == 6);
  }
  deniedChartReadColumn.clear();
}

void testOwnOrImmediateChildFolderProbe() {
  TempDirectory temporary;
  std::atomic<int> connections{0};
  ScopedConnectionObserver observer(connections);
  ChartRepository charts(temporary.path() / "chart.db");
  assert(charts.EnsureReady());
  auto database = openDatabase(charts.DatabasePath());
  assert(database);
  assert(execute(database.get(),
      "INSERT INTO chart_meta(path,folder,md5,sha256) "
      "SELECT column1,column2,'','' FROM (VALUES "
      "('own/chart.bms','own'),('nested/song/chart.bms','nested/song'),"
      "('deeper/song/leaf/chart.bms','deeper/song/leaf'),"
      "('empty-own/chart.bms',''),('null-own/chart.bms',NULL),"
      "('empty-child/song/chart.bms',''),('null-child/song/chart.bms',NULL),"
      "('empty-deep/song/leaf/chart.bms',''),"
      "('null-deep/song/leaf/chart.bms',NULL),"
      "('C:\\own\\chart.bms','C:\\own'),"
      "('C:\\nested\\song\\chart.bms','C:\\nested\\song'),"
      "('C:\\deeper\\song\\leaf\\chart.bms','C:\\deeper\\song\\leaf'),"
      "('C:\\empty-own\\chart.bms',''),('C:\\null-own\\chart.bms',NULL),"
      "('C:\\empty-child\\song\\chart.bms',''),"
      "('C:\\null-child\\song\\chart.bms',NULL),"
      "('C:\\empty-deep\\song\\leaf\\chart.bms',''),"
      "('C:\\null-deep\\song\\leaf\\chart.bms',NULL))"));
  auto session = charts.OpenSession();
  assert(session);
  for (const auto *folder : {"own", "own/", "nested", "empty-own", "null-own",
                            "empty-child", "null-child", R"(C:\own)",
                            R"(C:\nested)", R"(C:\empty-own)", R"(C:\null-own)",
                            R"(C:\empty-child)", R"(C:\null-child)"}) {
    assert(session->HasChartMetaForFolderOrParentFolder(folder));
  }
  for (const auto *folder : {"absent", "ow", "deeper", "empty-deep", "null-deep",
                            R"(C:\deeper)", R"(C:\empty-deep)", R"(C:\null-deep)"}) {
    assert(!session->HasChartMetaForFolderOrParentFolder(folder));
  }
  assert(!session->HasChartMetaForParentFolder("own"));
  assert(session->HasChartMetaForParentFolder("nested"));
  const auto sql = tracedStatementContaining("@exact_folder");
  assert(!sql.empty() && sql.find("LIMIT 1") != std::string::npos);
  assert(sql.find("ORDER BY") == std::string::npos);
  assert(sql.find("JOIN") == std::string::npos);
  const auto plan = repository_test::explainPlan(database.get(), sql);
  assert(repository_test::planContains(plan, "idx_chart_meta_folder"));
  assert(!repository_test::planContains(plan, "SCAN cm"));
  std::stop_source cancelled;
  cancelled.request_stop();
  bool threw = false;
  try {
    (void)session->HasChartMetaForFolderOrParentFolder("own", cancelled.get_token());
  } catch (const std::runtime_error &) {
    threw = true;
  }
  assert(threw);
  assert(session->HasChartMetaForFolderOrParentFolder("own"));
}

void testStreamingSelectionFailuresThrowAndReleaseStatement() {
  TempDirectory temporary;
  std::atomic<int> connections{0};
  ScopedConnectionObserver observer(connections);
  ChartRepository charts(temporary.path() / "chart.db");
  assert(charts.EnsureReady());
  auto session = charts.OpenSession();
  assert(session);
  auto meta = chartMeta(temporary.path());
  assert(session->InsertChartMeta(meta));
  for (const bool cancellable : {false, true}) {
    std::stop_source cancellation;
    const auto stop = cancellable ? cancellation.get_token() : std::stop_token{};
    for (const bool prepareFailure : {true, false}) {
      deniedChartReadColumn = prepareFailure ? "title" : "";
      interruptSelectionRead = !prepareFailure;
      bool threw = false;
      int visited = 0;
      try {
        session->VisitChartMetaSelection(temporary.path(),
            [&](const ChartMetaRecord &) { ++visited; }, stop);
      } catch (const std::runtime_error &) {
        threw = true;
      }
      assert(threw && visited == 0);
      deniedChartReadColumn.clear();
      interruptSelectionRead = false;
    }
  }
  bool threw = false;
  try {
    session->VisitChartMetaSelection(temporary.path(), [](const ChartMetaRecord &) {
      throw std::logic_error("visitor failed");
    });
  } catch (const std::logic_error &) {
    threw = true;
  }
  assert(threw);
  int visited = 0;
  session->VisitChartMetaSelection(temporary.path(),
      [&](const ChartMetaRecord &) { ++visited; });
  assert(visited == 1);
}

void testFolderProbeAndCancelledReadsDoNotPoisonSession() {
  TempDirectory temporary;
  std::atomic<int> connections{0};
  ScopedConnectionObserver observer(connections);
  ChartRepository charts(temporary.path() / "chart.db");
  assert(charts.EnsureReady());
  {
    auto database = openDatabase(charts.DatabasePath());
    assert(execute(database.get(),
        "WITH RECURSIVE rows(value) AS (SELECT 1 UNION ALL "
        "SELECT value + 1 FROM rows WHERE value < 50000) "
        "INSERT INTO chart_meta(path,folder,md5,sha256,title) "
        "SELECT 'library/song/' || value || '.bms', 'library/song', "
        "printf('%032d', 0), printf('%064d', 0), "
        "printf('%06d', 50000-value) FROM rows"));
  }
  auto session = charts.OpenSession();
  assert(session);
  assert(session->HasChartMetaForParentFolder("library"));
  assert(observedProbeVmSteps > 0 && observedProbeVmSteps < 1000);
  assert(!session->HasChartMetaForParentFolder("absent"));
  assert(session->HasChartMetaForFolderOrParentFolder("library/song"));
  assert(observedProbeVmSteps > 0 && observedProbeVmSteps < 1000);
  assert(session->HasChartMetaForFolderOrParentFolder("library"));
  assert(observedProbeVmSteps > 0 && observedProbeVmSteps < 1000);

  std::stop_source cancelled;
  readCancellation = &cancelled;
  cancelReadSql = "@recursive_folder";
  cancelReadAfterRows = 0;
  observedReadRows = 0;
  {
    std::lock_guard lock(traceMutex);
    tracedStatements.clear();
  }
  bool threw = false;
  try {
    (void)MusicSelectRepositoryProjection::loadDirectoryRecords(
        *session, {.kind = skin::MusicSelectBarKind::Folder,
                   .directoryPath = "library"}, 1, nullptr,
        cancelled.get_token());
  } catch (const std::runtime_error &) {
    threw = true;
  }
  assert(threw && cancelled.stop_requested());
  assert(traced("@recursive_folder"));

  for (const int rowLimit : {0, 3}) {
    cancelled = std::stop_source{};
    cancelReadAfterRows = rowLimit;
    observedReadRows = 0;
    observedReadVmSteps = 0;
    int visited = 0;
    threw = false;
    try {
      session->VisitChartMetaSelection("library",
          [&](const ChartMetaRecord &) { ++visited; }, cancelled.get_token());
    } catch (const std::runtime_error &) {
      threw = true;
    }
    assert(threw && cancelled.stop_requested());
    assert(observedReadRows == rowLimit);
    assert(visited == (rowLimit == 0 ? 0 : rowLimit - 1));
    if (rowLimit == 0) assert(observedReadVmSteps < 1000);
  }
  readCancellation = nullptr;
  cancelled = std::stop_source{};
  int visited = 0;
  threw = false;
  try {
    session->VisitChartMetaSelection("library", [&](const ChartMetaRecord &) {
      if (++visited == 3) cancelled.request_stop();
    }, cancelled.get_token());
  } catch (const std::runtime_error &) {
    threw = true;
  }
  assert(threw && visited == 3);
  visited = 0;
  threw = false;
  try {
    session->VisitChartMetaSelection("library",
        [&](const ChartMetaRecord &) { ++visited; }, cancelled.get_token());
  } catch (const std::runtime_error &) {
    threw = true;
  }
  assert(threw && visited == 0);
  session->VisitChartMetaSelection("library",
      [&](const ChartMetaRecord &) { ++visited; });
  assert(visited == 50000);
  readCancellation = &cancelled;

  ChartMetaQuery query;
  query.recursiveFolder = "library";
  query.rawSongData = true;
  std::vector<ChartMetaRecord> records;
  for (const int rowLimit : {0, 3}) {
    cancelled = std::stop_source{};
    cancelReadSql = "@recursive_folder";
    cancelReadAfterRows = rowLimit;
    observedReadRows = 0;
    observedReadVmSteps = 0;
    threw = false;
    try {
      session->QueryChartMeta(query, records, cancelled.get_token());
    } catch (const std::runtime_error &) {
      threw = true;
    }
    assert(threw && cancelled.stop_requested());
    assert(records.empty());
    assert(observedReadRows == rowLimit);
    if (rowLimit == 0) assert(observedReadVmSteps < 1000);
    readCancellation = nullptr;
    query.limit = 1;
    session->QueryChartMeta(query, records);
    assert(records.size() == 1);
    records.clear();
    query.limit = 0;
    readCancellation = &cancelled;
  }
  cancelled = std::stop_source{};
  cancelReadSql = "WHERE cm.sha256 = ?";
  cancelReadAfterRows = 3;
  observedReadRows = 0;
  threw = false;
  try {
    (void)session->SelectChartMetaByHash(std::string(64, '0'), {},
                                         cancelled.get_token());
  } catch (const std::runtime_error &) {
    threw = true;
  }
  assert(threw && observedReadRows == 3);

  cancelled = std::stop_source{};
  cancelReadSql = "WHERE cm.path IN (";
  cancelReadAfterRows = 1;
  observedReadRows = 0;
  const std::vector<std::filesystem::path> paths{
      "library/song/1.bms", "library/song/2.bms"};
  threw = false;
  try {
    (void)session->SelectChartMetaByPaths(paths, cancelled.get_token());
  } catch (const std::runtime_error &) {
    threw = true;
  }
  assert(threw && observedReadRows == 1);
  readCancellation = nullptr;
  cancelReadSql.clear();
  assert(session->SelectChartMetaByPaths(paths).records.size() == 2);
  assert(session->HasChartMetaForParentFolder("library"));
  assert(session->CountAllChartMeta() == 50000);
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
  for (const int inputVersion : {0, 1, 2, 3, 4, 5, 6, 7}) {
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
      assert(execute(database.get(),
                     "ALTER TABLE chart_meta DROP COLUMN add_date;"
                     "ALTER TABLE chart_meta DROP COLUMN total_landmine_notes;"
                     "ALTER TABLE chart_meta DROP COLUMN has_random_sequence;"
                     "ALTER TABLE chart_meta DROP COLUMN most_prevalent_bpm"));
      if (inputVersion <= 5) {
        assert(execute(database.get(),
                       "ALTER TABLE chart_meta DROP COLUMN has_bpm_stop;"
                       "ALTER TABLE chart_meta DROP COLUMN has_scroll_change"));
      }
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
      if (inputVersion >= 5) {
        assert(execute(database.get(),
                       "INSERT INTO review(sha256, favorite) VALUES('" +
                           std::string(lowerSha) + "', 2)"));
      }
    }

    ChartRepository migrated(path);
    assert(migrated.EnsureReady());
    Database database = openDatabase(path);
    assert(database);
    assert(queryInt(database.get(), "PRAGMA user_version") == 10);
    assert(queryInt(database.get(), "SELECT COUNT(*) FROM chart_meta") == 0);
    assert(queryInt(database.get(),
                    "SELECT COUNT(*) FROM chart_favorites") == 1);
    assert(queryString(database.get(),
                       "SELECT chart_md5 FROM chart_favorites") == lowerMd5);
    assert(queryString(database.get(),
                       "SELECT chart_sha256 FROM chart_favorites") ==
           lowerSha);
    assert(queryInt(database.get(), "SELECT COUNT(*) FROM review") == 1);
    const std::string reviewFavoriteQuery =
        "SELECT favorite FROM review WHERE sha256='" +
        std::string(lowerSha) + "'";
    assert(queryInt(database.get(), reviewFavoriteQuery.c_str()) == 2);
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
    assert(rebuildRowExists && rebuildRequired == 1);
    assert(queryInt(database.get(),
                    "SELECT COUNT(*) FROM pragma_table_info('chart_meta') "
                    "WHERE name IN ('has_bpm_stop', "
                    "'has_scroll_change')") == 2);
    assert(queryInt(database.get(),
                    "SELECT COUNT(*) FROM pragma_table_info('folder') "
                    "WHERE name IN ('path', 'date', 'adddate')") == 3);
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
    assert(queryInt(database.get(), "PRAGMA user_version") == 10);
    assert(queryInt(database.get(), "SELECT COUNT(*) FROM chart_meta") == 0);
    assert(queryInt(database.get(),
                    "SELECT required FROM chart_meta_rebuild_state "
                    "WHERE id=1") == 1);
  }
  assert(repository.GetLibraryRevision() == revisionBefore + 6);
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

void testMetadataFolderMergePreservesNormalizedDuplicatesAndScales() {
  TempDirectory temporary;
  const auto databasePath = temporary.path() / "chart.db";
  ChartRepository repository(databasePath);
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session);
  Database database = openDatabase(databasePath);
  assert(database);
  assert(execute(database.get(),
      "INSERT INTO folder(path,date,adddate) VALUES "
      "('/songs/alias/../kept',123,456),('/songs/kept',789,987)"));
  assert(execute(database.get(),
      "INSERT INTO chart_meta(path,folder,md5,sha256) VALUES "
      "('/songs/kept/chart.bms','/songs/kept','',''),"
      "('/songs/new/a.bms','/songs/alias/../new','',''),"
      "('/songs/new/b.bms','/songs/new','','')"));
  const auto originalFolders = session->SelectFolderRecords();
  const auto merged = MusicSelectRepositoryProjection::loadMetadata(*session, 0);
  assert(merged.folders.size() == 3);
  for (std::size_t index = 0; index < originalFolders.size(); ++index) {
    assert(merged.folders[index].path == originalFolders[index].path);
    assert(merged.folders[index].dateSeconds == originalFolders[index].dateSeconds);
    assert(merged.folders[index].addDateSeconds == originalFolders[index].addDateSeconds);
  }
  assert(std::filesystem::path(merged.folders.back().path).lexically_normal() ==
         std::filesystem::path("/songs/new"));
  assert(merged.folders.back().dateSeconds == 0);
  assert(merged.folders.back().addDateSeconds == 0);
  assert(execute(database.get(),
      "WITH RECURSIVE sequence(number) AS (SELECT 1 UNION ALL "
      "SELECT number+1 FROM sequence WHERE number < 12000) "
      "INSERT INTO chart_meta(path,folder,md5,sha256) SELECT "
      "'/library/folder-'||number||'/chart.bms',"
      "'/library/folder-'||number,'','' FROM sequence"));
  const auto started = std::chrono::steady_clock::now();
  const auto large = MusicSelectRepositoryProjection::loadMetadata(*session, 0);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  std::fprintf(stderr, "12000-folder metadata merge: %.3f seconds\n",
               std::chrono::duration<double>(elapsed).count());
  assert(large.folders.size() == 12003);
  assert(elapsed < std::chrono::seconds(5));
}

namespace {

MusicSelectBar physicalDirectory(const std::filesystem::path &path) {
  return {.id = {"folder:" + path.generic_string()},
          .kind = skin::MusicSelectBarKind::Folder,
          .directoryPath = path,
          .childrenLoaded = false};
}

void clearPhysicalDirectoryTrace() {
  std::lock_guard lock(traceMutex);
  tracedStatements.clear();
  physicalDirectoryPages.clear();
}

std::vector<std::size_t> physicalDirectoryPageSizes() {
  std::vector<std::size_t> sizes;
  std::lock_guard lock(traceMutex);
  for (const auto &page : physicalDirectoryPages) {
    sizes.push_back(page.limit);
  }
  return sizes;
}

void testPhysicalDirectoryCategoriesAndEmptyFoldersUseOnlyMetadata() {
  TempDirectory temporary;
  std::atomic<int> connections{0};
  ScopedConnectionObserver observer(connections);
  ChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session);
  auto database = openDatabase(repository.DatabasePath());
  assert(database);
  const auto root = temporary.path() / "library";
  assert(execute(database.get(),
      "INSERT INTO folder(path,date,adddate) VALUES ('" +
      (root / "category").generic_string() + "',123,456),('" +
      (root / "empty").generic_string() + "',789,987)"));
  auto nested = chartMeta(root / "category" / "song");
  assert(session->InsertChartMeta(nested));
  const auto metadata = MusicSelectRepositoryProjection::loadMetadata(*session, 0);
  clearPhysicalDirectoryTrace();
  deniedChartReadColumn = "title";
  const auto loaded = loadMusicSelectPhysicalDirectory(
      repository, metadata, physicalDirectory(root), {}, {}, temporary.path(), {}, 0);
  assert(!loaded.provider);
  assert(loaded.children.size() == 2);
  assert(loaded.children[0].directoryPath == root / "category");
  assert(loaded.children[0].title == "category");
  assert(loaded.children[0].presentation.addDateSeconds == 456);
  assert(loaded.children[1].directoryPath == root / "empty");
  assert(loaded.children[1].presentation.addDateSeconds == 987);
  for (const auto &child : loaded.children) {
    assert(child.kind == skin::MusicSelectBarKind::Folder);
    assert(!child.childrenLoaded);
    assert(!child.chart);
  }
  const auto empty = loadMusicSelectPhysicalDirectory(
      repository, metadata, physicalDirectory(root / "empty"), {}, {},
      temporary.path(), {}, 0);
  assert(empty.children.empty() && !empty.provider);
  const auto absent = loadMusicSelectPhysicalDirectory(
      repository, metadata, physicalDirectory(root / "absent"), {}, {},
      temporary.path(), {}, 0);
  assert(absent.children.empty() && !absent.provider);
  deniedChartReadColumn.clear();
  assert(!traced("@recursive_folder"));
  assert(physicalDirectoryPageSizes().empty());
}

void testPhysicalDirectoryOwnAndMixedSongsMatchRawSubtree() {
  TempDirectory temporary;
  std::atomic<int> connections{0};
  ScopedConnectionObserver observer(connections);
  ChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session);
  const auto root = temporary.path() / "songs";
  const auto directory = physicalDirectory(root);
  auto own = chartMeta(root);
  own.Title = "Own";
  assert(session->InsertChartMeta(own));
  auto loaded = loadMusicSelectPhysicalDirectory(
      repository, {}, directory, {}, {}, temporary.path(), {}, 0);
  assert(loaded.children.empty() && loaded.provider);
  assert(loaded.provider->size() == 1);
  assert(loaded.provider->at(0).chart->meta.BmsPath == own.BmsPath);
  auto nested = chartMeta(root / "nested" / "leaf");
  nested.Title = "Nested";
  nested.SHA256.assign(64, 'b');
  assert(session->InsertChartMeta(nested));
  auto immediate = chartMeta(root / "immediate");
  immediate.Title = "Immediate";
  immediate.SHA256.assign(64, 'c');
  assert(session->InsertChartMeta(immediate));
  auto outside = chartMeta(temporary.path() / "songs-other");
  outside.Title = "Outside";
  outside.SHA256.assign(64, 'd');
  assert(session->InsertChartMeta(outside));
  auto duplicate = own;
  duplicate.BmsPath = root / "duplicate.bms";
  assert(session->InsertChartMeta(duplicate));
  clearPhysicalDirectoryTrace();
  loaded = loadMusicSelectPhysicalDirectory(
      repository, {}, directory, {}, {}, temporary.path(), {}, 0);
  assert(loaded.children.empty() && loaded.provider);
  assert(loaded.provider->size() == 3);
  assert(loaded.provider->at(0).chart->meta.BmsPath == immediate.BmsPath);
  assert(loaded.provider->at(1).chart->meta.BmsPath == nested.BmsPath);
  assert(loaded.provider->at(2).chart->meta.BmsPath == own.BmsPath);
  assert(physicalDirectoryPageSizes() == std::vector<std::size_t>{3});
  const auto page = tracedStatementContaining("LIMIT @selector_limit OFFSET @selector_offset");
  assert(!page.empty());
  assert(page.find("stage_file") != std::string::npos);
  assert(page.find("representative.path") != std::string::npos);
  assert(page.find("preferred") == std::string::npos);
  assert(!traced("WHERE cm.path IN ("));
}

std::string physicalChartHash(int number) {
  const auto digits = std::to_string(number);
  return std::string(64 - digits.size(), '0') + digits;
}

void seedPhysicalDirectoryPages(ChartRepository &repository,
                                const std::filesystem::path &root,
                                int count = 320) {
  auto database = openDatabase(repository.DatabasePath());
  assert(database);
  const auto path = root.generic_string();
  assert(execute(database.get(),
      "WITH RECURSIVE rows(number) AS (SELECT 1 UNION ALL "
      "SELECT number+1 FROM rows WHERE number < " + std::to_string(count) + ") "
      "INSERT INTO chart_meta(path,folder,md5,sha256,title) SELECT '" + path +
      "' || CASE WHEN number<=160 THEN '' WHEN number<=240 THEN '/nested' "
      "ELSE '/nested/leaf' END || printf('/song-%03d.bms',number), '" + path +
      "' || CASE WHEN number<=160 THEN '' WHEN number<=240 THEN '/nested' "
      "ELSE '/nested/leaf' END, printf('%032d',number), printf('%064d',number), "
      "printf('Song %03d',number) FROM rows"));
  assert(execute(database.get(),
      "UPDATE chart_meta SET subtitle='Subtitle', genre='Genre', artist='Artist', "
      "sub_artist='Sub artist', difficulty=4, level=12.5, keys=14, player=2, "
      "total_notes=901, total_scratch_notes=23, total_backspin_notes=17, "
      "total_long_notes=41, ln_mode=0, min_bpm=87.25, max_bpm=231.5, "
      "length=5123456789, has_bpm_stop=1, has_scroll_change=1, "
      "stage_file='stage.png', banner='banner.png', back_bmp='back.png', "
      "preview='preview.ogg', bpm=150, total=200, has_total=1, rank=1, "
      "has_document=1, has_bga=1, has_random_sequence=1, "
      "total_landmine_notes=11, most_prevalent_bpm=155, add_date=1234"));
}

void testPhysicalDirectoryPagesOwnWorkerSessionAndRichProjectionInputs() {
  TempDirectory temporary;
  std::atomic<int> connections{0};
  ScopedConnectionObserver observer(connections);
  ChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  const auto root = temporary.path() / "songs";
  seedPhysicalDirectoryPages(repository, root);
  auto session = repository.OpenSession();
  assert(session);
  const auto scoredPath = root / "song-160.bms";
  const auto hydrated = session->SelectChartMetaByPaths(
      std::array<std::filesystem::path, 1>{scoredPath});
  assert(hydrated.status == ChartMetaPathBatchReadStatus::Loaded);
  const auto record = hydrated.records.front();
  assert(session->SetFavorite(record.meta, true));
  assert(session->SetSongReviewFavorite(record.meta.SHA256, 3));
  ScoreRepository scores(temporary.path() / "score.db");
  scores.SetChartDatabasePath(repository.DatabasePath());
  assert(scores.EnsureSchema());
  seedChartScore(scores.GetDatabasePath(), scoredPath.generic_string(), record.meta.MD5,
                 record.meta.SHA256, 1, kClearTypeNormalClearRank, 950);
  seedChartScore(scores.GetDatabasePath(), scoredPath.generic_string(), record.meta.MD5,
                 record.meta.SHA256, 2, kClearTypeFailedRank, 800);
  seedChartScore(scores.GetDatabasePath(), scoredPath.generic_string(), record.meta.MD5,
                 record.meta.SHA256, 2, kClearTypeExHardClearRank, 200);
  seedChartScore(scores.GetDatabasePath(), "other.bms", std::string(29, '0') + "310",
                 physicalChartHash(310), 2, kClearTypeNormalClearRank, 600);
  auto best = std::make_shared<const ScoreBestCache>(scores.LoadBestScores());
  auto clears =
      std::make_shared<const ScoreClearRankCache>(scores.LoadBestClearRanks());
  const auto expectedScore = best->bestFor(record.meta, 2);
  assert(expectedScore && expectedScore->score == 800);
  assert(clears->bestRankFor(record.meta, 2) == kClearTypeExHardClearRank);
  const auto profile = temporary.path() / "profile";
  std::filesystem::create_directories(profile / "replay");
  std::ofstream(profile / "replay" / ("C" + physicalChartHash(160) + ".brd"));
  std::ofstream(profile / "replay" / ("C" + physicalChartHash(160) + "_3.brd"));
  auto metadata = MusicSelectRepositoryProjection::loadMetadata(*session, 2);
  auto directory = physicalDirectory(root);
  const auto context = directory.id.value;
  auto replayRoot = profile;
  MusicSelectBarManagerConfig config{"14KEY", "ALL", "TITLE"};
  std::stop_source cancellation;
  clearPhysicalDirectoryTrace();
  auto worker = std::async(std::launch::async, [&] {
    const auto workerMetadata = metadata;
    const auto workerDirectory = directory;
    const auto workerReplayRoot = replayRoot;
    const auto workerConfig = config;
    return loadMusicSelectPhysicalDirectory(
        repository, workerMetadata, workerDirectory, best, clears,
        workerReplayRoot, workerConfig, 2, cancellation.get_token());
  });
  auto loaded = worker.get();
  assert(loaded.children.empty() && loaded.provider);
  assert(loaded.provider->size() == 320);
  assert(physicalDirectoryPageSizes() == (std::vector<std::size_t>{128, 64}));
  const auto page = tracedStatementContaining("LIMIT @selector_limit OFFSET @selector_offset");
  assert(!page.empty() && page.find("stage_file") != std::string::npos);
  assert(!traced("WHERE cm.path IN ("));
  clearPhysicalDirectoryTrace();
  assert(loaded.provider->at(0).id.value == context + ":sha256:" + physicalChartHash(1));
  assert(loaded.provider->at(319).id.value == context + ":sha256:" + physicalChartHash(320));
  assert(physicalDirectoryPageSizes().empty());
  cancellation.request_stop();
  best.reset();
  clears.reset();
  metadata = {};
  directory = {};
  replayRoot.clear();
  config = {};
  session.reset();
  const auto bar = loaded.provider->at(159);
  assert(physicalDirectoryPageSizes() == std::vector<std::size_t>{128});
  assert(bar.id.value == context + ":sha256:" + physicalChartHash(160));
  assert(bar.title == "Song 160 Subtitle");
  assert(bar.selectable && bar.presentation.exists);
  assert(bar.chart && bar.chart->meta.BmsPath == scoredPath);
  const auto &rich = *bar.chart;
  assert(rich.meta.Folder == root);
  assert(rich.meta.Title == "Song 160" && rich.meta.SubTitle == "Subtitle");
  assert(rich.meta.Artist == "Artist" && rich.meta.SubArtist == "Sub artist");
  assert(rich.meta.Genre == "Genre");
  assert(rich.meta.StageFile == "stage.png" && rich.meta.Banner == "banner.png");
  assert(rich.meta.BackBmp == "back.png" && rich.meta.Preview == "preview.ogg");
  assert(rich.meta.MD5 == std::string(29, '0') + "160");
  assert(rich.meta.SHA256 == physicalChartHash(160));
  assert(rich.meta.Bpm == 150 && rich.meta.MostPrevalentBpm == 155);
  assert(rich.meta.MinBpm == 87.25 && rich.meta.MaxBpm == 231.5);
  assert(rich.meta.Total == 200 && rich.meta.HasTotal);
  assert(rich.meta.Rank == 1 && rich.meta.Player == 2);
  assert(rich.meta.KeyMode == 14 && rich.meta.PlayLevel == 12.5);
  assert(rich.meta.Difficulty == 4 && rich.meta.LnMode == 0);
  assert(rich.meta.PlayLength == 5123456789);
  assert(rich.meta.TotalNotes == 901 && rich.meta.TotalScratchNotes == 23);
  assert(rich.meta.TotalBackSpinNotes == 17 && rich.meta.TotalLongNotes == 41);
  assert(rich.meta.TotalLandmineNotes == 11);
  assert(rich.hasDocument && rich.hasBga && rich.hasRandomSequence);
  assert(rich.hasBpmStop && rich.hasScrollChange);
  assert(rich.favorite && rich.songReviewFavorite == 3);
  assert(rich.addDateSeconds == 1234);
  assert(!rich.unavailable && !rich.solidArchive);
  assert(rich.downloadUrl.empty() && rich.appendDownloadUrl.empty());
  assert(!rich.originalMd5s);
  assert(bar.presentation.addDateSeconds == 1234);
  assert(bar.presentation.level == 12 && bar.presentation.difficulty == 4);
  assert(bar.presentation.lamp == 7);
  assert(bar.presentation.featureFlags == (skin::MusicSelectFeatureUndefinedLn |
      skin::MusicSelectFeatureMine | skin::MusicSelectFeatureRandom));
  assert(bar.replayExists == (std::array<bool, 4>{true, false, false, true}));
  assert(bar.score && bar.score->score == 800 && bar.score->maxScore == 1000);
  assert(bar.score->clearType == kClearTypeFailedRank);
  assert(bar.score->judgementCounts == expectedScore->judgementCounts);
  assert(bar.score->fast == expectedScore->fast && bar.score->slow == expectedScore->slow);
  assert(bar.score->playCount == expectedScore->playCount);
  assert(bar.score->clearCount == expectedScore->clearCount);
  assert(bar.score->lastPlayedUnixSeconds == expectedScore->lastPlayedUnixSeconds);
  assert(bar.score->maxCombo == expectedScore->maxCombo);
  assert(bar.score->comboBreak == expectedScore->comboBreak);
  assert(bar.score->badPoints == expectedScore->badPoints);
  assert(bar.score->averageJudgeMicros == expectedScore->averageJudgeMicros);
  assert(bar.score->finalGauge == expectedScore->finalGauge);
  assert(bar.score->createdAt == expectedScore->createdAt);
  assert(bar.score->attemptId == expectedScore->attemptId);
  assert(bar.score->bestOrderTime == expectedScore->bestOrderTime);
  assert(bar.score->source == expectedScore->source);
  for (std::size_t position = 0; position < 320; ++position) {
    const MusicSelectBarId id{context + ":sha256:" + physicalChartHash(position + 1)};
    assert(loaded.provider->at(position).id == id);
    assert(loaded.provider->indexOf(id) == position);
  }
  loaded.provider->configure("14KEY", "ALL", "SCORE");
  assert(loaded.provider->at(0).chart->meta.SHA256 == physicalChartHash(310));
  assert(loaded.provider->at(1).id == bar.id);
  loaded.provider->configure("14KEY", "ALL", "CLEAR");
  assert(loaded.provider->at(0).chart->meta.SHA256 == physicalChartHash(310));
  assert(loaded.provider->at(1).id == bar.id);
  assert(loaded.provider->at(1).presentation.lamp == 7);
}

void testPhysicalDirectoryFirstPageDoesNotVisitWholeFolder() {
  TempDirectory temporary;
  std::atomic<int> connections{0};
  ScopedConnectionObserver observer(connections);
  ChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  const auto root = temporary.path() / "songs";
  seedPhysicalDirectoryPages(repository, root);
  clearPhysicalDirectoryTrace();
  const auto loaded = loadMusicSelectPhysicalDirectory(
      repository, {}, physicalDirectory(root), {}, {}, temporary.path(), {}, 0);
  assert(loaded.provider && loaded.provider->size() == 320);
  assert(loaded.provider->at(0).chart.has_value());
  assert(!traced("SELECT cm.path, cm.md5, cm.sha256, cm.title, cm.artist"));
  assert(!traced("WHERE cm.path IN ("));
  assert(physicalDirectoryPageSizes() == (std::vector<std::size_t>{128, 64}));
  {
    std::lock_guard lock(traceMutex);
    assert(physicalDirectoryPages[0].offset == 0);
    assert(physicalDirectoryPages[0].rows == 128);
    assert(physicalDirectoryPages[1].offset == 0);
    assert(physicalDirectoryPages[1].rows == 64);
  }
  clearPhysicalDirectoryTrace();
  assert(loaded.provider->at(127).chart.has_value());
  assert(loaded.provider->at(256).chart->meta.SHA256 == physicalChartHash(257));
  assert(loaded.provider->at(319).chart->meta.SHA256 == physicalChartHash(320));
  assert(physicalDirectoryPageSizes().empty());
}

void testPhysicalDirectoryCancellationAcrossProbeCountAndPriming() {
  TempDirectory temporary;
  std::atomic<int> connections{0};
  ScopedConnectionObserver observer(connections);
  ChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  const auto root = temporary.path() / "songs";
  seedPhysicalDirectoryPages(repository, root);
  const auto directory = physicalDirectory(root);
  const std::array phases{
      std::pair{"", 0}, std::pair{"SELECT 1 FROM chart_meta cm", 0},
      std::pair{"SELECT COUNT(*) FROM (SELECT cm.sha256 FROM chart_meta cm", 1},
      std::pair{"LIMIT @selector_limit OFFSET @selector_offset", 0}};
  for (const auto &[sql, rows] : phases) {
    clearPhysicalDirectoryTrace();
    std::stop_source cancellation;
    readCancellation = &cancellation;
    cancelReadSql = sql;
    cancelReadAfterRows = rows;
    observedReadRows = 0;
    if (cancelReadSql.empty()) cancellation.request_stop();
    const auto connectionsBefore = connections.load();
    bool threw = false;
    try {
      (void)loadMusicSelectPhysicalDirectory(repository, {}, directory, {}, {},
          temporary.path(), {}, 0, cancellation.get_token());
    } catch (const std::runtime_error &) {
      threw = true;
    }
    readCancellation = nullptr;
    cancelReadSql.clear();
    assert(threw && cancellation.stop_requested());
    if (std::string_view(sql).empty()) assert(connections == connectionsBefore);
    if (std::string_view(sql) == "LIMIT @selector_limit OFFSET @selector_offset") {
      assert(physicalDirectoryPageSizes() == std::vector<std::size_t>{128});
    } else {
      assert(physicalDirectoryPageSizes().empty());
    }
  }
  const auto retried = loadMusicSelectPhysicalDirectory(
      repository, {}, directory, {}, {}, temporary.path(), {}, 0);
  assert(retried.provider && retried.provider->size() == 320);
}

void testPhysicalDirectoryDurationOverflowMatchesLegacyIndex() {
  TempDirectory temporary;
  std::atomic<int> connections{0};
  ScopedConnectionObserver observer(connections);
  ChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session);
  const auto root = temporary.path() / "songs";
  const auto directory = physicalDirectory(root);
  for (int number = 1; number <= 3; ++number) {
    auto meta = chartMeta(root);
    meta.BmsPath = root / (std::to_string(number) + ".bms");
    meta.Title = "Song " + std::to_string(number);
    meta.SHA256 = physicalChartHash(number);
    meta.TotalLongNotes = 0;
    meta.TotalBackSpinNotes = 0;
    meta.LnMode = 0;
    assert(session->InsertChartMeta(meta));
  }
  const auto clears = std::make_shared<const ScoreClearRankCache>();
  {
    ScoreBestCache scores;
    const std::array<std::int64_t, 3> durations{
        0, 1, static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max()) + 1};
    for (std::size_t position = 0; position < durations.size(); ++position) {
      scores.scoreBySha256[physicalChartHash(static_cast<int>(position) + 1)]
          .snapshots[0] = ScoreBestSnapshot{.averageJudgeMicros = durations[position]};
    }
    const auto best = std::make_shared<const ScoreBestCache>(std::move(scores));
    MusicSelectSongIndex reference(directory.id.value);
    session->VisitChartMetaSelection(root, [&](const ChartMetaRecord &record) {
      reference.add(record, best->bestFor(record.meta, 0),
                    clears->bestRankFor(record.meta, 0));
    });
    reference.finish();
    const auto resolved = reference.configure("ALL", "ALL", "DURATION");
    assert(reference.size() == 3);
    const auto verify = [&](const std::shared_ptr<MusicSelectRowProvider> &provider) {
      assert(provider && provider->size() == reference.size());
      for (std::size_t position = 0; position < reference.size(); ++position) {
        const auto &bar = provider->at(position);
        assert(bar.id == reference.idAt(position));
        assert(bar.chart && bar.chart->meta.BmsPath == reference.pathAt(position));
        assert(bar.score && bar.score->averageJudgeMicros ==
            best->bestFor(bar.chart->meta, 0)->averageJudgeMicros);
        assert(provider->indexOf(bar.id) == position);
      }
    };
    clearPhysicalDirectoryTrace();
    const auto initial = loadMusicSelectPhysicalDirectory(repository, {}, directory,
        best, clears, temporary.path(), {"ALL", "ALL", "DURATION"}, 0);
    assert(initial.children.empty());
    verify(initial.provider);
    assert(traced("SELECT cm.path, cm.md5, cm.sha256, cm.title, cm.artist"));
    assert(traced("WHERE cm.path IN ("));

    clearPhysicalDirectoryTrace();
    const auto configured = loadMusicSelectPhysicalDirectory(repository, {}, directory,
        best, clears, temporary.path(), {"ALL", "ALL", "TITLE"}, 0);
    assert(configured.provider && configured.provider->size() == 3);
    assert(!traced("SELECT cm.path, cm.md5, cm.sha256, cm.title, cm.artist"));
    clearPhysicalDirectoryTrace();
    assert(configured.provider->configure("ALL", "ALL", "DURATION") == resolved);
    verify(configured.provider);
    assert(traced("SELECT cm.path, cm.md5, cm.sha256, cm.title, cm.artist"));
    assert(traced("WHERE cm.path IN ("));

    clearPhysicalDirectoryTrace();
    configured.provider->configure("ALL", "ALL", "TITLE");
    assert(configured.provider->at(0).chart->meta.SHA256 == physicalChartHash(1));
    assert(!traced("SELECT cm.path, cm.md5, cm.sha256, cm.title, cm.artist"));
    assert(traced("LIMIT @selector_limit OFFSET @selector_offset"));
  }
}

void testPhysicalDirectoryDurationFallbackRejectsReplacedIdentity() {
  TempDirectory temporary;
  std::atomic<int> connections{0};
  ScopedConnectionObserver observer(connections);
  ChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  const auto root = temporary.path() / "songs";
  const auto directory = physicalDirectory(root);
  seedPhysicalDirectoryPages(repository, root);
  auto session = repository.OpenSession();
  assert(session);
  ScoreBestCache scores;
  for (int number = 1; number <= 320; ++number) {
    const auto duration = number == 320
        ? static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max()) + 2
        : 0;
    scores.scoreBySha256[physicalChartHash(number)].snapshots[2] =
        ScoreBestSnapshot{.averageJudgeMicros = duration};
  }
  const auto best = std::make_shared<const ScoreBestCache>(std::move(scores));
  const auto clears = std::make_shared<const ScoreClearRankCache>();
  MusicSelectSongIndex reference(directory.id.value);
  session->VisitChartMetaSelection(root, [&](const ChartMetaRecord &record) {
    reference.add(record, best->bestFor(record.meta, 2),
                  clears->bestRankFor(record.meta, 2));
  });
  reference.finish();
  reference.configure("ALL", "ALL", "DURATION");
  assert(reference.size() == 320);
  constexpr std::size_t position = 128;
  const auto originalId = reference.idAt(position);
  const auto replacedPath = reference.pathAt(position);
  const std::string replacementHash(64, 'f');
  const MusicSelectBarId replacementId{
      directory.id.value + ":sha256:" + replacementHash};
  clearPhysicalDirectoryTrace();
  const auto loaded = loadMusicSelectPhysicalDirectory(repository, {}, directory,
      best, clears, temporary.path(), {"ALL", "ALL", "DURATION"}, 2);
  const auto provider = std::dynamic_pointer_cast<MusicSelectSqlSongs>(loaded.provider);
  assert(provider && provider->size() == 320 && provider->diagnostic().empty());
  assert(traced("SELECT cm.path, cm.md5, cm.sha256, cm.title, cm.artist"));
  assert(traced("WHERE cm.path IN ("));
  assert(provider->at(0).id == reference.idAt(0));
  assert(provider->at(319).id == reference.idAt(319));
  assert(provider->indexOf(originalId) == position);
  auto database = openDatabase(repository.DatabasePath());
  assert(database);
  assert(execute(database.get(), "UPDATE chart_meta SET sha256='" +
      replacementHash + "' WHERE path='" + fspath_to_utf8(replacedPath) + "'"));
  assert(sqlite3_changes(database.get()) == 1);
  clearPhysicalDirectoryTrace();
  const auto &bar = provider->at(position);
  assert(traced("WHERE cm.path IN ("));
  assert(!bar.chart && !bar.selectable && !bar.presentation.exists);
  assert(bar.id != replacementId);
  assert(!provider->diagnostic().empty());
  assert(provider->indexOf(originalId) == position);
  assert(!provider->indexOf(replacementId));
}

void testPhysicalDirectoryStorageFailuresThrowAndRetry() {
  TempDirectory temporary;
  std::atomic<int> connections{0};
  ScopedConnectionObserver observer(connections);
  ChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session);
  auto record = chartMeta(temporary.path() / "songs");
  assert(session->InsertChartMeta(record));
  const auto directory = physicalDirectory(record.Folder);
  for (const auto *column : {"folder", "title", "stage_file"}) {
    clearPhysicalDirectoryTrace();
    deniedChartReadColumn = column;
    bool threw = false;
    try {
      (void)loadMusicSelectPhysicalDirectory(
          repository, {}, directory, {}, {}, temporary.path(), {}, 0);
    } catch (const std::runtime_error &error) {
      threw = true;
      assert(!std::string_view(error.what()).empty());
    }
    deniedChartReadColumn.clear();
    assert(threw);
    const auto retried = loadMusicSelectPhysicalDirectory(
        repository, {}, directory, {}, {}, temporary.path(), {}, 0);
    assert(retried.provider && retried.provider->size() == 1);
    assert(retried.provider->at(0).chart);
  }
  ChartRepository invalid(temporary.path());
  bool threw = false;
  try {
    (void)loadMusicSelectPhysicalDirectory(
        invalid, {}, directory, {}, {}, temporary.path(), {}, 0);
  } catch (const std::runtime_error &) {
    threw = true;
  }
  assert(threw);
}

void testPhysicalDirectoryAutoplayKeepsRawOrderHiddenAndWrongModeSongs() {
  TempDirectory temporary;
  std::atomic<int> connections{0};
  ScopedConnectionObserver observer(connections);
  ChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session);
  const auto root = temporary.path() / "songs";
  auto alpha = chartMeta(root);
  alpha.BmsPath = root / "alpha.bms";
  alpha.Title = "Alpha";
  alpha.KeyMode = 7;
  alpha.Preview = "preview.ogg";
  assert(session->InsertChartMeta(alpha));
  auto duplicate = alpha;
  duplicate.BmsPath = root / "duplicate.bms";
  duplicate.Title = "Zulu duplicate";
  assert(session->InsertChartMeta(duplicate));
  auto beta = chartMeta(root / "nested" / "leaf");
  beta.Title = "Beta";
  beta.SHA256.assign(64, 'b');
  beta.KeyMode = 14;
  assert(session->InsertChartMeta(beta));
  auto delta = chartMeta(root);
  delta.BmsPath = root / "delta.bms";
  delta.Title = "Delta";
  delta.KeyMode = 9;
  delta.SHA256.clear();
  delta.MD5.assign(32, 'd');
  assert(session->InsertChartMeta(delta));
  auto epsilon = delta;
  epsilon.BmsPath = root / "epsilon.bms";
  epsilon.Title = "Epsilon";
  epsilon.MD5.clear();
  assert(session->InsertChartMeta(epsilon));
  auto hidden = chartMeta(root);
  hidden.BmsPath = root / "hidden.bms";
  hidden.Title = "Gamma hidden";
  hidden.KeyMode = 7;
  hidden.SHA256.assign(64, 'c');
  assert(session->InsertChartMeta(hidden));
  assert(session->SetSongReviewFavorite(hidden.SHA256, 4));
  auto outside = alpha;
  outside.BmsPath = temporary.path() / "songs-other" / "outside.bms";
  outside.Folder = outside.BmsPath.parent_path();
  assert(session->InsertChartMeta(outside));
  auto database = openDatabase(repository.DatabasePath());
  assert(database);
  assert(execute(database.get(),
      "UPDATE chart_meta SET source_priority=100 WHERE path='" +
      outside.BmsPath.generic_string() + "'"));
  const auto directory = physicalDirectory(root);
  const auto filtered = loadMusicSelectPhysicalDirectory(
      repository, {}, directory, {}, {}, temporary.path(),
      {"7KEY", "ALL", "TITLE"}, 2);
  assert(filtered.provider && filtered.provider->size() == 1);
  clearPhysicalDirectoryTrace();
  const auto loaded = loadMusicSelectPhysicalDirectoryAutoplay(repository, directory, 2);
  assert(!loaded.provider);
  assert(loaded.children.size() == 4);
  const std::array expected{hidden.BmsPath, delta.BmsPath, beta.BmsPath, alpha.BmsPath};
  for (std::size_t position = 0; position < expected.size(); ++position) {
    const auto &bar = loaded.children[position];
    assert(bar.kind == skin::MusicSelectBarKind::Song);
    assert(bar.chart && bar.chart->meta.BmsPath == expected[position]);
    assert(bar.selectable && bar.presentation.exists);
    assert(!bar.score && !bar.rivalScore);
    assert(bar.replayExists == (std::array<bool, 4>{}));
    assert(bar.presentation.lamp == 0);
  }
  assert(loaded.children[0].chart->songReviewFavorite == 4);
  assert(loaded.children[1].id.value == directory.id.value + ":md5:" + delta.MD5);
  assert(loaded.children[2].chart->meta.KeyMode == 14);
  assert(loaded.children[3].id.value == directory.id.value + ":sha256:" + alpha.SHA256);
  assert(loaded.children[3].chart->meta.Preview == "preview.ogg");
  assert(physicalDirectoryPageSizes().empty());
  const auto raw = tracedStatementContaining("@recursive_folder");
  assert(!raw.empty() && raw.find("stage_file") != std::string::npos);
  assert(raw.find("preferred") == std::string::npos);
}

void testPhysicalDirectoryAutoplayCategoriesAndCancellation() {
  TempDirectory temporary;
  std::atomic<int> connections{0};
  ScopedConnectionObserver observer(connections);
  ChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session);
  const auto root = temporary.path() / "category";
  auto deep = chartMeta(root / "nested" / "song");
  deep.SHA256.clear();
  deep.MD5.clear();
  assert(session->InsertChartMeta(deep));
  clearPhysicalDirectoryTrace();
  const auto category = loadMusicSelectPhysicalDirectoryAutoplay(
      repository, physicalDirectory(root), 0);
  assert(category.children.empty() && !category.provider);
  assert(!traced("@recursive_folder"));
  const auto empty = loadMusicSelectPhysicalDirectoryAutoplay(
      repository, physicalDirectory(root / "missing"), 0);
  assert(empty.children.empty() && !empty.provider);
  const auto directory = physicalDirectory(root / "nested");
  const auto children = loadMusicSelectPhysicalDirectoryAutoplay(repository, directory, 0);
  assert(children.children.size() == 1 && !children.provider);
  assert(children.children.front().id.value == directory.id.value + ":path:" +
         deep.BmsPath.generic_string());
  for (const bool alreadyCancelled : {false, true}) {
    std::stop_source cancellation;
    if (alreadyCancelled) cancellation.request_stop();
    readCancellation = &cancellation;
    cancelReadSql = "@recursive_folder";
    cancelReadAfterRows = 0;
    bool threw = false;
    try {
      (void)loadMusicSelectPhysicalDirectoryAutoplay(
          repository, directory, 0, cancellation.get_token());
    } catch (const std::runtime_error &) {
      threw = true;
    }
    readCancellation = nullptr;
    cancelReadSql.clear();
    assert(threw && cancellation.stop_requested());
  }
  const auto retried = loadMusicSelectPhysicalDirectoryAutoplay(repository, directory, 0);
  assert(retried.children.size() == 1);
}

void testRawPhysicalFolderInventoryRestoresCategoryBranches() {
  TempDirectory temporary;
  ChartRepository charts(temporary.path() / "chart.db");
  assert(charts.EnsureReady());
  auto database = openDatabase(charts.DatabasePath());
  assert(database);
  assert(execute(database.get(),
      "INSERT INTO chart_meta(path,folder,md5,sha256,source_priority) VALUES "
      "('/raw/a/song/one.bms','/raw/a/song','same','same',0),"
      "('/raw/z/song/copy.bms','/raw/z/song','same','same',3),"
      "('/raw/empty/song/two.bms','','empty','empty',0),"
      "('/raw/null/song/three.bms',NULL,'null','null',0),"
      "('/raw/overlap/song/four.bms','/raw/overlap/song','four','four',0),"
      "('/raw/overlap/song/five.bms','','five','five',0)"));
  assert(execute(database.get(),
      "INSERT INTO folder(path,date,adddate) VALUES "
      "('/raw/dated',123,456),('/raw/overlap/song',789,987)"));
  auto session = charts.OpenSession();
  assert(session);
  assert(session->SelectChartMetaFolders() == std::vector<std::filesystem::path>({
      "/raw/a/song", "/raw/overlap/song"}));
  auto metadata = MusicSelectRepositoryProjection::loadMetadata(*session, 0);
  metadata.entries = {{.path = utf8_to_path_t("/raw")}};
  const auto children = MusicSelectRepositoryProjection::projectDirectoryFolders(metadata, "/raw");
  std::vector<std::filesystem::path> childPaths;
  for (const auto &child : children) childPaths.push_back(child.directoryPath);
  assert(childPaths == std::vector<std::filesystem::path>({
      "/raw/a", "/raw/dated", "/raw/empty", "/raw/null", "/raw/overlap", "/raw/z"}));
  assert(metadata.folders.size() == 6);
  assert(metadata.folders[0].dateSeconds == 123 && metadata.folders[0].addDateSeconds == 456);
  assert(metadata.folders[1].dateSeconds == 789 && metadata.folders[1].addDateSeconds == 987);
  MusicSelectRepositoryMetadata eagerMetadata;
  eagerMetadata.entries = metadata.entries;
  eagerMetadata.folders = session->SelectFolderRecords();
  ChartMetaQuery query;
  query.rawSongData = true;
  query.recursiveFolder = "/raw";
  std::vector<ChartMetaRecord> records;
  session->QueryChartMeta(query, records);
  assert(records.size() == 6);
  const auto eager = MusicSelectRepositoryProjection{}.project({
      .records = records, .metadata = &eagerMetadata});
  const auto *root = eager.find({"folder:" + fspath_to_utf8(
      std::filesystem::path("/raw").lexically_normal())});
  assert(root);
  std::vector<std::filesystem::path> eagerChildren;
  for (const auto &id : root->children) {
    const auto *child = eager.find(id);
    assert(child);
    eagerChildren.push_back(child->directoryPath);
  }
  std::ranges::sort(eagerChildren);
  assert(eagerChildren == childPaths);
}

void testOwnFolderProbeMatchesRecursiveWindowsDelimiters() {
  TempDirectory temporary;
  ChartRepository charts(temporary.path() / "chart.db");
  assert(charts.EnsureReady());
  auto database = openDatabase(charts.DatabasePath());
  assert(database);
  assert(execute(database.get(),
      "INSERT INTO chart_meta(path,folder,md5,sha256) VALUES "
      "('C:\\library\\back\\song.bms','C:\\library\\back','back','back'),"
      "('C:/library/slash/song.bms','C:/library/slash','slash','slash'),"
      "('C:\\library\\null\\song.bms',NULL,'null','null')"));
  auto session = charts.OpenSession();
  assert(session);
  for (const auto *folder : {"C:/library/back", "C:/library/back/", R"(C:\library\back)",
                            R"(C:\library\back\)", "C:/library/slash", R"(C:\library\slash)",
                            R"(C:\library\slash\)", "C:/library/null"}) {
    ChartMetaQuery query;
    query.rawSongData = true;
    query.recursiveFolder = folder;
    std::vector<ChartMetaRecord> records;
    session->QueryChartMeta(query, records);
    assert(records.size() == 1);
    assert(session->HasChartMetaForFolderOrParentFolder(folder));
  }
  assert(!session->HasChartMetaForFolderOrParentFolder("C:/library/absent"));
  const auto metadata = MusicSelectRepositoryProjection::loadMetadata(*session, 0);
  std::vector<std::string> childPaths;
  for (const auto &child : MusicSelectRepositoryProjection::projectDirectoryFolders(
           metadata, "C:/library")) {
    auto text = fspath_to_utf8(child.directoryPath);
    std::ranges::replace(text, '\\', '/');
    childPaths.push_back(text);
  }
  assert(childPaths == std::vector<std::string>({
      "C:/library/back", "C:/library/null", "C:/library/slash"}));
}

}

namespace {

void testPhysicalDirectoryPrimingInterruptsTheActiveRichBatch() {
  TempDirectory temporary;
  std::atomic<int> connections{0};
  ScopedConnectionObserver observer(connections);
  ChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  const auto root = temporary.path() / "songs";
  seedPhysicalDirectoryPages(repository, root);
  for (const int rowLimit : {1, 129}) {
    std::stop_source cancellation;
    readCancellation = &cancellation;
    cancelReadSql = "LIMIT @selector_limit OFFSET @selector_offset";
    cancelReadAfterRows = rowLimit;
    observedReadRows = 0;
    clearPhysicalDirectoryTrace();
    bool threw = false;
    try {
      (void)loadMusicSelectPhysicalDirectory(
          repository, {}, physicalDirectory(root), {}, {}, temporary.path(), {},
          0, cancellation.get_token());
    } catch (const std::runtime_error &) {
      threw = true;
    }
    readCancellation = nullptr;
    cancelReadSql.clear();
    assert(threw && cancellation.stop_requested());
    assert(observedReadRows == rowLimit);
    assert(physicalDirectoryPageSizes().size() == (rowLimit == 1 ? 1 : 2));
  }
  const auto retried = loadMusicSelectPhysicalDirectory(
      repository, {}, physicalDirectory(root), {}, {}, temporary.path(), {}, 0);
  assert(retried.provider && retried.provider->size() == 320);
  assert(retried.provider->at(159).chart);
}

}

namespace {

using BenchmarkClock = std::chrono::steady_clock;

struct FirstPageBenchmarkSample {
  double countMillis = 0;
  double firstPageMillis = 0;
  double wrapMillis = 0;
  double totalMillis = 0;
  std::size_t count = 0;
  std::size_t firstVisibleCount = 0;
};

struct BenchmarkSqlObservation {
  int connections = 0;
  std::vector<BenchmarkClock::time_point> richPageStarts;
};

BenchmarkSqlObservation *benchmarkSqlObservation = nullptr;

int traceBenchmarkStatement(unsigned, void *, void *rawStatement, void *) {
  const auto started = BenchmarkClock::now();
  auto *statement = static_cast<sqlite3_stmt *>(rawStatement);
  bool hasPath = false;
  bool hasStageFile = false;
  for (int column = 0; column < sqlite3_column_count(statement); ++column) {
    const std::string_view name = sqlite3_column_name(statement, column);
    hasPath = hasPath || name == "path";
    hasStageFile = hasStageFile || name == "stage_file";
  }
  if (hasPath && hasStageFile) {
    benchmarkSqlObservation->richPageStarts.push_back(started);
  }
  return 0;
}

int observeBenchmarkConnection(sqlite3 *database, char **,
                               const sqlite3_api_routines *) {
  ++benchmarkSqlObservation->connections;
  return sqlite3_trace_v2(database, SQLITE_TRACE_STMT,
                          traceBenchmarkStatement, nullptr);
}

class ScopedBenchmarkSqlObserver {
public:
  explicit ScopedBenchmarkSqlObserver(BenchmarkSqlObservation &observation) {
    assert(benchmarkSqlObservation == nullptr && connectionCount == nullptr);
    benchmarkSqlObservation = &observation;
    sqlite3_reset_auto_extension();
    if (sqlite3_auto_extension(reinterpret_cast<void (*)()>(
            observeBenchmarkConnection)) != SQLITE_OK) {
      benchmarkSqlObservation = nullptr;
      throw std::runtime_error("Unable to observe benchmark SQL");
    }
  }

  ~ScopedBenchmarkSqlObserver() {
    sqlite3_reset_auto_extension();
    benchmarkSqlObservation = nullptr;
  }
};

double benchmarkMillis(BenchmarkClock::time_point begin,
                        BenchmarkClock::time_point end) {
  return std::chrono::duration<double, std::milli>(end - begin).count();
}

FirstPageBenchmarkSample benchmarkFirstPage(
    std::string_view mode, ChartRepository &repository,
    const std::filesystem::path &root, const std::filesystem::path &replayRoot,
    const std::shared_ptr<const ScoreBestCache> &best,
    const std::shared_ptr<const ScoreClearRankCache> &clears) {
  constexpr std::size_t pageSize = 128;
  const auto directory = physicalDirectory(root);
  FirstPageBenchmarkSample sample;
  BenchmarkSqlObservation observation;
  ScopedBenchmarkSqlObserver observer(observation);
  const auto started = BenchmarkClock::now();
  if (mode == "main") {
    auto session = repository.OpenSession();
    if (!session) throw std::runtime_error("Unable to open benchmark session");
    ChartMetaQuery query;
    query.limit = pageSize;
    const int count = session->CountChartMeta(query);
    if (count <= 0) throw std::runtime_error("Benchmark count failed");
    sample.count = static_cast<std::size_t>(count);
    const auto counted = BenchmarkClock::now();
    std::vector<ChartMetaRecord> first;
    session->QueryChartMeta(query, first);
    const auto firstLoaded = BenchmarkClock::now();
    sample.firstVisibleCount = first.size();
    query.offset = static_cast<int>((sample.count - 1) / pageSize * pageSize);
    std::vector<ChartMetaRecord> wrapped;
    if (query.offset != 0) session->QueryChartMeta(query, wrapped);
    const auto finished = BenchmarkClock::now();
    if (first.size() != std::min(pageSize, sample.count) ||
        (query.offset != 0 && wrapped.size() != sample.count - query.offset)) {
      throw std::runtime_error("MainMenu benchmark returned an incomplete page");
    }
    sample.countMillis = benchmarkMillis(started, counted);
    sample.firstPageMillis = benchmarkMillis(counted, firstLoaded);
    sample.wrapMillis = benchmarkMillis(firstLoaded, finished);
    sample.totalMillis = benchmarkMillis(started, finished);
  } else {
    std::shared_ptr<MusicSelectRowProvider> provider;
    if (mode == "indexed") {
      auto opened = repository.OpenSession();
      if (!opened) throw std::runtime_error("Unable to open benchmark session");
      auto session = std::make_shared<ChartRepository::Session>(std::move(*opened));
      if (!session->HasChartMetaForFolderOrParentFolder(root)) {
        throw std::runtime_error("Benchmark folder probe failed");
      }
      MusicSelectRepositoryProjectionInput input;
      input.scoreFor = [best](const bms_parser::ChartMeta &meta, int mode) {
        return best->bestFor(meta, mode);
      };
      input.clearFor = [clears](const bms_parser::ChartMeta &meta, int mode) {
        return clears->bestRankFor(meta, mode);
      };
      input.replayExistsFor = [replayRoot](const ChartMetaRecord &record, int mode) {
        return musicSelectExistingChartReplaySlots(record, mode, replayRoot);
      };
      MusicSelectSongIndex index(directory.id.value);
      session->VisitChartMetaSelection(root, [&](const ChartMetaRecord &record) {
        index.add(record, input.scoreFor(record.meta, 0),
                  input.clearFor(record.meta, 0));
      });
      index.finish();
      index.configure("ALL", "ALL", "TITLE");
      auto indexed = std::make_shared<MusicSelectPagedSongs>(
          std::move(index),
          [session](std::span<const std::filesystem::path> paths) {
            return session->SelectChartMetaByPaths(paths);
          },
          [context = directory.id.value, input](const ChartMetaRecord &record) {
            return MusicSelectRepositoryProjection::projectSong(record, context, input);
          });
      if (indexed->size() != 0) {
        (void)indexed->at(0);
        (void)indexed->at(indexed->size() - 1);
      }
      if (!indexed->diagnostic().empty()) {
        throw std::runtime_error(indexed->diagnostic());
      }
      provider = std::move(indexed);
    } else {
      provider = loadMusicSelectPhysicalDirectory(
          repository, {}, directory, best, clears, replayRoot, {}, 0).provider;
    }
    const auto finished = BenchmarkClock::now();
    if (!provider || provider->size() == 0) {
      throw std::runtime_error("Selector benchmark returned no songs");
    }
    sample.count = provider->size();
    sample.firstVisibleCount = std::min(pageSize, sample.count);
    if (observation.richPageStarts.size() != (sample.count > pageSize ? 2 : 1)) {
      throw std::runtime_error("Selector benchmark did not execute bounded rich pages");
    }
    const auto firstStarted = observation.richPageStarts.front();
    const auto wrapStarted = observation.richPageStarts.size() == 2
        ? observation.richPageStarts.back() : finished;
    sample.countMillis = benchmarkMillis(started, firstStarted);
    sample.firstPageMillis = benchmarkMillis(firstStarted, wrapStarted);
    sample.wrapMillis = benchmarkMillis(wrapStarted, finished);
    sample.totalMillis = benchmarkMillis(started, finished);
    for (std::size_t position = 0; position < sample.firstVisibleCount; ++position) {
      if (!provider->at(position).chart) {
        throw std::runtime_error("Selector benchmark first page was not hydrated");
      }
    }
    if (!provider->at(sample.count - 1).chart) {
      throw std::runtime_error("Selector benchmark wrap page was not hydrated");
    }
  }
  if (observation.connections != 1) {
    throw std::runtime_error("Benchmark must open exactly one fresh SQLite connection");
  }
  return sample;
}

void printFirstPageBenchmark(std::string_view mode, std::string_view pass,
                             const FirstPageBenchmarkSample &sample) {
  std::cout << std::fixed << std::setprecision(3)
            << "mode=" << mode << " pass=" << pass
            << " count=" << sample.count
            << " first_visible=" << sample.firstVisibleCount
            << " count_prepare_ms=" << sample.countMillis
            << " first_page_ms=" << sample.firstPageMillis
            << " wrap_ms=" << sample.wrapMillis
            << " total_ms=" << sample.totalMillis << std::endl;
}

int runFirstPageBenchmark(int argc, char **argv) {
  if (argc != 4) throw std::invalid_argument(
      "Usage: chart_repository_tests --benchmark-first-page main|indexed|sql|all COUNT");
  const std::string mode = argv[2];
  if (mode != "main" && mode != "indexed" && mode != "sql" && mode != "all") {
    throw std::invalid_argument("Unknown first-page benchmark mode");
  }
  std::size_t parsed = 0;
  const auto requested = std::stoll(argv[3], &parsed);
  if (parsed != std::string_view(argv[3]).size() || requested <= 0 ||
      requested > std::numeric_limits<int>::max()) {
    throw std::invalid_argument("COUNT must be a positive SQLite page-range integer");
  }
  TempDirectory temporary;
  ChartRepository repository(temporary.path() / "chart.db");
  if (!repository.EnsureReady()) throw std::runtime_error("Unable to seed benchmark DB");
  const auto root = temporary.path() / "songs";
  seedPhysicalDirectoryPages(repository, root, static_cast<int>(requested));
  {
    auto database = openDatabase(repository.DatabasePath());
    if (!database || !execute(database.get(),
        "UPDATE chart_meta SET title=printf('Song %010d', "
        "(rowid * 1103515245 + 12345) % 2147483647)")) {
      throw std::runtime_error("Unable to seed pseudorandom benchmark titles");
    }
  }
  const auto best = std::make_shared<const ScoreBestCache>();
  const auto clears = std::make_shared<const ScoreClearRankCache>();
  const std::vector<std::string> modes = mode == "all"
      ? std::vector<std::string>{"main", "indexed", "sql"}
      : std::vector<std::string>{mode};
  std::vector<std::vector<FirstPageBenchmarkSample>> samples(modes.size());
  std::cout << "SQLite first-page benchmark: same seeded DB; OS filesystem warm from seed; "
               "new SQLite connection each pass; six runs per mode; empty immutable score caches.\n"
               "count_prepare includes session open and count (indexed: full visitor/index/sort; "
               "sql: probe/resolve/count); selector page phases include SQL/decode/projectSong/"
               "replay existence checks; main pages are QueryChartMeta only; wrap aligned to 128.\n";
  for (int pass = 0; pass < 6; ++pass) {
    for (std::size_t modeIndex = 0; modeIndex < modes.size(); ++modeIndex) {
      auto sample = benchmarkFirstPage(modes[modeIndex], repository, root,
                                        temporary.path() / "profile", best, clears);
      if (sample.count != static_cast<std::size_t>(requested)) {
        throw std::runtime_error("Benchmark modes must select the entire identical dataset");
      }
      samples[modeIndex].push_back(sample);
      printFirstPageBenchmark(modes[modeIndex],
          pass == 0 ? "first" : "warm-" + std::to_string(pass), sample);
    }
  }
  for (std::size_t modeIndex = 0; modeIndex < modes.size(); ++modeIndex) {
    auto median = samples[modeIndex].front();
    for (auto member : {&FirstPageBenchmarkSample::countMillis,
                        &FirstPageBenchmarkSample::firstPageMillis,
                        &FirstPageBenchmarkSample::wrapMillis,
                        &FirstPageBenchmarkSample::totalMillis}) {
      std::vector<double> values;
      for (std::size_t pass = 1; pass < samples[modeIndex].size(); ++pass) {
        values.push_back(samples[modeIndex][pass].*member);
      }
      std::ranges::sort(values);
      median.*member = values[values.size() / 2];
    }
    printFirstPageBenchmark(modes[modeIndex], "warm-median-5", median);
  }
  return 0;
}

}

void testSelectorQueryRejectsStaleSnapshots() {
  TempDirectory temporary;
  ChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  const auto root = temporary.path() / "songs";
  seedPhysicalDirectoryPages(repository, root);
  auto session = repository.OpenSession();
  assert(session);
  ChartSelectorQuery query{.recursiveFolder = root};
  assert(session->ResolveChartSelectorQuery(query) == 320);
  auto other = openDatabase(repository.DatabasePath());
  assert(execute(other.get(), "UPDATE chart_meta SET title='Changed title' "
      "WHERE sha256='" + physicalChartHash(160) + "'"));
  const auto rejected = [](auto operation) {
    bool threw = false;
    try { operation(); } catch (const std::runtime_error &) { threw = true; }
    assert(threw);
  };
  rejected([&] { session->SelectChartSelectorPage(query, 128, 128); });
  rejected([&] {
    session->FindChartSelectorIndex(query, "sha256:" + physicalChartHash(160));
  });
  assert(session->ResolveChartSelectorQuery(query) == 320);
  assert(session->SelectChartSelectorPage(query, 128, 128).size() == 128);
  assert(session->SetSongReviewFavorite(physicalChartHash(160), 4));
  rejected([&] { session->SelectChartSelectorPage(query, 128, 128); });
  assert(session->ResolveChartSelectorQuery(query) == 319);
  assert(session->SelectChartSelectorPage(query, 128, 128).size() == 128);
}

void testSelectorPathIdentityUsesNormalizedStoredAliases() {
  TempDirectory temporary;
  ChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  const auto root = temporary.path() / "songs";
  auto database = openDatabase(repository.DatabasePath());
  assert(execute(database.get(),
      "INSERT INTO chart_meta(path,folder,sha256,md5,title) VALUES ('" +
      fspath_to_utf8(root / "alias" / ".." / "song.bms") + "','" +
      fspath_to_utf8(root) + "','','','Song')"));
  auto session = repository.OpenSession();
  assert(session);
  ChartSelectorQuery query{.recursiveFolder = root};
  assert(session->ResolveChartSelectorQuery(query) == 1);
  const auto records = session->SelectChartSelectorPage(query, 0, 1);
  assert(records.size() == 1);
  const auto identity = "path:" +
      fspath_to_utf8(records.front().meta.BmsPath.lexically_normal());
  const auto found = session->FindChartSelectorIndex(query, identity);
  assert(found && *found == 0);
}

int main(int argc, char **argv) {
  if (argc > 1) {
    try {
      if (argc == 2 && std::string_view(argv[1]) == "--duration-compat-test") {
        testPhysicalDirectoryDurationOverflowMatchesLegacyIndex();
        return 0;
      }
      if (argc == 2 && std::string_view(argv[1]) == "--duration-stale-identity-test") {
        testPhysicalDirectoryDurationFallbackRejectsReplacedIdentity();
        return 0;
      }
      if (argc == 2 && std::string_view(argv[1]) == "--sql-snapshot-tests") {
        testSelectorQueryRejectsStaleSnapshots();
        return 0;
      }
      if (argc == 2 && std::string_view(argv[1]) == "--sql-path-identity-test") {
        testSelectorPathIdentityUsesNormalizedStoredAliases();
        return 0;
      }
      if (std::string_view(argv[1]) != "--benchmark-first-page") {
        throw std::invalid_argument("Unknown chart repository test argument");
      }
      return runFirstPageBenchmark(argc, argv);
    } catch (const std::exception &error) {
      std::cerr << error.what() << '\n';
      return 1;
    }
  }
  testPhysicalDirectoryFirstPageDoesNotVisitWholeFolder();
  testSelectorQueryRejectsStaleSnapshots();
  testSelectorPathIdentityUsesNormalizedStoredAliases();
  testPhysicalDirectoryDurationOverflowMatchesLegacyIndex();
  testPhysicalDirectoryDurationFallbackRejectsReplacedIdentity();
  testPhysicalDirectoryPrimingInterruptsTheActiveRichBatch();
  testRawPhysicalFolderInventoryRestoresCategoryBranches();
  testOwnFolderProbeMatchesRecursiveWindowsDelimiters();
  testPhysicalDirectoryCategoriesAndEmptyFoldersUseOnlyMetadata();
  testPhysicalDirectoryOwnAndMixedSongsMatchRawSubtree();
  testPhysicalDirectoryPagesOwnWorkerSessionAndRichProjectionInputs();
  testPhysicalDirectoryCancellationAcrossProbeCountAndPriming();
  testPhysicalDirectoryStorageFailuresThrowAndRetry();
  testPhysicalDirectoryAutoplayKeepsRawOrderHiddenAndWrongModeSongs();
  testPhysicalDirectoryAutoplayCategoriesAndCancellation();
  testStreamingSelectionMatchesRawRowsWithNarrowPayload();
  testOwnOrImmediateChildFolderProbe();
  testStreamingSelectionFailuresThrowAndReleaseStatement();
  testDirectoryRecordsIncludeDirectChartsAndRawDescendants();
  testMetadataFolderMergePreservesNormalizedDuplicatesAndScales();
  testRawExactFolderKeepsNonpreferredDuplicate();
  testScanBatchCommitAndRollback();
  testScanBatchRetainsSessionStorage();
  testScanBatchUpsertPreservesExistingAddDate();
  testScanBatchReusesPreparedInsertAndTransaction();
  testSessionRoundTripAndReadinessCost();
  testSelectChartMetaByPathsHydratesInInputOrder();
  testFavoriteToggleMaintainsSongReviewChartBit();
  testSongReviewFavoritePersistsExactSourceBitfield();
  testSelectChartMetaByHashUsesDurableIndexedIdentity();
  testRejectedFamiliesRemainUnchanged();
  testChartQueryBehaviorMatrix();
  testDifficultyEntryDownloadUrlsFollowTheirSourceRows();
  testExactFolderQuery();
  testFolderProbeAndCancelledReadsDoNotPoisonSession();
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
