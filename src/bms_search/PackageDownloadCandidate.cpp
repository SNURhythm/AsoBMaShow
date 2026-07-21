#include "PackageDownloadCandidate.h"

#include "../ArchiveFile.h"

#include <cctype>

namespace asobmshow::bms_search {
namespace {

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

DownloadCandidate configurePackageDownloadCandidate(
    DownloadCandidate candidate, const std::string &downloadUrl,
    const std::string &archiveName, const std::string &md5,
    PackageArchiveSupportCheck supportCheck) {
  candidate.originalUrl = downloadUrl;
  if (candidate.downloadUrl.empty()) {
    candidate.downloadUrl = downloadUrl;
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
