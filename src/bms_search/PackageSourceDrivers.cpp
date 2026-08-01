#include "PackageSourceDrivers.h"
#include "PackageDownloadCandidate.h"

namespace asobmshow::bms_search {

DownloadCandidate packageDownloadCandidate(const std::string &downloadUrl,
                                           const std::string &archiveName,
                                           const std::string &md5) {
  return configurePackageDownloadCandidate(
      classifyLink(downloadUrl), downloadUrl, archiveName, md5,
      isSupportedArchiveExtension);
}

PackageSourceLookupResult
GingerRushDriver::lookupByMd5(const std::string &md5) {
  PackageSourceLookupResult result;
  result.sourceName = "Ginger";
  result.sourceUrl = "https://gingerrush.com/download/package/" + md5;

  std::string errorMessage;
  const auto body = fetchUrlText(result.sourceUrl, errorMessage);
  if (!body) {
    result.errorMessage =
        errorMessage.empty() ? "Ginger did not find a package." : errorMessage;
    return result;
  }

  try {
    const auto parsed = json::parse(*body);
    std::string downloadUrl = jsonStringAt(parsed, "downloadURL");
    if (downloadUrl.empty()) {
      downloadUrl = jsonStringAt(parsed, "downloadUrl");
    }
    if (downloadUrl.empty()) {
      downloadUrl = jsonStringAt(parsed, "download_url");
    }
    if (downloadUrl.empty()) {
      result.errorMessage = "Ginger did not return a download URL.";
      return result;
    }
    downloadUrl = resolveUrl(result.sourceUrl, downloadUrl);
    result.candidate = packageDownloadCandidate(
        downloadUrl, jsonStringAt(parsed, "fileName"), md5);
  } catch (const std::exception &e) {
    result.errorMessage =
        std::string("Ginger returned invalid package JSON: ") + e.what();
  }
  return result;
}

PackageSourceLookupResult KonmaiDriver::lookupByMd5(const std::string &md5) {
  PackageSourceLookupResult result;
  result.sourceName = "Konmai";
  result.sourceUrl = "https://bms.alvorna.com/api/hash?md5=" + md5;

  std::string errorMessage;
  const auto body = fetchUrlText(result.sourceUrl, errorMessage);
  if (!body) {
    result.errorMessage =
        errorMessage.empty() ? "Konmai did not find a package." : errorMessage;
    return result;
  }

  try {
    const auto parsed = json::parse(*body);
    const std::string status = lowerCopy(jsonStringAt(parsed, "result"));
    if (!status.empty() && status != "success") {
      result.errorMessage = jsonStringAt(parsed, "msg");
      if (result.errorMessage.empty()) {
        result.errorMessage = "Konmai did not find a package.";
      }
      return result;
    }

    const json *data = nullptr;
    const auto dataIt = parsed.find("data");
    if (dataIt != parsed.end()) {
      if (dataIt->is_object()) {
        data = &*dataIt;
      } else if (dataIt->is_array() && !dataIt->empty() &&
                 dataIt->front().is_object()) {
        data = &dataIt->front();
      }
    }
    if (data == nullptr) {
      result.errorMessage = "Konmai did not return package metadata.";
      return result;
    }

    std::string downloadUrl = jsonStringAt(*data, "song_url");
    if (downloadUrl.empty()) {
      downloadUrl = jsonStringAt(*data, "songUrl");
    }
    if (downloadUrl.empty()) {
      result.errorMessage = "Konmai did not return a song URL.";
      return result;
    }
    downloadUrl = resolveUrl(result.sourceUrl, downloadUrl);

    std::string archiveName = jsonStringAt(*data, "song_name");
    if (!archiveName.empty() && archiveExtensionFromName(archiveName).empty()) {
      archiveName += ".7z";
    }
    result.candidate = packageDownloadCandidate(downloadUrl, archiveName, md5);
  } catch (const std::exception &e) {
    result.errorMessage =
        std::string("Konmai returned invalid package JSON: ") + e.what();
  }
  return result;
}

PackageSourceLookupResult WriggleDriver::lookupByMd5(const std::string &md5) {
  PackageSourceLookupResult result;
  result.sourceName = "Wriggle";
  result.sourceUrl = "https://bms.wrigglebug.xyz/download/package/" + md5;
  result.candidate =
      packageDownloadCandidate(result.sourceUrl, md5 + ".7z", md5);
  return result;
}

bool EndlessDreamSourcesDriver::tryDownloadByMd5(
    const std::string &md5, const std::string &archiveKey,
    const std::filesystem::path &libraryRoot, std::atomic_bool &cancelled,
    BmsSearchDownloadProgressCallback progressCallback,
    const BmsSearchDownloadOptions &options,
    BmsSearchResult &result) {
  const std::string md5Hash = normalizedHash(md5);
  if (!isHexStringOfLength(md5Hash, 32)) {
    return false;
  }

  std::optional<BmsSearchResult> lastDownloadFailure;
  std::string lastLookupError;
  struct PackageSource {
    const char *name;
    PackageSourceLookupResult (*lookupByMd5)(const std::string &);
  };
  const std::array<PackageSource, 3> sources = {
      PackageSource{"Ginger", GingerRushDriver::lookupByMd5},
      PackageSource{"Konmai", KonmaiDriver::lookupByMd5},
      PackageSource{"Wriggle", WriggleDriver::lookupByMd5},
  };

  for (const auto &source : sources) {
    if (cancelled.load()) {
      result.status = BmsSearchResult::Status::DownloadFailed;
      result.message = "Lookup cancelled.";
      return true;
    }

    if (progressCallback) {
      progressCallback({.message = "Searching " + std::string(source.name) +
                                   " package source"});
    }
    const auto lookup = source.lookupByMd5(md5Hash);
    if (!lookup.candidate || !lookup.candidate->supported) {
      if (!lookup.errorMessage.empty()) {
        lastLookupError = lookup.sourceName + ": " + lookup.errorMessage;
        SDL_Log("BMS package source lookup failed: %s",
                lastLookupError.c_str());
      }
      continue;
    }

    if (progressCallback) {
      progressCallback({.message = "Preparing " + lookup.sourceName +
                                   " package download"});
    }

    BmsSearchResult attempt = result;
    if (!lookup.sourceUrl.empty()) {
      attempt.fallbackUrl = lookup.sourceUrl;
    }
    bool downloadedArchive = false;
    const std::string effectiveArchiveKey =
        archiveKey.empty() ? md5Hash : archiveKey;
    const bool finished = downloadAndExtractArchive(
        lookup.candidate->downloadUrl, lookup.candidate->originalUrl,
        effectiveArchiveKey, libraryRoot, cancelled, progressCallback, options,
        attempt, lookup.candidate->archiveName, effectiveArchiveKey,
        &downloadedArchive);
    if (finished || downloadedArchive || cancelled.load()) {
      result = std::move(attempt);
      return true;
    }

    if (!attempt.message.empty()) {
      attempt.message = lookup.sourceName + ": " + attempt.message;
    }
    lastDownloadFailure = std::move(attempt);
  }

  if (lastDownloadFailure) {
    result = std::move(*lastDownloadFailure);
  } else if (!lastLookupError.empty() && result.message.empty()) {
    result.status = BmsSearchResult::Status::NotFound;
    result.message = lastLookupError;
  }
  return false;
}


} // namespace asobmshow::bms_search
