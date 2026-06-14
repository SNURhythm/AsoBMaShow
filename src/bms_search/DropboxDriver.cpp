#include "DropboxDriver.h"

namespace asobmshow::bms_search {

bool DropboxDriver::isHost(const ParsedUrl &url) {
  return hostMatches(url.host, "dropbox.com") ||
         hostMatches(url.host, "dropboxusercontent.com");
}

std::string DropboxDriver::forceDownloadUrl(const std::string &url) {
  return setQueryParameter(url, "dl", "1", {"raw"});
}

std::optional<DownloadCandidate> DropboxDriver::classify(
    const std::string &url) {
  const auto parsed = parseUrl(url);
  if (!parsed || !isHost(*parsed)) {
    return std::nullopt;
  }

  DownloadCandidate candidate;
  candidate.originalUrl = url;
  candidate.downloadUrl = forceDownloadUrl(url);
  candidate.archiveName = archiveNameFromUrl(url);
  const std::string ext = archiveExtensionFromName(candidate.archiveName);
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
      "Dropbox link did not point to a supported archive file.";
  return candidate;
}


} // namespace asobmshow::bms_search
