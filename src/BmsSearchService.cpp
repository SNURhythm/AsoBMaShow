#include "BmsSearchService.h"

#include "bms_search/BmsSearchDriver.h"
#include "bms_search/HorieYuukaDriver.h"
#include "bms_search/Internal.h"
#include "bms_search/PackageSourceDrivers.h"

#include <SDL2/SDL.h>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace asobmshow::bms_search;

std::string BmsSearchService::patternUrlForSha256(const std::string &sha256) {
  return std::string(kBaseUrl) + "/patterns/" + normalizedHash(sha256);
}

std::string BmsSearchService::searchUrlForText(const std::string &query) {
  return bmsSearchUrlForText(query);
}

std::string
BmsSearchService::googleSearchUrlForSha256(const std::string &sha256) {
  return googleSearchUrlForText(normalizedHash(sha256));
}

BmsSearchResult BmsSearchService::findAndDownload(
    const std::string &sha256, const std::string &md5,
    const std::filesystem::path &libraryRoot, std::atomic_bool &cancelled,
    BmsSearchDownloadProgressCallback progressCallback,
    const std::string &title, const std::string &artist,
    BmsSearchDownloadOptions options) const {
  BmsSearchResult result;
  const std::string hash = normalizedHash(sha256);
  const std::string md5Hash = normalizedHash(md5);
  const std::string titleOnly = trimCopy(title);
  const std::string artistOnly = trimCopy(artist);
  const std::string titleQuery = trimCopy(
      titleOnly + (artistOnly.empty() ? std::string() : " " + artistOnly));
  const std::string archiveKey = hash.empty() ? md5Hash : hash;
  const HorieLookupTerms horieTerms =
      horieLookupTermsForMeta(titleOnly, artistOnly);
  const auto horieQueries = HorieYuukaDriver::searchQueries(
      horieTerms.title, horieTerms.artist, hash, md5Hash);
  result.patternUrl = hash.empty() ? std::string() : patternUrlForSha256(hash);
  const std::string titleSearchQuery =
      titleQuery.empty() ? (!md5Hash.empty() ? md5Hash : hash) : titleQuery;
  const std::string bmsTitleSearchUrl = bmsSearchUrlForText(titleSearchQuery);
  result.fallbackUrl = result.patternUrl.empty() ? bmsTitleSearchUrl
                                                 : result.patternUrl;

  if (hash.empty() && md5Hash.empty() && titleOnly.empty()) {
    result.status = BmsSearchResult::Status::NotFound;
    result.message =
        "Selected entry does not have a hash or title to search with.";
    result.fallbackUrl = bmsSearchUrlForText("");
    return result;
  }

  auto preserveLookupContext = [](BmsSearchResult &target,
                                  const BmsSearchResult &source) {
    if (!source.fallbackUrl.empty()) {
      target.fallbackUrl = source.fallbackUrl;
    }
    if (target.patternUrl.empty()) {
      target.patternUrl = source.patternUrl;
    }
    if (target.bmsUrl.empty()) {
      target.bmsUrl = source.bmsUrl;
    }
  };

  std::optional<BmsSearchResult> packageFailure;
  auto tryPackageSources = [&]() -> std::optional<BmsSearchResult> {
    if (md5Hash.empty()) {
      return std::nullopt;
    }

    BmsSearchResult packageResult;
    preserveLookupContext(packageResult, result);
    if (EndlessDreamSourcesDriver::tryDownloadByMd5(
            md5Hash, archiveKey, libraryRoot, cancelled, progressCallback,
            options, packageResult)) {
      return packageResult;
    }
    if (!packageResult.message.empty()) {
      packageFailure = packageResult;
    }
    return std::nullopt;
  };

  auto tryHorieAfterAutomaticFailure =
      [&](const BmsSearchResult &automaticFailure)
      -> std::optional<BmsSearchResult> {
    BmsSearchResult horieResult;
    preserveLookupContext(horieResult, automaticFailure);
    if (HorieYuukaDriver::tryDownload(
            horieQueries, horieTerms.title, horieTerms.artist,
            !horieTerms.title.empty(), archiveKey, libraryRoot, cancelled,
            progressCallback, options, horieResult)) {
      return horieResult;
    }
    preserveLookupContext(horieResult, automaticFailure);
    if (horieResult.message.empty()) {
      horieResult.status = BmsSearchResult::Status::NotFound;
      horieResult.message = "Horie did not find a matching BMS archive.";
    } else if (packageFailure &&
               horieResult.status == BmsSearchResult::Status::NotFound) {
      horieResult.message =
          "Automatic sources did not produce a usable archive. " +
          horieResult.message;
    }
    return horieResult;
  };

  if (const auto packageResult = tryPackageSources()) {
    return *packageResult;
  }

  if (hash.empty()) {
    result.status = BmsSearchResult::Status::NotFound;
    result.message =
        "Selected entry does not have SHA256 for BMS Search lookup.";
    result.fallbackUrl = bmsTitleSearchUrl;
    if (const auto fallbackResult = tryHorieAfterAutomaticFailure(result)) {
      return *fallbackResult;
    }
    result.message =
        "No package source or Horie archive found a matching song.";
    return result;
  }

  if (progressCallback) {
    progressCallback({.message = "Opening BMS Search pattern page"});
  }

  std::string errorMessage;
  const auto patternHtml = fetchUrlText(result.patternUrl, errorMessage);
  if (cancelled.load()) {
    result.status = BmsSearchResult::Status::DownloadFailed;
    result.message = "Lookup cancelled.";
    return result;
  }
  if (!patternHtml) {
    result.status = BmsSearchResult::Status::NotFound;
    result.message =
        errorMessage.empty() ? "BMS Search did not return a pattern page."
                             : errorMessage;
    result.fallbackUrl = bmsTitleSearchUrl;
    if (const auto fallbackResult = tryHorieAfterAutomaticFailure(result)) {
      return *fallbackResult;
    }
    return result;
  }

  const auto bmsLinks =
      BmsSearchDriver::bmsLinks(result.patternUrl, *patternHtml);
  if (bmsLinks.empty()) {
    result.status = BmsSearchResult::Status::NoDownloadLink;
    result.message = "BMS Search found the pattern, but no BMS page link.";
    result.fallbackUrl = result.patternUrl;
    if (const auto fallbackResult = tryHorieAfterAutomaticFailure(result)) {
      return *fallbackResult;
    }
    return result;
  }

  std::vector<DownloadCandidate> candidates;
  for (const auto &bmsUrl : bmsLinks) {
    if (cancelled.load()) {
      result.status = BmsSearchResult::Status::DownloadFailed;
      result.message = "Lookup cancelled.";
      return result;
    }
    if (progressCallback) {
      progressCallback({.message = "Opening BMS Search details page"});
    }
    std::string bmsError;
    const auto bmsHtml = fetchUrlText(bmsUrl, bmsError);
    if (!bmsHtml) {
      SDL_Log("Failed to fetch BMS Search details page %s: %s",
              bmsUrl.c_str(), bmsError.c_str());
      continue;
    }
    if (result.bmsUrl.empty()) {
      result.bmsUrl = bmsUrl;
      result.fallbackUrl = bmsUrl;
    }
    auto pageCandidates =
        BmsSearchDriver::downloadCandidates(bmsUrl, *bmsHtml);
    candidates.insert(candidates.end(),
                      std::make_move_iterator(pageCandidates.begin()),
                      std::make_move_iterator(pageCandidates.end()));
  }

  const auto supportedIt = std::find_if(
      candidates.begin(), candidates.end(),
      [](const DownloadCandidate &candidate) { return candidate.supported; });
  if (supportedIt == candidates.end()) {
    const auto externalIt = std::find_if(
        candidates.begin(), candidates.end(), [](const DownloadCandidate &c) {
          return !c.originalUrl.empty() &&
                 c.reason != "Internal BMS Search link.";
        });
    result.status = externalIt == candidates.end()
                        ? BmsSearchResult::Status::NoDownloadLink
                        : BmsSearchResult::Status::UnsupportedLink;
    result.message = externalIt == candidates.end()
                         ? "BMS Search did not expose a download link."
                         : externalIt->reason;
    if (result.fallbackUrl.empty()) {
      result.fallbackUrl = result.bmsUrl.empty() ? result.patternUrl
                                                 : result.bmsUrl;
    }
    if (const auto fallbackResult = tryHorieAfterAutomaticFailure(result)) {
      return *fallbackResult;
    }
    return result;
  }

  const std::string effectiveDownloadUrl = supportedIt->downloadUrl;
  if (!downloadAndExtractArchive(effectiveDownloadUrl, supportedIt->originalUrl,
                                 hash, libraryRoot, cancelled,
                                 progressCallback, options, result,
                                 supportedIt->archiveName)) {
    if (result.fallbackUrl.empty()) {
      result.fallbackUrl = result.bmsUrl.empty() ? result.patternUrl
                                                 : result.bmsUrl;
    }
    if (const auto fallbackResult = tryHorieAfterAutomaticFailure(result)) {
      return *fallbackResult;
    }
  }
  return result;
}

BmsSearchResult BmsSearchService::downloadCandidate(
    const BmsSearchCandidate &candidate, const std::string &sha256,
    const std::string &md5, const std::filesystem::path &libraryRoot,
    std::atomic_bool &cancelled,
    BmsSearchDownloadProgressCallback progressCallback,
    BmsSearchDownloadOptions options) const {
  BmsSearchResult result;
  const std::string hash = normalizedHash(sha256);
  const std::string md5Hash = normalizedHash(md5);
  const std::string archiveKey = hash.empty() ? md5Hash : hash;
  if (progressCallback) {
    progressCallback({.message = "Preparing Horie archive download"});
  }
  HorieYuukaDriver::downloadCandidateById(candidate, archiveKey, libraryRoot,
                                          cancelled, progressCallback, options,
                                          result);
  return result;
}
