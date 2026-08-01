#include "DirectArchiveDriver.h"

namespace asobmshow::bms_search {

std::optional<DownloadCandidate> DirectArchiveDriver::classify(
    const std::string &url) {
  DownloadCandidate candidate;
  candidate.originalUrl = url;
  candidate.downloadUrl = url;
  candidate.archiveName = archiveNameFromUrl(url);
  const auto parsed = parseUrl(url);
  if (!parsed || (parsed->scheme != "http" && parsed->scheme != "https")) {
    candidate.reason = "Only HTTP and HTTPS links are supported.";
    return candidate;
  }

  const std::string ext = archiveExtensionFromUrl(url);
  if (isSupportedArchiveExtension(ext)) {
    candidate.supported = true;
    return candidate;
  }

  if (isRecognizedArchiveExtension(ext)) {
    candidate.knownUnsupportedArchive = true;
    candidate.reason =
        "This build cannot extract " + ext + " archives automatically.";
    return candidate;
  }

  candidate.reason =
      "This link is not a direct archive or supported cloud file.";
  return candidate;
}


} // namespace asobmshow::bms_search
