#include "BmsSearchService.h"
#include "bms_search/GingerRushDriver.h"

using namespace asobmshow::bms_search;

BmsSearchResult BmsSearchService::findAndDownload(
    const std::string &sha256, const std::string &md5,
    const std::filesystem::path &libraryRoot, std::atomic_bool &cancelled,
    BmsSearchDownloadProgressCallback progressCallback,
    const std::string &, const std::string &,
    BmsSearchDownloadOptions options) const {
  BmsSearchResult result;
  const auto cancel = [&] {
    result.status = BmsSearchResult::Status::DownloadFailed;
    result.message = "Lookup cancelled.";
    result.presentationMessage = i18n::message("library.find_bms.result.cancelled");
  };
  if (cancelled.load()) { cancel(); return result; }
  if (!canDownload(md5)) {
    result.message = "A valid chart MD5 is required for GingerRush downloads.";
    result.presentationMessage = i18n::message("library.find_bms.result.md5_required");
    return result;
  }
  if (progressCallback) progressCallback({.message = "Searching GingerRush package source"});
  const auto lookup = GingerRushDriver::lookupByMd5(md5, cancelled);
  if (cancelled.load()) { cancel(); return result; }
  if (!lookup.candidate || !lookup.candidate->supported) {
    result.status = lookup.candidate ? BmsSearchResult::Status::UnsupportedLink
                                    : BmsSearchResult::Status::NotFound;
    result.message = lookup.errorMessage.empty() ? "GingerRush did not find a package."
                                               : lookup.errorMessage;
    return result;
  }
  const auto hash = normalizedHash(sha256);
  const auto archiveKey = isHexStringOfLength(hash, 64) ? hash : normalizedHash(md5);
  if (progressCallback) progressCallback({.message = "Preparing GingerRush package download"});
  // There is deliberately no alternate provider or webpage lookup on any outcome.
  downloadAndExtractArchive(lookup.candidate->downloadUrl, lookup.candidate->originalUrl,
      archiveKey, libraryRoot, cancelled, progressCallback, options, result,
      lookup.candidate->archiveName, archiveKey);
  return result;
}
