#include "bms_search/BmsSearchDriver.h"
#include "bms_search/HorieYuukaDriver.h"
#include "bms_search/PackageSourceDrivers.h"

#include <map>

namespace asobmshow::bms_search {
std::vector<std::string> providerLookups;
std::vector<std::string> providerDownloads;
std::string availableProvider;
std::map<std::string, std::string> providerUrlOverrides;
bool bmsFileAvailable = false;
int horieLookups = 0;

PackageSourceLookupResult providerLookup(const std::string &name) {
  providerLookups.push_back(name);
  PackageSourceLookupResult result{
      .sourceName = name, .sourceUrl = "https://fixture.invalid/api/" + name};
  if (availableProvider == name || availableProvider == "all" ||
      providerUrlOverrides.contains(name)) {
    const auto override = providerUrlOverrides.find(name);
    const std::string url = override != providerUrlOverrides.end()
        ? override->second : "https://fixture.invalid/" + name + ".zip";
    result.candidate = DownloadCandidate{
        .originalUrl = url,
        .downloadUrl = url,
        .archiveName = name + ".zip", .supported = true};
  }
  return result;
}
PackageSourceLookupResult GingerRushDriver::lookupByMd5(
    const std::string &, const std::atomic_bool &) { return providerLookup("Ginger"); }
PackageSourceLookupResult KonmaiDriver::lookupByMd5(
    const std::string &, const std::atomic_bool &) { return providerLookup("Konmai"); }
bool probeDownloadUrl(const std::string &, std::string &error,
                      const std::atomic_bool *) {
  providerLookups.push_back("Wriggle");
  if (availableProvider == "Wriggle" || availableProvider == "all") return true;
  error = "HTTP 404";
  return false;
}
DownloadCandidate packageDownloadCandidate(const std::string &url,
                                           const std::string &name,
                                           const std::string &) {
  return {.originalUrl = url, .downloadUrl = url,
          .archiveName = name, .supported = true};
}
std::vector<std::string> HorieYuukaDriver::searchQueries(
    const std::string &, const std::string &, const std::string &,
    const std::string &) { return {"fixture"}; }
bool HorieYuukaDriver::tryDownload(
    const std::vector<std::string> &, const std::string &, const std::string &,
    bool, const std::string &, const std::filesystem::path &, std::atomic_bool &,
    BmsSearchDownloadProgressCallback, const BmsSearchDownloadOptions &,
    BmsSearchResult &) {
  ++horieLookups;
  return false;
}
std::optional<std::string> fetchUrlText(
    const std::string &, std::string &, const std::atomic_bool *, size_t) {
  return "fixture page";
}
std::vector<std::string> BmsSearchDriver::bmsLinks(
    const std::string &, const std::string &) {
  return bmsFileAvailable ? std::vector<std::string>{"https://fixture.invalid/bms"}
                         : std::vector<std::string>{};
}
std::vector<DownloadCandidate> BmsSearchDriver::downloadCandidates(
    const std::string &, const std::string &) {
  const auto override = providerUrlOverrides.find("BMS");
  const std::string url = override != providerUrlOverrides.end()
      ? override->second : "https://fixture.invalid/BMS.zip";
  return {{.originalUrl = url, .downloadUrl = url,
           .archiveName = "BMS.zip", .supported = true}};
}
} // namespace asobmshow::bms_search

#include "find_bms_provider_methods.inc"

void testProviderSelectionStopsAfterAFileIsFound() {
  using namespace asobmshow::bms_search;
  downloadFixtureFailure = [&](const std::string &url, std::string &error) {
    providerDownloads.push_back(url);
    error = "Connection lost";
    return true;
  };
  for (const std::string provider : {"Ginger", "Konmai", "Wriggle", "BMS", "none"}) {
    providerLookups.clear();
    providerDownloads.clear();
    horieLookups = 0;
    availableProvider = provider;
    bmsFileAvailable = provider == "BMS";
    std::atomic_bool cancelled{false};
    const auto result = BmsSearchService().findAndDownload(
        std::string(64, 'a'), std::string(32, 'b'), "fixture-library", cancelled);
    if (provider == "none") {
      assert(providerDownloads.empty() && horieLookups == 1);
    } else {
      assert(result.status == BmsSearchResult::Status::DownloadFailed);
      assert(providerDownloads == std::vector<std::string>{
          provider == "Wriggle"
              ? "https://bms.wrigglebug.xyz/download/package/" + std::string(32, 'b')
              : "https://fixture.invalid/" + provider + ".zip"});
      assert(horieLookups == 0);
      if (provider != "BMS") {
        assert(result.fallbackUrl == providerDownloads.front());
      }
      const size_t expectedLookups = provider == "Ginger" ? 1 :
                                     provider == "Konmai" ? 2 : 3;
      assert(providerLookups.size() == expectedLookups);
    }
  }
  downloadFixtureFailure = {};
}

void testChosenProviderRetriesOnlyOnUserDecision() {
  using namespace asobmshow::bms_search;
  availableProvider = "Ginger";
  providerLookups.clear();
  providerDownloads.clear();
  horieLookups = 0;
  downloadFixtureFailure = [&](const std::string &url, std::string &error) {
    providerDownloads.push_back(url);
    error = "Connection lost";
    return true;
  };
  int prompts = 0;
  BmsSearchDownloadOptions options;
  options.requestRetry = [&](const std::string &error, bool canResume) {
    assert(error == "Connection lost" && !canResume);
    assert(providerDownloads.size() == static_cast<size_t>(prompts + 1));
    return ++prompts == 1;
  };
  std::atomic_bool cancelled{false};
  const auto result = BmsSearchService().findAndDownload(
      std::string(64, 'a'), std::string(32, 'b'), "fixture-library", cancelled,
      {}, {}, {}, options);
  assert(prompts == 2 && cancelled.load());
  assert(result.status == BmsSearchResult::Status::DownloadFailed);
  assert(providerLookups == std::vector<std::string>({"Ginger"}));
  assert(providerDownloads == std::vector<std::string>({
      "https://fixture.invalid/Ginger.zip", "https://fixture.invalid/Ginger.zip"}));
  assert(horieLookups == 0);
  downloadFixtureFailure = {};
}

void testPixelDrainApiErrorDownloadsFromNextProvider() {
  using namespace asobmshow::bms_search;
  CleanupPaths cleanup;
  std::string error;
  const auto fixture = createFindBmsDownloadAttempt("fixture.zip", error);
  assert(fixture);
  cleanup.add(fixture->root);
  const auto libraryRoot = testDownloadRoot(*fixture).parent_path();
  cleanup.add(libraryRoot);
  const std::string chart = "#TITLE Waterfall\n#00111:01\n";
  writeExtractionZip(fixture->archivePath, {{"chart.bms", chart}});
  extractionDownloadFixture = fixture->archivePath;
  availableProvider = "Konmai";
  for (const std::string url : {
           "https://pixeldrain.net/api/file/abcd1234?download",
           "https://pixeldrain.com/api/filesystem/abcd1234/song.zip?attach"}) {
    providerUrlOverrides = {{"Ginger", url}};
    for (const long status : {403L, 404L, 429L, 500L, 503L}) {
      providerLookups.clear();
      providerDownloads.clear();
      horieLookups = 0;
      int prompts = 0;
      std::filesystem::path rejectedAttempt;
      downloadFixtureFailure = [&](const std::string &target, std::string &message) {
        providerDownloads.push_back(target);
        downloadFixtureHttpStatus = target == url ? status : 200;
        if (target != url) return false;
        rejectedAttempt = observedDownloadAttempt;
        message = "Provider rejected the request";
        return true;
      };
      std::atomic_bool cancelled{false};
      const auto library = libraryRoot / ("library-" + std::to_string(status));
      const auto result = BmsSearchService().findAndDownload(
          "", bms_parser::md5(chart), library, cancelled, {}, {}, {},
          {.requestRetry = [&](const std::string &message, bool) {
            std::cerr << "Unexpected retry during provider fallback: " << message << '\n';
            ++prompts;
            return false;
          }});
      assert(result.status == BmsSearchResult::Status::Downloaded);
      assert(!cancelled && prompts == 0 && horieLookups == 0);
      assert(providerLookups == std::vector<std::string>({"Ginger", "Konmai"}));
      assert(providerDownloads == std::vector<std::string>(
          {url, "https://fixture.invalid/Konmai.zip"}));
      assert(!std::filesystem::exists(rejectedAttempt));
      assert(result.fallbackUrl == "https://fixture.invalid/Konmai.zip");
      assert(result.downloadHttpStatus == 200);
    }
  }
  providerUrlOverrides.clear();
  downloadFixtureHttpStatus = 0;
  downloadFixtureFailure = {};
}

void testPixelDrainApiErrorsContinueThroughRemainingSources() {
  using namespace asobmshow::bms_search;
  availableProvider.clear();
  for (const bool bmsAvailable : {false, true}) {
    providerUrlOverrides = {
        {"Konmai", "https://pixeldrain.net/api/file/konmai?download"},
        {"Ginger", "https://pixeldrain.net/api/filesystem/ginger/song.zip?attach"},
        {"BMS", "https://pixeldrain.net/api/filesystem/bms/song.zip?attach"}};
    providerLookups.clear();
    providerDownloads.clear();
    bmsFileAvailable = bmsAvailable;
    horieLookups = 0;
    int prompts = 0;
    downloadFixtureFailure = [&](const std::string &url, std::string &message) {
      providerDownloads.push_back(url);
      downloadFixtureHttpStatus = 403;
      message = "CAPTCHA required";
      return true;
    };
    std::atomic_bool cancelled{false};
    const auto result = BmsSearchService().findAndDownload(
        std::string(64, 'a'), std::string(32, 'b'), "fixture-library", cancelled,
        {}, {}, {}, {.requestRetry = [&](const std::string &, bool) {
          ++prompts;
          return false;
        }});
    assert(!cancelled && prompts == 0);
    assert(providerLookups == std::vector<std::string>({"Ginger", "Konmai", "Wriggle"}));
    assert(providerDownloads.size() == (bmsAvailable ? 3 : 2) && horieLookups == 1);
    assert(result.status != BmsSearchResult::Status::Downloaded);
  }
  bmsFileAvailable = false;
  providerUrlOverrides.clear();
  downloadFixtureHttpStatus = 0;
  downloadFixtureFailure = {};
}

void testApiWaterfallDoesNotOverrideCancellationOrOtherFailures() {
  using namespace asobmshow::bms_search;
  struct Failure { const char *url; long status; bool cancel; };
  for (const auto failure : {
           Failure{"https://pixeldrain.net/api/file/abcd1234", 0, false},
           Failure{"https://example.com/song.zip", 403, false},
           Failure{"https://pixeldrain.net.example/api/file/abcd1234", 403, false},
           Failure{"https://pixeldrain.net/api/file/abcd1234", 403, true}}) {
    availableProvider = "Konmai";
    providerUrlOverrides = {{"Ginger", failure.url}};
    providerLookups.clear();
    horieLookups = 0;
    int prompts = 0;
    std::atomic_bool cancelled{false};
    downloadFixtureFailure = [&](const std::string &, std::string &message) {
      downloadFixtureHttpStatus = failure.status;
      cancelled.store(failure.cancel);
      message = "Download failed";
      return true;
    };
    const auto result = BmsSearchService().findAndDownload(
        "", std::string(32, 'b'), "fixture-library", cancelled, {}, {}, {},
        {.requestRetry = [&](const std::string &, bool) {
          ++prompts;
          return false;
        }});
    assert(result.status == BmsSearchResult::Status::DownloadFailed);
    assert(cancelled && prompts == (failure.cancel ? 0 : 1));
    assert(providerLookups == std::vector<std::string>{"Ginger"});
    assert(horieLookups == 0);
  }
  providerUrlOverrides.clear();
  downloadFixtureHttpStatus = 0;
  downloadFixtureFailure = {};
}
