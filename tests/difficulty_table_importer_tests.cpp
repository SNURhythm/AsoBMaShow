#include "../src/DifficultyTableImporter.h"
#include "../src/DifficultyTableModel.h"
#include "../src/repositories/ChartRepository.h"
#include "../src/sqlite3.h"
#include "../yoga/lib/nlohmann/json.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

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
      "\"constraint\":[\"gauge_lr2\"],"
      "\"trophy\":[{\"name\":\"bronzemedal\",\"missrate\":7.5,"
      "\"scorerate\":55.0},{\"name\":\"invalid\",\"missrate\":0,"
      "\"scorerate\":50.0}],\"charts\":[{\"sha256\":\"" +
      sha256 + "\"}]}]}";
  std::string dataJson = "[{\"level\":\"1\",\"md5\":\"" + md5 +
                         "\",\"sha256\":\"" + sha256 +
                         "\",\"title\":\"Chart\",\"subtitle\":\"Sub\","
                         "\"artist\":\"Artist\",\"subartist\":\"Subartist\","
                         "\"url\":\"chart.zip\",\"url_diff\":\"patch.zip\","
                         "\"org_md5\":[\"11111111111111111111111111111111\","
                         "\"22222222222222222222222222222222\"]}]";
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

#if !defined(_WIN32)
class LoopbackTableServer {
public:
  LoopbackTableServer(std::string header, std::string data,
                      bool contentLength)
      : header_(std::move(header)), data_(std::move(data)),
        contentLength_(contentLength) {
    listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(listener_ >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(::bind(listener_, reinterpret_cast<sockaddr *>(&address),
                  sizeof(address)) == 0);
    assert(::listen(listener_, 4) == 0);
    socklen_t addressSize = sizeof(address);
    assert(::getsockname(listener_, reinterpret_cast<sockaddr *>(&address),
                         &addressSize) == 0);
    url_ = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port)) +
           "/header.json";
    worker_ = std::jthread([this](std::stop_token stop) {
      while (!stop.stop_requested()) {
        pollfd pending{listener_, POLLIN, 0};
        if (::poll(&pending, 1, 50) <= 0) continue;
        const int client = ::accept(listener_, nullptr, nullptr);
        if (client < 0) return;
#if defined(__APPLE__)
        const int noSigPipe = 1;
        ::setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &noSigPipe,
                      sizeof(noSigPipe));
#endif
        const timeval timeout{2, 0};
        ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        std::string request;
        char buffer[4096];
        while (request.find("\r\n\r\n") == std::string::npos &&
               request.size() < sizeof(buffer)) {
          const auto received = ::recv(client, buffer, sizeof(buffer), 0);
          if (received <= 0) break;
          request.append(buffer, static_cast<std::size_t>(received));
        }
        const auto &body = request.starts_with("GET /data.json ")
                               ? data_ : header_;
        std::string response = "HTTP/1.1 200 OK\r\nConnection: close\r\n";
        if (contentLength_) {
          response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        }
        response += "\r\n";
        if (sendAll(client, response)) sendAll(client, body);
        ::close(client);
      }
    });
  }

  ~LoopbackTableServer() {
    worker_.request_stop();
    worker_.join();
    ::close(listener_);
  }

  const std::string &url() const { return url_; }

private:
  static bool sendAll(int client, std::string_view bytes) {
    while (!bytes.empty()) {
#if defined(MSG_NOSIGNAL)
      constexpr int flags = MSG_NOSIGNAL;
#else
      constexpr int flags = 0;
#endif
      const auto sent = ::send(client, bytes.data(),
                               std::min(bytes.size(), std::size_t{4096}), flags);
      if (sent <= 0) return false;
      bytes.remove_prefix(static_cast<std::size_t>(sent));
    }
    return true;
  }

  int listener_ = -1;
  std::string header_;
  std::string data_;
  bool contentLength_;
  std::string url_;
  std::jthread worker_;
};

void testDesktopDownloadsEnforceIncrementalResponseBudget() {
  constexpr std::size_t responseBudget = 16 * 1024 * 1024;
  for (const bool oversizedHeader : {false, true}) {
    for (const auto responseSize :
         {responseBudget - 1, responseBudget, responseBudget + 1}) {
      Fixture fixture;
      auto &padded = oversizedHeader ? fixture.headerJson : fixture.dataJson;
      padded.resize(responseSize, ' ');
      LoopbackTableServer server(fixture.headerJson, fixture.dataJson,
                                 responseSize < responseBudget);
      TempDirectory temporary;
      ChartRepository repository(temporary.path() / "chart.db");
      assert(repository.EnsureReady());
      auto session = repository.OpenSession();
      assert(session.has_value());
      DifficultyTableImporter importer;
      std::string error;
      const bool imported = importer.ImportFromUrl(*session, server.url(), &error);
      if (responseSize > responseBudget) {
        assert(!imported);
        assert(error.find("16 MiB") != std::string::npos);
        assert(session->SelectDifficultyTables().empty());
      } else {
        assert(imported);
        assert(session->SelectDifficultyTables().size() == 1);
      }
    }
  }
}
#endif

void testLegacyHtmlDiscoversUtf8TableAndRejectsInvalidJson() {
  TempDirectory temporary;
  ChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());
  Fixture fixture;
  const std::string pageUrl = "http://example.test/legacy/pms.html";
  const std::string headerUrl = "https://example.test/table/header.json";
  // Shift_JIS title, as served by stellawingroad.web.fc2.com/new/pms.html.
  const std::string page =
      "<html><head><title>\x94\xad\x8b\xb6PMS\x93\xef\x88\xd5\x93x</title>"
      "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=Shift_JIS\">"
      "<meta name=\"bmstable\" content=\"" + headerUrl + "\"></head></html>";
  auto header = nlohmann::json::parse(fixture.headerJson);
  header["name"] = "発狂PMS難易度";
  fixture.headerJson = header.dump();
  DifficultyTableImporter importer(
      [&](const std::string &url, std::string *) -> std::optional<std::string> {
        if (url == pageUrl) return page;
        if (url == headerUrl) return fixture.headerJson;
        if (url == "https://example.test/table/data.json") return fixture.dataJson;
        return std::nullopt;
      });
  std::string error;
  assert(importer.ImportFromUrl(*session, pageUrl, &error));
  const auto tables = session->SelectDifficultyTables();
  assert(tables.size() == 1);
  assert(tables.front().name == "発狂PMS難易度");
  assert(tables.front().chartCount == 1);
  const auto before = snapshotDifficultyTables(repository.DatabasePath());
  fixture.dataJson = "[{\"title\":\"\xc0\xaf\"}]";
  assert(!importer.ImportFromUrl(*session, pageUrl, &error));
  assert(!error.empty());
  assert(snapshotDifficultyTables(repository.DatabasePath()) == before);
}

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
  assert(parsed->charts.front().originalMd5s ==
         std::optional<std::vector<std::string>>({
             "11111111111111111111111111111111",
             "22222222222222222222222222222222"}));
  assert(parsed->courses.size() == 1);
  assert(parsed->courses.front().groupName == "Course");
  assert(parsed->courses.front().level == "*1");
  assert(parsed->courses.front().constraintJson == "[\"gauge_lr2\"]");
  assert(parsed->courses.front().trophies.size() == 1);
  assert(parsed->courses.front().trophies.front().name == "bronzemedal");
  assert(parsed->courses.front().trophies.front().missRate == 7.5);
  assert(parsed->courses.front().trophies.front().scoreRate == 55.0);
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

void testGenocideLegacyUrlsImportAndUpdateMirrorTables() {
  for (const std::string kind : {"normal", "insane"}) {
    TempDirectory temporary;
    ChartRepository repository(temporary.path() / "chart.db");
    assert(repository.EnsureReady());
    auto session = repository.OpenSession();
    assert(session.has_value());
    Fixture fixture;
    const std::string legacy =
        "https://nekokan.dyndns.info/~lobsak/genocide/" + kind + ".html";
    const std::string directory =
        "https://miraiscarlet.github.io/bms/table/genocide_" + kind + "/";
    const std::string mirror = directory + kind + "_bms.html";
    const std::string listUrl = "https://example.test/tables.json";
    std::string list = nlohmann::json::array({legacy, mirror}).dump();
    bool failData = false;
    DifficultyTableImporter importer(
        [&](const std::string &url, std::string *) -> std::optional<std::string> {
          if (url == listUrl) return list;
          if (url == mirror) {
            return "<meta name=\"bmstable\" content=\"header_" + kind +
                   ".json\">";
          }
          if (url == directory + "header_" + kind + ".json") {
            return fixture.headerJson;
          }
          if (url == directory + "data.json" && !failData) {
            return fixture.dataJson;
          }
          return std::nullopt;
        });
    std::string error;
    assert(importer.ImportFromUrl(*session, legacy, &error));
    auto tables = session->SelectDifficultyTables();
    assert(tables.size() == 1);
    assert(tables.front().sourceUrl == mirror);
    assert(tables.front().chartCount == 1);
    const int tableId = tables.front().id;

    // Both schemes and page anchors resolve to the same stored source.
    assert(importer.ImportFromUrl(
        *session, " HTTP://NEKOKAN.DYNDNS.INFO/~lobsak/genocide/" + kind +
                      ".html?view=all#1 ", &error));
    assert(session->SelectDifficultyTables().size() == 1);
    assert(importer.UpdateFromSourceUrl(*session, tableId, &error));
    assert(session->SelectDifficultyTables().front().id == tableId);
    const auto before = snapshotDifficultyTables(repository.DatabasePath());
    failData = true;
    assert(!importer.ImportFromUrl(*session, legacy, &error));
    assert(snapshotDifficultyTables(repository.DatabasePath()) == before);
    failData = false;

    assert(importer.ImportFromUrl(*session, listUrl, &error));
    assert(error == "Imported 0, skipped 1 of 1 tables.");
    assert(session->DeleteDifficultyTable(tableId));
    assert(importer.ImportFromUrl(*session, listUrl, &error));
    assert(error == "Imported 1, skipped 0 of 1 tables.");
    assert(session->SelectDifficultyTables().size() == 1);
    assert(session->SelectDifficultyTables().front().sourceUrl == mirror);
    assert(session->DeleteDifficultyTable(
        session->SelectDifficultyTables().front().id));
    // HTTPS lists may link the HTTP original, but only fetch its HTTPS mirror.
    list = nlohmann::json::array({"http://nekokan.dyndns.info/~lobsak/genocide/" +
                                 kind + ".html"}).dump();
    assert(importer.ImportFromUrl(*session, listUrl, &error));
    assert(session->SelectDifficultyTables().size() == 1);
    assert(session->SelectDifficultyTables().front().sourceUrl == mirror);
  }
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

void testHttpsTableRejectsInsecureDataUrl() {
  auto rejects = [](const std::string &sourceUrl,
                    const std::string &insecureDataUrl) {
    TempDirectory temporary;
    ChartRepository repository(temporary.path() / "chart.db");
    assert(repository.EnsureReady());
    auto session = repository.OpenSession();
    assert(session.has_value());

    std::vector<std::string> requestedUrls;
    DifficultyTableImporter importer(
        [&](const std::string &url,
            std::string *) -> std::optional<std::string> {
          requestedUrls.push_back(url);
          if (url == sourceUrl) {
            return "{\"name\":\"Mixed Content\",\"symbol\":\"M\","
                   "\"data_url\":\"" +
                   insecureDataUrl + "\"}";
          }
          if (url == insecureDataUrl) {
            return "[]";
          }
          return std::nullopt;
        });

    std::string error;
    assert(!importer.ImportFromUrl(*session, sourceUrl, &error));
    assert((requestedUrls == std::vector<std::string>{sourceUrl}));
    assert(error == "HTTPS difficulty tables cannot load data over HTTP");
    assert(session->SelectDifficultyTables().empty());
  };

  rejects("https://example.test/table/header.json",
          "http://example.test/table/data.json");
  rejects("HTTPS://example.test/table/header.json",
          "http://example.test/table/data.json");
  rejects("https://example.test/table/header.json",
          "HtTp://example.test/table/data.json");
}

void testCheckpointInterruptsAnUpdateBetweenDownloadStages() {
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
        return url == fixture.sourceUrl
                   ? std::optional<std::string>{fixture.headerJson}
                   : std::optional<std::string>{fixture.dataJson};
      });
  int checkpoints = 0;
  std::string error;
  assert(!importer.ImportFromUrl(
      *session, fixture.sourceUrl, &error, nullptr,
      [&] { return ++checkpoints < 4; }));
  assert(checkpoints == 4);
  assert(requestedUrls == std::vector<std::string>{fixture.sourceUrl});
  assert(error == "Difficulty table update was interrupted");
  assert(session->SelectDifficultyTables().empty());
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

void testBundledDefaultsSurviveOfflineAndYieldToUpdates() {
  TempDirectory temporary;
  ChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());
  Fixture fixture;
  const auto assetPath = temporary.path() / "defaults.json";
  const nlohmann::json snapshot = {
      {"tables", {{{"source_url", fixture.sourceUrl},
                    {"header", nlohmann::json::parse(fixture.headerJson)},
                    {"data", nlohmann::json::parse(fixture.dataJson)}}}}};
  std::ofstream(assetPath) << snapshot.dump();
  bool online = false;
  DifficultyTableImporter importer(
      [&](const std::string &url, std::string *) -> std::optional<std::string> {
        if (!online) return std::nullopt;
        if (url == fixture.sourceUrl) return fixture.headerJson;
        return fixture.dataJson;
      });
  assert(importer.SeedBundledDefaults(*session, assetPath.string()) == 1);
  const auto originalId = session->SelectDifficultyTables().front().id;
  const auto before = snapshotTable(repository.DatabasePath(), "difficulty_tables");
  assert(!importer.ImportFromUrl(*session, fixture.sourceUrl));
  assert(snapshotTable(repository.DatabasePath(), "difficulty_tables") == before);
  assert(importer.SeedBundledDefaults(*session, assetPath.string()) == 0);
  online = true;
  auto header = nlohmann::json::parse(fixture.headerJson);
  header["name"] = "Updated Table";
  fixture.headerJson = header.dump();
  assert(importer.ImportFromUrl(*session, fixture.sourceUrl));
  assert(importer.SeedBundledDefaults(*session, assetPath.string()) == 0);
  const auto tables = session->SelectDifficultyTables();
  assert(tables.size() == 1);
  assert(tables.front().name == "Updated Table");
  assert(tables.front().id == originalId);
  assert(tables.front().chartCount == 1);
  assert(importer.SeedBundledDefaults(*session, "missing-snapshot.json") == 0);
  std::ofstream(assetPath) << "invalid json";
  assert(importer.SeedBundledDefaults(*session, assetPath.string()) == 0);
}

void testPackagedDefaultsImportWithoutNetwork() {
  TempDirectory temporary;
  ChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());
  DifficultyTableImporter importer([](const std::string &, std::string *)
      -> std::optional<std::string> {
    assert(false && "Bundled defaults must never access the network");
    return std::nullopt;
  });
  assert(importer.SeedBundledDefaults(*session) == 8);
  const auto tables = session->SelectDifficultyTables();
  for (const std::string kind : {"normal", "insane"}) {
    const std::string source =
        "https://miraiscarlet.github.io/bms/table/genocide_" + kind + "/" +
        kind + "_bms.html";
    assert(std::any_of(tables.begin(), tables.end(), [&](const auto &table) {
      return table.sourceUrl == source && table.chartCount > 0;
    }));
  }
  for (const auto &source : {"https://asumatoki.kr/table/aery/header.json",
                              "https://asumatoki.kr/table/aery7/header.json"}) {
    assert(std::any_of(tables.begin(), tables.end(), [&](const auto &table) {
      return table.sourceUrl == source && table.chartCount > 0;
    }));
  }
  for (const auto &table : tables) {
    assert(!table.name.empty());
    assert(table.sourceUrl.starts_with("https://"));
    assert(table.chartCount > 0);
  }
  assert(importer.SeedBundledDefaults(*session) == 0);
  for (const auto &table : tables) assert(session->DeleteDifficultyTable(table.id));
  assert(importer.SeedBundledDefaults(*session, "assets/difficulty-tables/defaults.json",
      {"https://asumatoki.kr/table/aery/header.json",
       "https://asumatoki.kr/table/aery7/header.json"}) == 2);
  assert(session->SelectDifficultyTables().size() == 2);
}

void testSharedSeedStatePreservesDeletedTables() {
  TempDirectory temporary;
  ChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  ApplicationUiState applicationState;
  applicationState.defaultDifficultyTablesSeeded = true;
  DifficultyTableImporter importer;
  {
    auto session = repository.OpenSession();
    assert(session);
    assert(importer.SeedBundledDefaultsForApplication(*session, applicationState));
    assert(applicationState.aeryDifficultyTablesSeeded);
    const auto tables = session->SelectDifficultyTables();
    assert(tables.size() == 2);
    for (const auto &table : tables) assert(session->DeleteDifficultyTable(table.id));
  }
  // Reopening the shared database under another profile uses the same state.
  auto session = repository.OpenSession();
  assert(session);
  assert(!importer.SeedBundledDefaultsForApplication(*session, applicationState));
  assert(session->SelectDifficultyTables().empty());
}

int main() {
  testGenocideLegacyUrlsImportAndUpdateMirrorTables();
  testLegacyHtmlDiscoversUtf8TableAndRejectsInvalidJson();
  testBundledDefaultsSurviveOfflineAndYieldToUpdates();
  testPackagedDefaultsImportWithoutNetwork();
  testSharedSeedStatePreservesDeletedTables();
#if !defined(_WIN32)
  testDesktopDownloadsEnforceIncrementalResponseBudget();
#endif
  testParseAndReplacementRollback();
  testInjectedFetcherAndProgress();
  testHttpsTableRejectsInsecureDataUrl();
  testCheckpointInterruptsAnUpdateBetweenDownloadStages();
  testListImportKeepsBoundedConcurrencyAndSkipsExistingSources();
  return 0;
}
