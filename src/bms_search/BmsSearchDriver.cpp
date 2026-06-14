#include "BmsSearchDriver.h"
#include "DirectArchiveDriver.h"
#include "DropboxDriver.h"
#include "GoogleDriveDriver.h"
#include "IpfsDriver.h"

namespace asobmshow::bms_search {

bool BmsSearchDriver::isHost(const ParsedUrl &url) {
  return hostMatches(url.host, "bmssearch.net");
}

DownloadCandidate classifyLink(const std::string &url) {
  DownloadCandidate candidate;
  candidate.originalUrl = url;
  candidate.downloadUrl = url;

  if (auto ipfs = IpfsDriver::classify(url)) {
    return *ipfs;
  }

  const auto parsed = parseUrl(url);
  if (!parsed || (parsed->scheme != "http" && parsed->scheme != "https")) {
    candidate.reason = "Only HTTP and HTTPS links are supported.";
    return candidate;
  }
  if (BmsSearchDriver::isHost(*parsed)) {
    candidate.reason = "Internal BMS Search link.";
    return candidate;
  }
  if (auto dropbox = DropboxDriver::classify(url)) {
    return *dropbox;
  }
  if (auto googleDrive = GoogleDriveDriver::classify(url)) {
    return *googleDrive;
  }
  if (auto direct = DirectArchiveDriver::classify(url)) {
    return *direct;
  }
  candidate.reason =
      "This link is not a direct archive or supported cloud file.";
  return candidate;
}

void appendExtractedLink(std::vector<ExtractedLink> &links,
                         std::set<std::string> &seen,
                         const std::string &baseUrl, const std::string &link,
                         const std::string &label = "") {
  const std::string trimmed = trimCopy(link);
  if (trimmed.empty() || trimmed.starts_with("#") ||
      trimmed.starts_with("javascript:")) {
    return;
  }
  const std::string resolved = resolveUrl(baseUrl, trimmed);
  if (!seen.insert(resolved).second) {
    if (!label.empty()) {
      for (auto &existing : links) {
        if (existing.url == resolved && existing.label.empty()) {
          existing.label = label;
          break;
        }
      }
    }
    return;
  }
  links.push_back({resolved, label});
}

std::vector<ExtractedLink> extractLinkRefs(const std::string &baseUrl,
                                           const std::string &html) {
  const std::string normalized = normalizeEmbeddedUrl(html);
  std::set<std::string> seen;
  std::vector<ExtractedLink> links;

  const std::regex anchorPattern(R"(<a\b([^>]*)>([\s\S]*?)</a>)",
                                 std::regex::icase);
  for (auto it = std::sregex_iterator(normalized.begin(), normalized.end(),
                                      anchorPattern);
       it != std::sregex_iterator(); ++it) {
    const std::string attributes = (*it)[1].str();
    const std::string label = plainTextFromHtmlFragment((*it)[2].str());
    if (const auto href = htmlAttributeValue(attributes, "href")) {
      appendExtractedLink(links, seen, baseUrl, *href, label);
    }
    if (const auto dataUrl = htmlAttributeValue(attributes, "data-url")) {
      appendExtractedLink(links, seen, baseUrl, *dataUrl, label);
    }
  }

  const std::regex hrefPattern(R"((?:href|data-url)\s*=\s*["']([^"']+)["'])",
                               std::regex::icase);
  for (auto it = std::sregex_iterator(normalized.begin(), normalized.end(),
                                      hrefPattern);
       it != std::sregex_iterator(); ++it) {
    appendExtractedLink(links, seen, baseUrl, (*it)[1].str());
  }

  const std::regex quotedUrlPattern(
      R"(["']((?:https?|ipfs)://[^"'\s<>]+)["'])", std::regex::icase);
  for (auto it = std::sregex_iterator(normalized.begin(), normalized.end(),
                                      quotedUrlPattern);
       it != std::sregex_iterator(); ++it) {
    appendExtractedLink(links, seen, baseUrl, htmlDecode((*it)[1].str()));
  }

  return links;
}

std::vector<std::string> extractLinks(const std::string &baseUrl,
                                      const std::string &html) {
  std::vector<std::string> result;
  for (const auto &link : extractLinkRefs(baseUrl, html)) {
    result.push_back(link.url);
  }
  return result;
}

std::vector<std::string> BmsSearchDriver::bmsLinks(
    const std::string &patternUrl, const std::string &html) {
  std::vector<std::string> result;
  for (const auto &link : extractLinks(patternUrl, html)) {
    const auto parsed = parseUrl(link);
    if (parsed && isHost(*parsed) && parsed->path.starts_with("/bmses/")) {
      result.push_back(link);
    }
  }
  return result;
}

std::vector<DownloadCandidate> BmsSearchDriver::downloadCandidates(
    const std::string &bmsUrl, const std::string &html) {
  std::vector<DownloadCandidate> result;
  for (const auto &link : extractLinkRefs(bmsUrl, html)) {
    auto candidate = classifyLink(link.url);
    if (candidate.archiveName.empty()) {
      candidate.archiveName = archiveNameFromText(link.label);
    }
    if (!candidate.supported && !candidate.archiveName.empty()) {
      const auto parsed = parseUrl(candidate.originalUrl);
      const std::string ext = archiveExtensionFromName(candidate.archiveName);
      if (parsed && DropboxDriver::isHost(*parsed) &&
          isSupportedArchiveExtension(ext)) {
        candidate.supported = true;
        candidate.knownUnsupportedArchive = false;
        candidate.reason.clear();
      }
    }
    if (!candidate.reason.empty() || candidate.supported) {
      result.push_back(std::move(candidate));
    }
  }

  std::stable_sort(result.begin(), result.end(),
                   [](const DownloadCandidate &a,
                      const DownloadCandidate &b) {
                     if (a.supported != b.supported) {
                       return a.supported;
                     }
                     if (a.knownUnsupportedArchive != b.knownUnsupportedArchive) {
                       return a.knownUnsupportedArchive;
                     }
                     return a.originalUrl < b.originalUrl;
                   });
  return result;
}


} // namespace asobmshow::bms_search
