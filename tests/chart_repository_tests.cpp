#include "../src/repositories/ChartRepository.h"
#include "../src/repositories/SqliteRAII.h"
#include "RepositorySqliteTestSupport.h"

#include <array>
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

std::atomic<int> *connectionCount = nullptr;
std::mutex traceMutex;
std::vector<std::string> tracedStatements;

int traceStatement(unsigned mask, void *, void *statement, void *) {
  if ((mask & SQLITE_TRACE_STMT) == 0 || statement == nullptr) {
    return 0;
  }
  const char *sql = sqlite3_sql(static_cast<sqlite3_stmt *>(statement));
  std::lock_guard lock(traceMutex);
  tracedStatements.emplace_back(sql != nullptr ? sql : "");
  return 0;
}

int observeConnection(sqlite3 *database, char **,
                      const sqlite3_api_routines *) {
  assert(connectionCount != nullptr);
  connectionCount->fetch_add(1, std::memory_order_relaxed);
  sqlite3_trace_v2(database, SQLITE_TRACE_STMT, traceStatement, nullptr);
  return SQLITE_OK;
}

class ScopedConnectionObserver {
public:
  explicit ScopedConnectionObserver(std::atomic<int> &count) {
    assert(connectionCount == nullptr);
    connectionCount = &count;
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

} // namespace

int main() {
  testSessionRoundTripAndReadinessCost();
  testRejectedFamiliesRemainUnchanged();
  return 0;
}
