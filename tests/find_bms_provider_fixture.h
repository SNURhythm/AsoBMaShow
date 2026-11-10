// Exercise the production GingerRush parser/service with controlled transport.
#include "bms_search/GingerRushDriver.h"

namespace asobmshow::bms_search {
std::vector<std::string> providerLookups;
std::optional<std::string> providerBody;
bool cancelDuringLookup = false;
std::optional<std::string> fetchUrlText(
    const std::string &url, std::string &error, const std::atomic_bool *cancelled, size_t) {
  providerLookups.push_back(url);
  if (cancelDuringLookup) const_cast<std::atomic_bool *>(cancelled)->store(true);
  if (!providerBody) error = "HTTP 403";
  return providerBody;
}
}

void testGingerRushOnlyLookupAndDownload() {
  using namespace asobmshow::bms_search;
  CleanupPaths cleanup;
  std::string error;
  const auto fixture = createFindBmsDownloadAttempt("fixture.zip", error);
  assert(fixture); cleanup.add(fixture->root);
  const auto library = testDownloadRoot(*fixture).parent_path();
  cleanup.add(library);
  const std::string chart = "#TITLE GingerRush\n#00111:01\n";
  writeExtractionZip(fixture->archivePath, {{"chart.bms", chart}});
  extractionDownloadFixture = fixture->archivePath;
  const auto md5 = bms_parser::md5(chart);
  const std::string endpoint = "https://gingerrush.com/download/package/" + md5;
  std::vector<std::string> downloads;
  downloadFixtureFailure = [&](const std::string &url, std::string &) {
    downloads.push_back(url); return false;
  };
  providerBody = R"({"downloadURL":"https://fixture.invalid/package.zip","fileName":"package.zip"})";
  for (const std::string hash : {std::string{}, std::string("invalid"), std::string(32, 'g')}) {
    providerLookups.clear(); std::atomic_bool cancelled{false};
    auto result = BmsSearchService().findAndDownload(std::string(64, 'a'), hash, library, cancelled);
    assert(result.status == BmsSearchResult::Status::NotFound);
    assert(providerLookups.empty() && downloads.empty());
  }
  {
    std::atomic_bool cancelled{true};
    auto result = BmsSearchService().findAndDownload("", md5, library, cancelled);
    assert(result.status == BmsSearchResult::Status::DownloadFailed);
    assert(providerLookups.empty() && downloads.empty());
  }
  for (const auto &body : std::vector<std::optional<std::string>>{
           std::nullopt, "{broken", "{}", "[]",
           R"({"downloadURL":"file:///tmp/forbidden.zip"})",
           R"({"downloadURL":"http://fixture.invalid/insecure.zip"})",
           R"({"downloadURL":"https://user:secret@fixture.invalid/package.zip"})"}) {
    providerLookups.clear(); providerBody = body; std::atomic_bool cancelled{false};
    auto result = BmsSearchService().findAndDownload("", md5, library, cancelled);
    assert(result.status != BmsSearchResult::Status::Downloaded);
    assert(providerLookups == std::vector<std::string>{endpoint});
    assert(downloads.empty());
  }
  providerBody = R"({"downloadURL":"https://fixture.invalid/package.zip","fileName":"package.zip"})";
  {
    providerLookups.clear(); cancelDuringLookup = true; std::atomic_bool cancelled{false};
    auto result = BmsSearchService().findAndDownload("", md5, library, cancelled);
    assert(cancelled && result.status == BmsSearchResult::Status::DownloadFailed);
    assert(providerLookups == std::vector<std::string>{endpoint} && downloads.empty());
    cancelDuringLookup = false;
  }
  {
    providerLookups.clear(); std::atomic_bool cancelled{false};
    auto upper = md5;
    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return std::toupper(c); });
    const auto result = BmsSearchService().findAndDownload("", " " + upper + " ", library, cancelled);
    assert(result.status == BmsSearchResult::Status::Downloaded);
    assert(providerLookups == std::vector<std::string>{endpoint});
    assert(downloads == std::vector<std::string>{"https://fixture.invalid/package.zip"});
    assert(std::filesystem::exists(result.outputPath));
  }
  providerBody = R"({"downloadURL":"https://pixeldra.in/u/abcd1234","fileName":"package.zip"})";
  for (long status : {403L, 404L, 429L, 500L}) {
    providerLookups.clear(); downloads.clear(); downloadFixtureHttpStatus = status;
    downloadFixtureFailure = [&](const std::string &url, std::string &message) {
      downloads.push_back(url); message = "HTTP rejected"; return true;
    };
    std::atomic_bool cancelled{false};
    auto result = BmsSearchService().findAndDownload("", md5, library, cancelled);
    assert(result.status == BmsSearchResult::Status::DownloadFailed);
    assert(providerLookups == std::vector<std::string>{endpoint});
    assert(downloads == std::vector<std::string>{"https://pixeldra.in/api/file/abcd1234?download"});
  }
  downloadFixtureFailure = {}; downloadFixtureHttpStatus = 0;
}

void testGingerRushQueuedTransfer() {
  using namespace asobmshow::bms_search;
  providerLookups.clear();
  providerBody = R"({"downloadUrl":"/files/package.zip","fileName":"package.zip"})";
  std::optional<downloads::Request> queued;
  BmsSearchDownloadOptions options;
  options.enqueueDownload = [&](const downloads::Request &request) { queued = request; return "queued"; };
  downloadFixtureFailure = [](const std::string &, std::string &) { assert(false); return true; };
  std::atomic_bool cancelled{false};
  const auto result = BmsSearchService().findAndDownload("", std::string(32, 'a'), {}, cancelled, {}, {}, {}, options);
  assert(result.queuedDownloadId == "queued" && queued);
  assert(queued->url == "https://gingerrush.com/files/package.zip");
  assert(queued->fallbackUrls.empty() && queued->browserTransferId.empty());
  assert(providerLookups == std::vector<std::string>{"https://gingerrush.com/download/package/" + std::string(32, 'a')});
  downloadFixtureFailure = {};
}
