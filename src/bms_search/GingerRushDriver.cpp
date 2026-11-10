#include "GingerRushDriver.h"
#include "PackageDownloadCandidate.h"

namespace asobmshow::bms_search {
PackageSourceLookupResult GingerRushDriver::lookupByMd5(
    const std::string &md5, const std::atomic_bool &cancelled) {
  PackageSourceLookupResult result;
  result.sourceName = "GingerRush";
  if (cancelled.load() || !BmsSearchService::canDownload(md5)) return result;
  const auto hash = normalizedHash(md5);
  result.sourceUrl = "https://gingerrush.com/download/package/" + hash;
  std::string error;
  const auto body = fetchUrlText(result.sourceUrl, error, &cancelled);
  if (!body) {
    result.errorMessage = error.empty() ? "GingerRush did not find a package." : error;
    return result;
  }
  if (cancelled.load()) return result;
  try {
    const auto parsed = json::parse(*body);
    std::string url = jsonStringAt(parsed, "downloadURL");
    if (url.empty()) url = jsonStringAt(parsed, "downloadUrl");
    if (url.empty()) url = jsonStringAt(parsed, "download_url");
    if (url.empty()) {
      result.errorMessage = "GingerRush did not return a download URL.";
      return result;
    }
    url = resolveUrl(result.sourceUrl, url);
    auto candidate = configurePackageDownloadCandidate(
        {}, url, jsonStringAt(parsed, "fileName"), hash, isSupportedArchiveExtension);
    if (!candidate.supported) result.errorMessage = candidate.reason;
    result.candidate = std::move(candidate);
  } catch (const std::exception &) {
    result.errorMessage = "GingerRush returned invalid package JSON.";
  }
  return result;
}
} // namespace asobmshow::bms_search
