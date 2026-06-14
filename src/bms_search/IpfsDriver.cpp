#include "IpfsDriver.h"

namespace asobmshow::bms_search {

std::optional<std::string> IpfsDriver::pathFromUrl(const std::string &url) {
  const auto parsed = parseUrl(url);
  if (!parsed) {
    return std::nullopt;
  }
  if (parsed->scheme == "ipfs" && !parsed->host.empty()) {
    std::string path = parsed->host;
    if (!parsed->path.empty() && parsed->path != "/") {
      path += parsed->path;
    }
    return path;
  }
  if ((parsed->scheme == "http" || parsed->scheme == "https") &&
      parsed->path.starts_with("/ipfs/")) {
    return urlDecode(parsed->path.substr(6));
  }
  return std::nullopt;
}

std::optional<DownloadCandidate> IpfsDriver::classify(const std::string &url) {
  const auto ipfsPath = pathFromUrl(url);
  if (!ipfsPath || trimCopy(*ipfsPath).empty()) {
    return std::nullopt;
  }

  DownloadCandidate candidate;
  candidate.originalUrl = url;
  candidate.downloadUrl = "https://gateway.ipfs.io/api/v0/get?arg=" +
                          urlEncode(*ipfsPath) +
                          "&archive=true&compress=true";
  const std::string shortIpfsPath =
      ipfsPath->substr(0, std::min<size_t>(32, ipfsPath->size()));
  candidate.archiveName = safeStorageKey("ipfs-" + shortIpfsPath) + ".tar.gz";
  candidate.supported = isSupportedArchiveExtension(".tar.gz");
  if (!candidate.supported) {
    candidate.knownUnsupportedArchive = true;
    candidate.reason = "This build cannot extract IPFS tar archives.";
  }
  return candidate;
}


} // namespace asobmshow::bms_search
