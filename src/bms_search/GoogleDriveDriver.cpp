#include "GoogleDriveDriver.h"

namespace asobmshow::bms_search {

bool GoogleDriveDriver::isHost(const ParsedUrl &url) {
  return url.host == "drive.google.com" || url.host == "docs.google.com" ||
         url.host == "drive.usercontent.google.com";
}

std::optional<std::string> GoogleDriveDriver::fileId(const std::string &url) {
  const auto parsed = parseUrl(url);
  if (!parsed || !isHost(*parsed)) {
    return std::nullopt;
  }
  if (parsed->path.starts_with("/drive/folders/")) {
    return std::nullopt;
  }

  static const std::regex filePathPattern(R"(/file/d/([^/?#]+))",
                                          std::regex::icase);
  std::smatch match;
  if (std::regex_search(parsed->path, match, filePathPattern) &&
      match.size() >= 2) {
    return match[1].str();
  }

  const std::string id = queryParam(*parsed, "id");
  if (!id.empty()) {
    return id;
  }
  return std::nullopt;
}

std::optional<std::string> GoogleDriveDriver::confirmationUrl(
    const std::string &fileId, const std::string &html,
    const std::string &baseUrl) {
  const std::string normalized = normalizeEmbeddedUrl(html);

  const std::regex formPattern(R"(<form\b[^>]*>)", std::regex::icase);
  for (auto formIt = std::sregex_iterator(normalized.begin(), normalized.end(),
                                          formPattern);
       formIt != std::sregex_iterator(); ++formIt) {
    const std::string formTag = formIt->str();
    const auto action = htmlAttributeValue(formTag, "action");
    if (!action || action->find("/download") == std::string::npos) {
      continue;
    }

    std::string id = fileId;
    std::string exportValue = "download";
    std::string confirmValue;
    std::string uuidValue;
    std::string resourceKeyValue;

    const size_t formStart = static_cast<size_t>(formIt->position());
    const size_t formEnd = normalized.find("</form>", formStart);
    const std::string formBody =
        formEnd == std::string::npos
            ? normalized.substr(formStart)
            : normalized.substr(formStart, formEnd - formStart);
    const std::regex inputPattern(R"(<input\b[^>]*>)", std::regex::icase);
    for (auto inputIt = std::sregex_iterator(formBody.begin(), formBody.end(),
                                             inputPattern);
         inputIt != std::sregex_iterator(); ++inputIt) {
      const std::string inputTag = inputIt->str();
      const auto name = htmlAttributeValue(inputTag, "name");
      const auto value = htmlAttributeValue(inputTag, "value");
      if (!name || !value) {
        continue;
      }
      const std::string lowerName = lowerCopy(*name);
      if (lowerName == "id") {
        id = *value;
      } else if (lowerName == "export") {
        exportValue = *value;
      } else if (lowerName == "confirm") {
        confirmValue = *value;
      } else if (lowerName == "uuid") {
        uuidValue = *value;
      } else if (lowerName == "resourcekey") {
        resourceKeyValue = *value;
      }
    }

    if (id.empty()) {
      continue;
    }

    std::string url = resolveUrl(baseUrl, *action);
    url += url.find('?') == std::string::npos ? "?" : "&";
    url += "id=" + urlEncode(id) + "&export=" + urlEncode(exportValue);
    if (!confirmValue.empty()) {
      url += "&confirm=" + urlEncode(confirmValue);
    }
    if (!uuidValue.empty()) {
      url += "&uuid=" + urlEncode(uuidValue);
    }
    if (!resourceKeyValue.empty()) {
      url += "&resourcekey=" + urlEncode(resourceKeyValue);
    }
    return url;
  }

  for (const auto &link : extractLinks(baseUrl, normalized)) {
    const auto parsed = parseUrl(link);
    if (!parsed || !isHost(*parsed)) {
      continue;
    }
    const bool looksLikeDownload =
        parsed->path == "/uc" || parsed->path.ends_with("/download") ||
        parsed->host == "drive.usercontent.google.com";
    if (!looksLikeDownload) {
      continue;
    }
    const std::string id = queryParam(*parsed, "id");
    if (!id.empty() && id != fileId) {
      continue;
    }
    if (!queryParam(*parsed, "confirm").empty() ||
        !queryParam(*parsed, "uuid").empty()) {
      return link;
    }
  }

  return std::nullopt;
}

std::optional<DownloadCandidate> GoogleDriveDriver::classify(
    const std::string &url) {
  const auto driveId = fileId(url);
  if (!driveId) {
    return std::nullopt;
  }

  DownloadCandidate candidate;
  candidate.originalUrl = url;
  candidate.archiveName = archiveNameFromUrl(url);
  candidate.downloadUrl =
      "https://drive.google.com/uc?export=download&id=" + *driveId;
  candidate.supported = true;
  return candidate;
}

std::optional<std::string> GoogleDriveDriver::fileIdFromUrls(
    const std::string &downloadUrl, const std::string &displayUrl) {
  if (const auto id = fileId(displayUrl)) {
    return id;
  }
  return fileId(downloadUrl);
}

bool GoogleDriveDriver::resolveWarningDownload(
    const std::string &downloadUrl, const std::string &displayUrl,
    const std::filesystem::path &archivePath, std::atomic_bool &cancelled,
    std::string &errorMessage,
    BmsSearchDownloadProgressCallback progressCallback) {
  const auto fileId = fileIdFromUrls(downloadUrl, displayUrl);
  if (!fileId) {
    return true;
  }

  const auto htmlBody = htmlBodyFromDownloadedFile(archivePath);
  if (!htmlBody) {
    return true;
  }

  const auto confirmedDownloadUrl =
      confirmationUrl(*fileId, *htmlBody, downloadUrl);
  if (!confirmedDownloadUrl) {
    errorMessage = "Google Drive returned an HTML page instead of an archive.";
    return false;
  }

  if (progressCallback) {
    progressCallback({.message = "Confirming Google Drive download"});
  }
  if (!downloadUrlToFile(*confirmedDownloadUrl, archivePath, cancelled,
                         errorMessage, progressCallback)) {
    return false;
  }

  if (htmlBodyFromDownloadedFile(archivePath)) {
    errorMessage = "Google Drive still returned an HTML page instead of an "
                   "archive after confirmation.";
    return false;
  }
  return true;
}


} // namespace asobmshow::bms_search
