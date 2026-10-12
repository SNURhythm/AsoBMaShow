#include "PackageDownloadCandidate.h"

#include "../ArchiveFile.h"

#include <cctype>

namespace asobmshow::bms_search {
namespace {

bool isPixelDrainHost(const std::string &host) {
  return hostMatches(host, "pixeldrain.net") ||
         hostMatches(host, "pixeldrain.com") ||
         hostMatches(host, "pixeldra.in");
}

std::string trimAsciiWhitespace(std::string value) {
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.front()))) {
    value.erase(value.begin());
  }
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back()))) {
    value.pop_back();
  }
  return value;
}

} // namespace

bool isPixelDrainApiError(const std::string &downloadUrl, long httpStatus) {
  if (httpStatus < 400 || httpStatus >= 600) return false;
  const auto parsed = parseUrl(downloadUrl);
  return parsed && (parsed->scheme == "http" || parsed->scheme == "https") &&
         isPixelDrainHost(parsed->host) &&
         (parsed->path.starts_with("/api/file/") ||
          parsed->path.starts_with("/api/filesystem/"));
}

DownloadCandidate configurePackageDownloadCandidate(
    DownloadCandidate candidate, const std::string &downloadUrl,
    const std::string &archiveName, const std::string &md5,
    PackageArchiveSupportCheck supportCheck) {
  candidate.originalUrl = downloadUrl;
  if (candidate.downloadUrl.empty()) {
    candidate.downloadUrl = downloadUrl;
  }
  const auto parsed = parseUrl(candidate.downloadUrl);
  if (parsed && (parsed->scheme == "http" || parsed->scheme == "https") &&
      isPixelDrainHost(parsed->host)) {
    // Package providers can return a file viewer URL rather than file data.
    // https://pixeldrain.com/api documents GET /file/{id}?download.
    static const std::regex filePath(R"(^/u/([A-Za-z0-9]+)/?$)");
    static const std::regex filesystemPath(
        R"(^/d/([A-Za-z0-9]+/(?:[^/]+/)*[^/]+)$)");
    std::smatch match;
    if (std::regex_match(parsed->path, match, filePath)) {
      candidate.downloadUrl = "https://" + parsed->host + "/api/file/" +
                              match[1].str() + "?download";
    } else if (std::regex_match(parsed->path, match, filesystemPath)) {
      // Filesystem shares use a separate API. Preserve the encoded path.
      candidate.downloadUrl = "https://" + parsed->host + "/api/filesystem/" +
                              match[1].str() + "?attach";
    }
  }

  const std::string suggestedArchiveName = trimAsciiWhitespace(archiveName);
  if (!suggestedArchiveName.empty() &&
      !archive_file::archiveExtensionFromName(suggestedArchiveName).empty()) {
    candidate.archiveName = suggestedArchiveName;
  } else if (candidate.archiveName.empty()) {
    candidate.archiveName = md5 + ".7z";
  }

  const std::string extension =
      archive_file::archiveExtensionFromName(candidate.archiveName);
  const bool extensionSupported =
      supportCheck != nullptr && supportCheck(extension);
  if (!candidate.supported && extensionSupported) {
    candidate.supported = true;
    candidate.knownUnsupportedArchive = false;
    candidate.reason.clear();
  } else if (!candidate.supported &&
             archive_file::isRecognizedArchiveExtension(extension)) {
    candidate.knownUnsupportedArchive = true;
    candidate.reason =
        "This build cannot extract " + extension + " archives automatically.";
  }
  return candidate;
}

} // namespace asobmshow::bms_search
