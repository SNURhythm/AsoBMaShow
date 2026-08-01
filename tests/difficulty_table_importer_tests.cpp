#include "../src/DifficultyTableImporter.h"
#include "../src/DifficultyTableModel.h"
#include "../src/repositories/ChartRepository.h"
#include "../src/sqlite3.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace {

class TempDirectory {
public:
  TempDirectory() {
    static std::atomic<unsigned long long> sequence{0};
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("asobmashow-difficulty-importer-" + std::to_string(nonce) + "-" +
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

struct Fixture {
  std::string sha256 = std::string(64, 'a');
  std::string md5 = std::string(32, 'b');
  std::string sourceUrl = "https://example.test/table/header.json";
  std::string headerJson =
      "{\"name\":\"Test Table\",\"symbol\":\"*\","
      "\"data_url\":\"data.json\",\"level_order\":[\"1\"],"
      "\"course\":[{\"name\":\"Course *1\","
      "\"constraint\":[\"gauge_lr2\"],\"charts\":[{\"sha256\":\"" +
      sha256 + "\"}]}]}";
  std::string dataJson = "[{\"level\":\"1\",\"md5\":\"" + md5 +
                         "\",\"sha256\":\"" + sha256 +
                         "\",\"title\":\"Chart\",\"subtitle\":\"Sub\","
                         "\"artist\":\"Artist\",\"subartist\":\"Subartist\","
                         "\"url\":\"chart.zip\",\"url_diff\":\"patch.zip\"}]";
};

std::string snapshotTable(const std::filesystem::path &databasePath,
                          const char *table) {
  sqlite3 *database = nullptr;
  assert(sqlite3_open_v2(databasePath.string().c_str(), &database,
                         SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
  const std::string query =
      std::string("SELECT * FROM ") + table + " ORDER BY rowid";
  sqlite3_stmt *statement = nullptr;
  assert(sqlite3_prepare_v2(database, query.c_str(), -1, &statement, nullptr) ==
         SQLITE_OK);

  std::string snapshot;
  while (sqlite3_step(statement) == SQLITE_ROW) {
    const int columnCount = sqlite3_column_count(statement);
    for (int column = 0; column < columnCount; ++column) {
      const int type = sqlite3_column_type(statement, column);
      const int size = sqlite3_column_bytes(statement, column);
      snapshot += std::to_string(type) + ":" + std::to_string(size) + ":";
      if (const auto *value = sqlite3_column_text(statement, column)) {
        snapshot.append(reinterpret_cast<const char *>(value), size);
      }
      snapshot.push_back('|');
    }
    snapshot.push_back('\n');
  }
  sqlite3_finalize(statement);
  sqlite3_close(database);
  return snapshot;
}

std::vector<std::string>
snapshotDifficultyTables(const std::filesystem::path &databasePath) {
  return {
      snapshotTable(databasePath, "difficulty_tables"),
      snapshotTable(databasePath, "difficulty_table_entries"),
      snapshotTable(databasePath, "difficulty_courses"),
      snapshotTable(databasePath, "difficulty_course_entries"),
  };
}

std::atomic_bool denyCourseEntryInsert{false};

int denyCourseEntryInsertAuthorizer(void *, int action, const char *first,
                                    const char *, const char *, const char *) {
  if (denyCourseEntryInsert.load(std::memory_order_relaxed) &&
      action == SQLITE_INSERT && first != nullptr &&
      std::string(first) == "difficulty_course_entries") {
    return SQLITE_DENY;
  }
  return SQLITE_OK;
}

int installCourseEntryAuthorizer(sqlite3 *database, char **,
                                 const sqlite3_api_routines *) {
  return sqlite3_set_authorizer(database, denyCourseEntryInsertAuthorizer,
                                nullptr);
}

class ScopedCourseEntryDenial {
public:
  ScopedCourseEntryDenial() {
    sqlite3_reset_auto_extension();
    assert(sqlite3_auto_extension(reinterpret_cast<void (*)()>(
               installCourseEntryAuthorizer)) == SQLITE_OK);
  }

  ~ScopedCourseEntryDenial() {
    denyCourseEntryInsert.store(false, std::memory_order_relaxed);
    sqlite3_reset_auto_extension();
  }
};

void testParseAndReplacementRollback() {
  const Fixture fixture;
  std::string error;
  const auto parsed = difficulty_table::Parse(
      fixture.headerJson, fixture.dataJson, fixture.sourceUrl, error);
  assert(parsed.has_value());
  assert(parsed->levelOrder == std::vector<std::string>{"1"});
  assert(parsed->charts.size() == 1);
  assert(parsed->charts.front().subtitle == "Sub");
  assert(parsed->charts.front().subartist == "Subartist");
  assert(parsed->charts.front().urlDiff == "patch.zip");
  assert(parsed->courses.size() == 1);
  assert(parsed->courses.front().groupName == "Course");
  assert(parsed->courses.front().level == "*1");
  assert(parsed->courses.front().constraintJson == "[\"gauge_lr2\"]");
  assert(parsed->courses.front().charts.size() == 1);
  assert(parsed->courses.front().charts.front().md5 == fixture.md5);

  TempDirectory temporary;
  const auto databasePath = temporary.path() / "chart.db";
  ScopedCourseEntryDenial denial;
  ChartRepository repository(databasePath);
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());
  assert(session->ReplaceDifficultyTable(*parsed));
  const auto before = snapshotDifficultyTables(databasePath);

  auto changed = *parsed;
  changed.name = "Changed Table";
  changed.charts.front().title = "Changed Chart";
  denyCourseEntryInsert.store(true, std::memory_order_relaxed);
  assert(!session->ReplaceDifficultyTable(changed));
  denyCourseEntryInsert.store(false, std::memory_order_relaxed);
  assert(snapshotDifficultyTables(databasePath) == before);
}

void testInjectedFetcherAndProgress() {
  const Fixture fixture;
  TempDirectory temporary;
  ChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());

  std::vector<std::string> requestedUrls;
  DifficultyTableImporter importer(
      [&](const std::string &url, std::string *) -> std::optional<std::string> {
        requestedUrls.push_back(url);
        if (url == fixture.sourceUrl) {
          return fixture.headerJson;
        }
        if (url == "https://example.test/table/data.json") {
          return fixture.dataJson;
        }
        return std::nullopt;
      });

  std::vector<DifficultyTableImportProgress> progress;
  std::string error;
  assert(importer.ImportFromUrl(
      *session, fixture.sourceUrl, &error,
      [&](const DifficultyTableImportProgress &value) {
        assert(session->SelectDifficultyTables().empty());
        progress.push_back(value);
      }));
  assert((requestedUrls ==
          std::vector<std::string>{fixture.sourceUrl,
                                   "https://example.test/table/data.json"}));
  assert(progress.size() == 1);
  assert(progress.front().current == 1);
  assert(progress.front().total == 1);
  assert(progress.front().tableName == "Test Table");
  assert(session->SelectDifficultyTables().size() == 1);
}

void testListImportKeepsBoundedConcurrencyAndSkipsExistingSources() {
  TempDirectory temporary;
  ChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());

  const std::string listUrl = "https://example.test/tables.json";
  std::string listJson = "[";
  for (int index = 0; index < 6; ++index) {
    if (index > 0) {
      listJson += ',';
    }
    listJson += "{\"name\":\"Table " + std::to_string(index) +
                "\",\"url\":\"table-" + std::to_string(index) +
                "/header.json\"}";
  }
  listJson += ']';

  std::mutex mutex;
  std::condition_variable ready;
  int activeHeaderFetches = 0;
  int maximumHeaderFetches = 0;
  bool releaseHeaders = false;
  std::vector<std::string> requestedUrls;
  DifficultyTableImporter importer(
      [&](const std::string &url, std::string *) -> std::optional<std::string> {
        {
          std::lock_guard lock(mutex);
          requestedUrls.push_back(url);
        }
        if (url == listUrl) {
          return listJson;
        }
        if (url.ends_with("/header.json")) {
          {
            std::unique_lock lock(mutex);
            ++activeHeaderFetches;
            maximumHeaderFetches =
                std::max(maximumHeaderFetches, activeHeaderFetches);
            if (activeHeaderFetches == 4) {
              releaseHeaders = true;
              ready.notify_all();
            } else {
              ready.wait_for(lock, std::chrono::seconds(1),
                             [&]() { return releaseHeaders; });
            }
            --activeHeaderFetches;
          }
          return "{\"name\":\"" + url +
                 "\",\"symbol\":\"L\",\"data_url\":\"data.json\"}";
        }
        if (url.ends_with("/data.json")) {
          return "[{\"level\":\"1\",\"md5\":\"" + std::string(32, 'c') +
                 "\",\"sha256\":\"" + std::string(64, 'd') + "\"}]";
        }
        return std::nullopt;
      });

  std::string summary;
  assert(importer.ImportFromUrl(*session, listUrl, &summary));
  assert(summary == "Imported 6, skipped 0 of 6 tables.");
  assert(maximumHeaderFetches == 4);
  assert(session->SelectDifficultyTables().size() == 6);

  {
    std::lock_guard lock(mutex);
    requestedUrls.clear();
  }
  summary.clear();
  assert(importer.ImportFromUrl(*session, listUrl, &summary));
  assert(summary == "Imported 0, skipped 6 of 6 tables.");
  {
    std::lock_guard lock(mutex);
    assert(requestedUrls == std::vector<std::string>{listUrl});
  }
}

} // namespace

int main() {
  testParseAndReplacementRollback();
  testInjectedFetcherAndProgress();
  testListImportKeepsBoundedConcurrencyAndSkipsExistingSources();
  return 0;
}
