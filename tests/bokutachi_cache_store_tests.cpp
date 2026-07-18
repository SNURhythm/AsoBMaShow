#include "ir/tachi/BokutachiCacheStore.h"

#include "AtomicFile.h"
#include "nlohmann/json.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view kOrigin = "https://boku.tachi.ac";
constexpr std::string_view kSha =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

class TempDirectory {
public:
  explicit TempDirectory(std::string_view suffix) {
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("asobmashow-bokutachi-cache-" + std::string(suffix) + "-" +
             std::to_string(nonce));
    std::filesystem::create_directories(path_);
  }

  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  [[nodiscard]] const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

void writeFile(const std::filesystem::path &path, std::string_view contents) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

std::string readFile(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

std::string indexedSha256(std::size_t index) {
  std::string result(64, '0');
  const std::string suffix = std::to_string(index);
  result.replace(result.size() - suffix.size(), suffix.size(), suffix);
  return result;
}

void testMissingCachePersistsAndReloadsLookups() {
  TempDirectory temp("roundtrip");
  const auto path = temp.path() / "bokutachi-cache.json";
  std::string diagnostic;
  ir::tachi::BokutachiCacheStore store;
  expect(store.activate(path, diagnostic), "missing cache activates normally");
  expect(!store.userId(kOrigin), "missing cache has no user identity");
  expect(!store.chartId(kOrigin, "bms-7k", kSha),
         "missing cache has no chart mapping");
  expect(store.rememberUserId(kOrigin, 123, diagnostic),
         "user identity is persisted");
  expect(store.rememberChartId(kOrigin, "bms-7k", kSha, "chart-native-7k",
                               diagnostic),
         "chart mapping is persisted");

  ir::tachi::BokutachiCacheStore reloaded;
  expect(reloaded.activate(path, diagnostic), "persisted cache reloads");
  expect(reloaded.userId(kOrigin) == 123, "user identity round trips");
  expect(reloaded.chartId(kOrigin, "bms-7k", kSha) == "chart-native-7k",
         "chart mapping round trips");

  const auto document = nlohmann::json::parse(readFile(path));
  expect(document.at("schemaVersion") == 1, "cache writes schema version one");
  const std::string encoded = document.dump();
  expect(encoded.find("apiKey") == std::string::npos &&
             encoded.find("fresh-api-key") == std::string::npos,
         "cache serialization contains no credential field or material");
}

void testMutationsAreValidatedAndIndependentlyClearable() {
  TempDirectory temp("mutations");
  const auto path = temp.path() / "bokutachi-cache.json";
  std::string diagnostic;
  ir::tachi::BokutachiCacheStore store;
  expect(store.activate(path, diagnostic), "mutation fixture activates");
  expect(!store.rememberUserId("https://boku.tachi.ac/", 5, diagnostic),
         "non-normalized origin is rejected");
  expect(!store.rememberUserId(kOrigin, 0, diagnostic),
         "non-positive user identity is rejected");
  expect(!store.rememberChartId(kOrigin, "bms-9k", kSha, "chart", diagnostic),
         "unsupported BMS game is rejected");
  expect(!store.rememberChartId(
             kOrigin, "bms-7k",
             "ABCDEF6789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
             "chart", diagnostic),
         "non-normalized chart hash is rejected");
  expect(
      !store.rememberChartId(kOrigin, "bms-7k", kSha, "bad chart", diagnostic),
      "unsafe chart identity is rejected");

  expect(store.rememberUserId(kOrigin, 17, diagnostic),
         "valid user identity is accepted");
  expect(store.rememberChartId(kOrigin, "bms-7k", kSha, "chart-17", diagnostic),
         "valid chart identity is accepted");
  expect(store.clearUserIds(diagnostic), "user identities can be cleared");
  expect(!store.userId(kOrigin), "clearing users removes identity lookup");
  expect(store.chartId(kOrigin, "bms-7k", kSha) == "chart-17",
         "clearing users retains chart mappings");
  expect(store.eraseChartId(kOrigin, "bms-7k", kSha, diagnostic),
         "chart mapping can be erased");
  expect(!store.chartId(kOrigin, "bms-7k", kSha),
         "erased chart mapping is absent");
}

void testUnchangedValuesDoNotRewrite() {
  TempDirectory temp("no-op");
  const auto path = temp.path() / "bokutachi-cache.json";
  std::string diagnostic;
  ir::tachi::BokutachiCacheStore store;
  expect(store.activate(path, diagnostic) &&
             store.rememberUserId(kOrigin, 77, diagnostic),
         "no-op fixture writes its first value");
  expect(!std::filesystem::exists(path.string() + ".bak"),
         "first cache write has no prior backup");
  expect(store.rememberUserId(kOrigin, 77, diagnostic),
         "unchanged cache value succeeds");
  expect(!std::filesystem::exists(path.string() + ".bak"),
         "unchanged cache value performs no atomic rewrite");
}

void testDisposableBadDataCanBeReplaced() {
  for (const auto &[name, contents] : {
           std::pair<std::string_view, std::string>{"malformed", "{bad"},
           {"oversized",
            std::string(ir::tachi::BokutachiCacheStore::kMaximumFileBytes + 1,
                        'x')},
       }) {
    TempDirectory temp(name);
    const auto path = temp.path() / "bokutachi-cache.json";
    writeFile(path, contents);
    std::string diagnostic;
    ir::tachi::BokutachiCacheStore store;
    expect(!store.activate(path, diagnostic),
           "malformed or oversized cache reports a diagnostic activation");
    expect(store.rememberUserId(kOrigin, 91, diagnostic),
           "disposable invalid cache can be replaced");
    expect(nlohmann::json::parse(readFile(path)).at("schemaVersion") == 1,
           "replacement is valid versioned JSON");
  }
}

void testFutureCacheIsPreserved() {
  TempDirectory temp("future");
  const auto path = temp.path() / "bokutachi-cache.json";
  const std::string future = R"({"schemaVersion":2,"future":"must-survive"})";
  writeFile(path, future);
  std::string diagnostic;
  ir::tachi::BokutachiCacheStore store;
  expect(!store.activate(path, diagnostic),
         "future cache is unavailable to this version");
  expect(!store.rememberUserId(kOrigin, 92, diagnostic),
         "future cache disables writes");
  expect(store.clearUserIds(diagnostic),
         "an unavailable cache with no loaded identity does not block a "
         "credential change");
  expect(readFile(path) == future, "future cache remains byte-for-byte intact");
}

void testLoadedCollectionsAreBounded() {
  TempDirectory temp("bounds");
  const auto path = temp.path() / "bokutachi-cache.json";
  nlohmann::json origins = nlohmann::json::array();
  for (std::size_t index = 0;
       index < ir::tachi::BokutachiCacheStore::kMaximumOrigins + 1; ++index) {
    origins.push_back(
        {{"serverOrigin", "https://cache-" + std::to_string(index) + ".test"},
         {"charts", nlohmann::json::array()}});
  }
  writeFile(path,
            nlohmann::json{{"schemaVersion", 1}, {"origins", origins}}.dump());
  std::string diagnostic;
  ir::tachi::BokutachiCacheStore store;
  expect(!store.activate(path, diagnostic),
         "over-limit origin collection is discarded");
  expect(store.rememberUserId(kOrigin, 93, diagnostic),
         "discarded over-limit cache can be replaced safely");
}

void testRuntimeChartCollectionEvictsOldestMapping() {
  TempDirectory temp("chart-eviction");
  const auto path = temp.path() / "bokutachi-cache.json";
  nlohmann::json charts = nlohmann::json::array();
  for (std::size_t index = 0;
       index < ir::tachi::BokutachiCacheStore::kMaximumChartMappings;
       ++index) {
    charts.push_back({{"game", "bms-7k"},
                      {"sha256", indexedSha256(index)},
                      {"chartID", "chart-" + std::to_string(index)}});
  }
  writeFile(path,
            nlohmann::json{
                {"schemaVersion", 1},
                {"origins",
                 nlohmann::json::array(
                     {{{"serverOrigin", std::string(kOrigin)},
                       {"charts", std::move(charts)}}})}}
                .dump());
  std::string diagnostic;
  ir::tachi::BokutachiCacheStore store;
  const std::string newestSha(64, 'f');
  expect(store.activate(path, diagnostic) &&
             store.rememberChartId(kOrigin, "bms-7k", newestSha,
                                   "newest-chart", diagnostic),
         "full chart cache accepts one replacement mapping");
  expect(!store.chartId(kOrigin, "bms-7k", indexedSha256(0)),
         "full chart cache evicts its oldest mapping");
  expect(store.chartId(kOrigin, "bms-7k", indexedSha256(1)) == "chart-1" &&
             store.chartId(kOrigin, "bms-7k", newestSha) == "newest-chart",
         "full chart cache retains newer mappings within the bound");
}

void testPersistedCacheNeverExceedsItsReadLimit() {
  TempDirectory temp("encoded-size");
  const auto path = temp.path() / "bokutachi-cache.json";
  const auto makeDocument = [](std::size_t chartIdBytes) {
    nlohmann::json charts = nlohmann::json::array();
    const std::string chartId(chartIdBytes, '"');
    for (std::size_t index = 0;
         index < ir::tachi::BokutachiCacheStore::kMaximumChartMappings;
         ++index) {
      charts.push_back({{"game", "bms-7k"},
                        {"sha256", indexedSha256(index)},
                        {"chartID", chartId}});
    }
    return nlohmann::json{
        {"schemaVersion", 1},
        {"origins",
         nlohmann::json::array({{{"serverOrigin", std::string(kOrigin)},
                                 {"charts", std::move(charts)}}})}};
  };
  std::size_t lower = 1;
  std::size_t upper = ir::tachi::BokutachiCacheStore::kMaximumChartIdBytes;
  std::size_t chartIdBytes = 0;
  while (lower <= upper) {
    const std::size_t candidate = lower + (upper - lower) / 2;
    if (makeDocument(candidate).dump().size() <=
        ir::tachi::BokutachiCacheStore::kMaximumFileBytes) {
      chartIdBytes = candidate;
      lower = candidate + 1;
    } else {
      upper = candidate - 1;
    }
  }
  expect(chartIdBytes != 0,
         "encoded-size fixture fits the cache input limit");
  const nlohmann::json document = makeDocument(chartIdBytes);
  writeFile(path, document.dump());

  std::string diagnostic;
  ir::tachi::BokutachiCacheStore store;
  expect(store.activate(path, diagnostic) &&
             store.rememberChartId(kOrigin, "bms-7k", std::string(64, 'f'),
                                   std::string(
                                       ir::tachi::BokutachiCacheStore::
                                           kMaximumChartIdBytes,
                                       '"'),
                                   diagnostic),
         "near-limit cache accepts a new escaped chart identity");
  std::error_code error;
  expect(std::filesystem::file_size(path, error) <=
                 ir::tachi::BokutachiCacheStore::kMaximumFileBytes &&
             !error,
         "persisted cache remains within its own read limit");
  ir::tachi::BokutachiCacheStore reloaded;
  expect(reloaded.activate(path, diagnostic),
         "size-bounded persisted cache reloads successfully");
}

void testUserInvalidationDiscardsCacheAfterAtomicWriteFailure() {
  TempDirectory temp("clear-failure");
  const auto path = temp.path() / "bokutachi-cache.json";
  std::string diagnostic;
  ir::tachi::BokutachiCacheStore initial;
  expect(initial.activate(path, diagnostic) &&
             initial.rememberUserId(kOrigin, 404, diagnostic) &&
             initial.rememberChartId(kOrigin, "bms-7k", kSha, "chart-404",
                                     diagnostic),
         "clear-failure fixture persists an identity");

  atomic_file::Operations operations = atomic_file::defaultOperations();
  const auto realReplace = operations.replace;
  operations.replace = [&](const auto &from, const auto &to,
                           std::string &error) {
    if (from == path.string() + ".tmp" && to == path) {
      error = "injected Bokutachi cache replacement failure";
      return false;
    }
    return realReplace(from, to, error);
  };
  ir::tachi::BokutachiCacheStore failing(std::move(operations));
  expect(failing.activate(path, diagnostic),
         "clear-failure cache reloads before injection");
  expect(failing.clearUserIds(diagnostic),
         "failed atomic identity clear falls back to discarding the cache");
  expect(!std::filesystem::exists(path),
         "discard fallback removes the stale primary cache");

  ir::tachi::BokutachiCacheStore reloaded;
  expect(reloaded.activate(path, diagnostic) && !reloaded.userId(kOrigin),
         "discarded cache cannot restore an identity under a replacement key");
}

} // namespace

int main() {
  testMissingCachePersistsAndReloadsLookups();
  testMutationsAreValidatedAndIndependentlyClearable();
  testUnchangedValuesDoNotRewrite();
  testDisposableBadDataCanBeReplaced();
  testFutureCacheIsPreserved();
  testLoadedCollectionsAreBounded();
  testRuntimeChartCollectionEvictsOldestMapping();
  testPersistedCacheNeverExceedsItsReadLimit();
  testUserInvalidationDiscardsCacheAfterAtomicWriteFailure();
  if (failures != 0) {
    std::cerr << failures << " Bokutachi cache store test(s) failed\n";
    return 1;
  }
  std::cout << "Bokutachi cache store tests passed\n";
  return 0;
}
