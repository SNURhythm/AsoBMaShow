#include "bms_search/BmsSearchDriver.h"
#include "bms_search/HorieYuukaDriver.h"
#include "bms_search/PackageSourceDrivers.h"

namespace asobmshow::bms_search {
std::vector<std::string> providerLookups;
std::vector<std::string> providerDownloads;
std::string availableProvider;
bool bmsFileAvailable = false;
int horieLookups = 0;

PackageSourceLookupResult providerLookup(const std::string &name) {
  providerLookups.push_back(name);
  PackageSourceLookupResult result{.sourceName = name};
  if (availableProvider == name) {
    result.candidate = DownloadCandidate{
        .originalUrl = "https://fixture.invalid/" + name + ".zip",
        .downloadUrl = "https://fixture.invalid/" + name + ".zip",
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
  if (availableProvider == "Wriggle") return true;
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
  return {{.originalUrl = "https://fixture.invalid/BMS.zip",
           .downloadUrl = "https://fixture.invalid/BMS.zip", .supported = true}};
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
  assert(providerLookups == std::vector<std::string>{"Ginger"});
  assert(providerDownloads == std::vector<std::string>({
      "https://fixture.invalid/Ginger.zip", "https://fixture.invalid/Ginger.zip"}));
  assert(horieLookups == 0);
  downloadFixtureFailure = {};
}
