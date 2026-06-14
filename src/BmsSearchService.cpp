#include "BmsSearchService.h"

#include "Utils.h"
#include "bms_parser.hpp"
#include "path.h"
#include "targets.h"

#include <SDL2/SDL.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <clocale>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string_view>
#include <thread>
#include <utility>

#include "../yoga/lib/nlohmann/json.hpp"

#if __has_include(<archive.h>) && __has_include(<archive_entry.h>)
#include <archive.h>
#include <archive_entry.h>
#define ASOBMSHOW_HAS_LIBARCHIVE 1
#else
#define ASOBMSHOW_HAS_LIBARCHIVE 0
#endif

#if __has_include(<iconv.h>)
#include <iconv.h>
#define ASOBMSHOW_HAS_ICONV 1
#else
#define ASOBMSHOW_HAS_ICONV 0
#endif

#if !ASOBMSHOW_HAS_LIBARCHIVE
#include "../bgfx/bimg/3rdparty/tinyexr/deps/miniz/miniz.h"
#endif

#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
#include "iOSNatives.hpp"
#else
#include <curl/curl.h>
#endif

namespace {
using json = nlohmann::json;

constexpr std::uint64_t kMaxDownloadBytes = 1024ull * 1024ull * 1024ull * 4ull;
constexpr const char *kHorieApiOrigin = "https://horie.synology.me:8443";

struct ParsedUrl {
  std::string scheme;
  std::string host;
  std::string path;
  std::string query;
};

struct DownloadCandidate {
  std::string originalUrl;
  std::string downloadUrl;
  std::string archiveName;
  bool supported = false;
  bool knownUnsupportedArchive = false;
  std::string reason;
};

struct ExtractedLink {
  std::string url;
  std::string label;
};

struct HorieArchiveNameParts {
  std::string artist;
  std::string title;
};

struct HorieLookupTerms {
  std::string title;
  std::string artist;
};

struct HorieCandidateSearchResult {
  std::vector<json> candidates;
  std::string sourceUrl;
  std::string errorMessage;
};

struct PackageSourceLookupResult {
  std::string sourceName;
  std::string sourceUrl;
  std::optional<DownloadCandidate> candidate;
  std::string errorMessage;
};

class GoogleDriveDriver {
public:
  static bool isHost(const ParsedUrl &url);
  static std::optional<DownloadCandidate> classify(const std::string &url);
  static bool resolveWarningDownload(
      const std::string &downloadUrl, const std::string &displayUrl,
      const std::filesystem::path &archivePath, std::atomic_bool &cancelled,
      std::string &errorMessage,
      BmsSearchDownloadProgressCallback progressCallback);

private:
  static std::optional<std::string> fileId(const std::string &url);
  static std::optional<std::string> fileIdFromUrls(
      const std::string &downloadUrl, const std::string &displayUrl);
  static std::optional<std::string> confirmationUrl(
      const std::string &fileId, const std::string &html,
      const std::string &baseUrl);
};

class DropboxDriver {
public:
  static bool isHost(const ParsedUrl &url);
  static std::optional<DownloadCandidate> classify(const std::string &url);

private:
  static std::string forceDownloadUrl(const std::string &url);
};

class DirectArchiveDriver {
public:
  static std::optional<DownloadCandidate> classify(const std::string &url);
};

class IpfsDriver {
public:
  static std::optional<DownloadCandidate> classify(const std::string &url);

private:
  static std::optional<std::string> pathFromUrl(const std::string &url);
};

class BmsSearchDriver {
public:
  static bool isHost(const ParsedUrl &url);
  static std::vector<std::string> bmsLinks(const std::string &patternUrl,
                                           const std::string &html);
  static std::vector<DownloadCandidate>
  downloadCandidates(const std::string &bmsUrl, const std::string &html);
};

class GingerRushDriver {
public:
  static PackageSourceLookupResult lookupByMd5(const std::string &md5);
};

class KonmaiDriver {
public:
  static PackageSourceLookupResult lookupByMd5(const std::string &md5);
};

class WriggleDriver {
public:
  static PackageSourceLookupResult lookupByMd5(const std::string &md5);
};

class EndlessDreamSourcesDriver {
public:
  static bool tryDownloadByMd5(
      const std::string &md5, const std::string &archiveKey,
      const std::filesystem::path &libraryRoot, std::atomic_bool &cancelled,
      BmsSearchDownloadProgressCallback progressCallback,
      BmsSearchResult &result);
};

class HorieYuukaDriver {
public:
  static std::vector<std::string>
  searchQueries(const std::string &title, const std::string &artist,
                const std::string &sha256, const std::string &md5);
  static bool tryDownload(
      const std::vector<std::string> &queries, const std::string &title,
      const std::string &artist, bool requireTitleMatch,
      const std::string &archiveKey, const std::filesystem::path &libraryRoot,
      std::atomic_bool &cancelled,
      BmsSearchDownloadProgressCallback progressCallback,
      BmsSearchResult &result);
  static bool downloadCandidateById(
      const BmsSearchCandidate &candidate, const std::string &archiveKey,
      const std::filesystem::path &libraryRoot, std::atomic_bool &cancelled,
      BmsSearchDownloadProgressCallback progressCallback,
      BmsSearchResult &result);

private:
  static std::string searchUrl(const std::string &folder,
                               const std::string &query);
  static BmsSearchCandidate candidateFromJson(const json &item,
                                              const std::string &query,
                                              const std::string &sourceUrl);
  static HorieCandidateSearchResult
  findCandidates(const std::string &query, const std::string &title,
                 const std::string &artist, bool requireTitleMatch,
                 bool requireArtistMatch);
};

bool downloadUrlToFile(const std::string &url, const std::filesystem::path &path,
                       std::atomic_bool &cancelled, std::string &errorMessage,
                       BmsSearchDownloadProgressCallback progressCallback);
std::vector<std::string> extractLinks(const std::string &baseUrl,
                                      const std::string &html);
std::vector<ExtractedLink> extractLinkRefs(const std::string &baseUrl,
                                           const std::string &html);
std::string urlEncode(const std::string &value);

std::string trimCopy(const std::string &value) {
  const auto begin =
      std::find_if_not(value.begin(), value.end(),
                       [](unsigned char c) { return std::isspace(c) != 0; });
  const auto end =
      std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) {
        return std::isspace(c) != 0;
      }).base();
  if (begin >= end) {
    return "";
  }
  return std::string(begin, end);
}

std::string lowerCopy(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool endsWith(std::string_view value, std::string_view suffix) {
  return value.size() >= suffix.size() &&
         value.substr(value.size() - suffix.size()) == suffix;
}

bool hostMatches(const std::string &host, std::string_view domain) {
  return std::string_view(host) == domain ||
         (host.size() > domain.size() &&
          host.ends_with(std::string(".") + std::string(domain)));
}

std::string replaceAll(std::string value, std::string_view needle,
                       std::string_view replacement) {
  if (needle.empty()) {
    return value;
  }
  size_t pos = 0;
  while ((pos = value.find(needle, pos)) != std::string::npos) {
    value.replace(pos, needle.size(), replacement);
    pos += replacement.size();
  }
  return value;
}

std::string htmlDecode(std::string value) {
  value = replaceAll(std::move(value), "&amp;", "&");
  value = replaceAll(std::move(value), "&quot;", "\"");
  value = replaceAll(std::move(value), "&#39;", "'");
  value = replaceAll(std::move(value), "&#x27;", "'");
  value = replaceAll(std::move(value), "&lt;", "<");
  value = replaceAll(std::move(value), "&gt;", ">");
  return value;
}

std::string normalizeEmbeddedUrl(std::string value) {
  value = htmlDecode(std::move(value));
  value = replaceAll(std::move(value), "\\u002F", "/");
  value = replaceAll(std::move(value), "\\/", "/");
  value = replaceAll(std::move(value), "\\u0026", "&");
  value = replaceAll(std::move(value), "\\u003D", "=");
  return value;
}

std::optional<ParsedUrl> parseUrl(const std::string &url) {
  const auto schemePos = url.find("://");
  if (schemePos == std::string::npos) {
    return std::nullopt;
  }
  ParsedUrl parsed;
  parsed.scheme = lowerCopy(url.substr(0, schemePos));
  const size_t authorityStart = schemePos + 3;
  const size_t pathStart = url.find('/', authorityStart);
  std::string authority =
      pathStart == std::string::npos ? url.substr(authorityStart)
                                     : url.substr(authorityStart,
                                                  pathStart - authorityStart);
  const auto atPos = authority.rfind('@');
  if (atPos != std::string::npos) {
    authority = authority.substr(atPos + 1);
  }
  const auto portPos = authority.find(':');
  parsed.host = lowerCopy(portPos == std::string::npos
                              ? authority
                              : authority.substr(0, portPos));

  const size_t queryStart =
      pathStart == std::string::npos ? std::string::npos
                                     : url.find('?', pathStart);
  if (pathStart == std::string::npos) {
    parsed.path = "/";
  } else if (queryStart == std::string::npos) {
    const size_t fragmentStart = url.find('#', pathStart);
    parsed.path = fragmentStart == std::string::npos
                      ? url.substr(pathStart)
                      : url.substr(pathStart, fragmentStart - pathStart);
  } else {
    parsed.path = url.substr(pathStart, queryStart - pathStart);
  }
  if (queryStart != std::string::npos) {
    const size_t fragmentStart = url.find('#', queryStart);
    parsed.query =
        fragmentStart == std::string::npos
            ? url.substr(queryStart + 1)
            : url.substr(queryStart + 1, fragmentStart - queryStart - 1);
  }
  return parsed;
}

bool hasUrlScheme(const std::string &value) {
  const size_t colon = value.find(':');
  if (colon == std::string::npos || colon == 0) {
    return false;
  }
  const size_t firstDelimiter = value.find_first_of("/?#");
  if (firstDelimiter != std::string::npos && firstDelimiter < colon) {
    return false;
  }
  if (std::isalpha(static_cast<unsigned char>(value.front())) == 0) {
    return false;
  }
  for (size_t i = 1; i < colon; ++i) {
    const unsigned char c = static_cast<unsigned char>(value[i]);
    if (std::isalnum(c) == 0 && c != '+' && c != '-' && c != '.') {
      return false;
    }
  }
  return true;
}

std::string trimUrlForBase(std::string url) {
  const auto fragment = url.find('#');
  if (fragment != std::string::npos) {
    url.erase(fragment);
  }
  const auto query = url.find('?');
  if (query != std::string::npos) {
    url.erase(query);
  }
  return url;
}

std::string resolveUrl(const std::string &baseUrl, const std::string &link) {
  if (hasUrlScheme(link)) {
    return link;
  }

  const auto schemePos = baseUrl.find("://");
  if (schemePos == std::string::npos) {
    return link;
  }

  const auto originStart = schemePos + 3;
  const auto pathStart = baseUrl.find('/', originStart);
  const std::string origin =
      pathStart == std::string::npos ? baseUrl : baseUrl.substr(0, pathStart);

  if (link.starts_with("//")) {
    return baseUrl.substr(0, schemePos) + ":" + link;
  }
  if (link.starts_with("/")) {
    return origin + link;
  }

  const std::string cleanBase = trimUrlForBase(baseUrl);
  const auto slash = cleanBase.rfind('/');
  const std::string directory = slash == std::string::npos
                                    ? origin + "/"
                                    : cleanBase.substr(0, slash + 1);
  std::vector<std::string> parts;
  std::stringstream stream(directory.substr(origin.size()) + link);
  std::string part;
  while (std::getline(stream, part, '/')) {
    if (part.empty() || part == ".") {
      continue;
    }
    if (part == "..") {
      if (!parts.empty()) {
        parts.pop_back();
      }
      continue;
    }
    parts.push_back(part);
  }

  std::string resolved = origin;
  for (const auto &urlPart : parts) {
    resolved += "/";
    resolved += urlPart;
  }
  return resolved;
}

std::string queryParam(const ParsedUrl &url, const std::string &name) {
  std::stringstream stream(url.query);
  std::string part;
  while (std::getline(stream, part, '&')) {
    const auto eq = part.find('=');
    const std::string key = eq == std::string::npos ? part : part.substr(0, eq);
    if (key == name) {
      return eq == std::string::npos ? "" : part.substr(eq + 1);
    }
  }
  return "";
}

std::string urlDecode(const std::string &value, bool plusAsSpace = false) {
  std::string result;
  result.reserve(value.size());
  for (size_t i = 0; i < value.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(value[i]);
    if (c == '%' && i + 2 < value.size() &&
        std::isxdigit(static_cast<unsigned char>(value[i + 1])) != 0 &&
        std::isxdigit(static_cast<unsigned char>(value[i + 2])) != 0) {
      const std::string hex = value.substr(i + 1, 2);
      result.push_back(static_cast<char>(std::strtoul(hex.c_str(), nullptr, 16)));
      i += 2;
    } else if (plusAsSpace && c == '+') {
      result.push_back(' ');
    } else {
      result.push_back(static_cast<char>(c));
    }
  }
  return result;
}

std::string setQueryParameter(std::string url, const std::string &name,
                              const std::string &value,
                              const std::set<std::string> &removeNames = {}) {
  std::string fragment;
  const size_t fragmentStart = url.find('#');
  if (fragmentStart != std::string::npos) {
    fragment = url.substr(fragmentStart);
    url.erase(fragmentStart);
  }

  std::string base = url;
  std::string query;
  const size_t queryStart = url.find('?');
  if (queryStart != std::string::npos) {
    base = url.substr(0, queryStart);
    query = url.substr(queryStart + 1);
  }

  std::vector<std::string> parts;
  std::stringstream stream(query);
  std::string part;
  while (std::getline(stream, part, '&')) {
    if (part.empty()) {
      continue;
    }
    const size_t eq = part.find('=');
    const std::string key = eq == std::string::npos ? part : part.substr(0, eq);
    if (key == name || removeNames.contains(key)) {
      continue;
    }
    parts.push_back(part);
  }
  parts.push_back(name + "=" + urlEncode(value));

  std::string rebuilt = base + "?";
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i > 0) {
      rebuilt += "&";
    }
    rebuilt += parts[i];
  }
  return rebuilt + fragment;
}

std::string jsonStringAt(const json &object, const char *key,
                         const std::string &fallback = "") {
  if (!object.is_object()) {
    return fallback;
  }
  const auto it = object.find(key);
  if (it == object.end() || it->is_null()) {
    return fallback;
  }
  if (it->is_string()) {
    return it->get<std::string>();
  }
  if (it->is_number_integer()) {
    return std::to_string(it->get<long long>());
  }
  if (it->is_number_unsigned()) {
    return std::to_string(it->get<unsigned long long>());
  }
  return fallback;
}

std::string urlEncode(const std::string &value) {
  std::ostringstream stream;
  stream << std::uppercase << std::hex;
  for (unsigned char c : value) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      stream << static_cast<char>(c);
    } else if (c == ' ') {
      stream << '+';
    } else {
      stream << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(c)
             << std::setfill(' ');
    }
  }
  return stream.str();
}

std::string fileNameFromUrl(const std::string &url) {
  const auto parsed = parseUrl(url);
  if (!parsed) {
    return "";
  }
  std::filesystem::path path(parsed->path);
  return lowerCopy(urlDecode(path.filename().string()));
}

std::string displayFileNameFromUrl(const std::string &url) {
  const auto parsed = parseUrl(url);
  if (!parsed) {
    return "";
  }
  std::filesystem::path path(parsed->path);
  return trimCopy(urlDecode(path.filename().string()));
}

std::string archiveExtensionFromName(std::string name) {
  name = lowerCopy(std::move(name));
  static constexpr std::array<std::string_view, 15> kArchiveExtensions = {
      ".tar.bz2", ".tar.gz", ".tar.xz", ".tbz2", ".tgz",
      ".txz",     ".zip",    ".7z",     ".rar",  ".lzh",
      ".lha",     ".tar",    ".bz2",    ".gz",   ".xz",
  };
  for (const std::string_view extension : kArchiveExtensions) {
    if (endsWith(name, extension)) {
      return std::string(extension);
    }
  }
  return "";
}

std::string stripArchiveExtension(std::string name) {
  const std::string extension = archiveExtensionFromName(name);
  if (!extension.empty() && name.size() >= extension.size()) {
    name.erase(name.size() - extension.size());
  }
  return trimCopy(name);
}

std::string safeStorageKey(const std::string &value) {
  std::string result;
  result.reserve(std::min<size_t>(value.size(), 128));
  for (unsigned char c : value) {
    if (c >= 0x80 || std::isalnum(c) || c == '-' || c == '_' || c == '.' ||
        c == ' ' || c == '[' || c == ']' || c == '(' || c == ')') {
      result.push_back(static_cast<char>(c));
    } else if (!result.empty() && result.back() != '_') {
      result.push_back('_');
    }
    if (result.size() >= 128) {
      break;
    }
  }
  while (!result.empty() &&
         (result.back() == '_' || result.back() == ' ' || result.back() == '.')) {
    result.pop_back();
  }
  return result.empty() ? "archive" : result;
}

std::string archiveNameFromUrl(const std::string &url) {
  const std::string name = displayFileNameFromUrl(url);
  return archiveExtensionFromName(name).empty() ? std::string() : name;
}

std::string preferredArchiveName(const std::string &suggestedArchiveName,
                                 const std::string &displayUrl,
                                 const std::string &downloadUrl,
                                 const std::string &archiveKey,
                                 const std::string &archiveExtension) {
  std::string name = trimCopy(suggestedArchiveName);
  if (archiveExtensionFromName(name).empty()) {
    name = archiveNameFromUrl(displayUrl);
  }
  if (archiveExtensionFromName(name).empty()) {
    name = archiveNameFromUrl(downloadUrl);
  }

  if (!name.empty()) {
    return safeStorageKey(name);
  }

  std::string fallback =
      archiveKey.empty()
          ? std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count())
          : archiveKey;
  fallback = safeStorageKey(fallback);
  return fallback + (archiveExtension.empty() ? ".archive" : archiveExtension);
}

std::string storageKeyFromArchiveName(const std::string &archiveName) {
  std::string key = stripArchiveExtension(archiveName);
  if (key.empty()) {
    key = archiveName;
  }
  return safeStorageKey(key);
}

std::string collapseWhitespace(const std::string &value) {
  std::string result;
  bool lastWasSpace = true;
  for (unsigned char c : value) {
    if (std::isspace(c) != 0) {
      if (!lastWasSpace) {
        result.push_back(' ');
      }
      lastWasSpace = true;
    } else {
      result.push_back(static_cast<char>(c));
      lastWasSpace = false;
    }
  }
  return trimCopy(result);
}

std::string plainTextFromHtmlFragment(const std::string &html) {
  static const std::regex tagPattern(R"(<[^>]*>)", std::regex::icase);
  return collapseWhitespace(
      htmlDecode(std::regex_replace(html, tagPattern, " ")));
}

std::string archiveNameFromText(const std::string &text) {
  std::string cleaned = plainTextFromHtmlFragment(text);
  for (const std::string &prefix : {"Download ", "download "}) {
    if (cleaned.starts_with(prefix)) {
      cleaned = trimCopy(cleaned.substr(prefix.size()));
      break;
    }
  }
  return archiveExtensionFromName(cleaned).empty() ? std::string() : cleaned;
}

HorieArchiveNameParts parseHorieArchiveName(const json &item) {
  HorieArchiveNameParts parts;
  std::string name = stripArchiveExtension(jsonStringAt(item, "name"));
  if (name.empty()) {
    name = stripArchiveExtension(jsonStringAt(item, "relativePath"));
    const size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) {
      name = name.substr(slash + 1);
    }
  }

  if (!name.empty() && name.front() == '[') {
    const size_t close = name.find(']');
    if (close != std::string::npos) {
      parts.artist = trimCopy(name.substr(1, close - 1));
      parts.title = trimCopy(name.substr(close + 1));
      return parts;
    }
  }

  parts.title = trimCopy(name);
  return parts;
}

std::string archiveExtensionFromUrl(const std::string &url) {
  return archiveExtensionFromName(fileNameFromUrl(url));
}

bool isRecognizedArchiveExtension(const std::string &extension) {
  return !extension.empty() &&
         !archiveExtensionFromName("download" + extension).empty();
}

bool isSupportedArchiveExtension(const std::string &extension) {
  if (extension == ".zip") {
    return true;
  }
#if ASOBMSHOW_HAS_LIBARCHIVE
  return isRecognizedArchiveExtension(extension);
#else
  return false;
#endif
}

bool isArchiveContentType(std::string contentType) {
  contentType = lowerCopy(trimCopy(std::move(contentType)));
  return contentType == "application/zip" ||
         contentType == "application/x-zip-compressed" ||
         contentType == "application/x-7z-compressed" ||
         contentType == "application/vnd.rar" ||
         contentType == "application/x-rar-compressed" ||
         contentType == "application/x-lzh-compressed" ||
         contentType == "application/x-lha" ||
         contentType == "application/x-tar" ||
         contentType == "application/gzip" ||
         contentType == "application/x-gzip" ||
         contentType == "application/x-bzip2" ||
         contentType == "application/x-xz";
}

std::string googleSearchUrlForText(const std::string &query) {
  std::string trimmed = trimCopy(query);
  if (trimmed.empty()) {
    trimmed = "BMS chart";
  }
  return "https://www.google.com/search?q=" + urlEncode("BMS " + trimmed);
}

std::string bmsSearchUrlForText(const std::string &query) {
  std::string trimmed = trimCopy(query);
  if (trimmed.empty()) {
    trimmed = "BMS chart";
  }
  return std::string(BmsSearchService::kBaseUrl) + "/search?q=" +
         urlEncode(trimmed);
}

std::string normalizedSearchText(const std::string &value) {
  std::string result;
  bool lastWasSpace = true;
  for (unsigned char c : value) {
    if (std::isalnum(c)) {
      result.push_back(static_cast<char>(std::tolower(c)));
      lastWasSpace = false;
    } else if (c >= 0x80) {
      result.push_back(static_cast<char>(c));
      lastWasSpace = false;
    } else if (!lastWasSpace) {
      result.push_back(' ');
      lastWasSpace = true;
    }
  }
  if (!result.empty() && result.back() == ' ') {
    result.pop_back();
  }
  return result;
}

std::vector<std::string> splitSearchTokens(const std::string &value) {
  std::vector<std::string> tokens;
  std::istringstream stream(normalizedSearchText(value));
  std::string token;
  while (stream >> token) {
    tokens.push_back(token);
  }
  return tokens;
}

bool containsNonAscii(const std::string &value) {
  return std::any_of(value.begin(), value.end(), [](unsigned char c) {
    return c >= 0x80;
  });
}

size_t utf8CodepointCount(const std::string &value) {
  return static_cast<size_t>(
      std::count_if(value.begin(), value.end(), [](unsigned char c) {
        return (c & 0xc0) != 0x80;
      }));
}

bool isMeaningfulSearchToken(const std::string &token) {
  return token.size() >= 4 || containsNonAscii(token);
}

bool requiresExactTitleTokenMatch(const std::string &normalizedTitle) {
  return containsNonAscii(normalizedTitle) &&
         utf8CodepointCount(normalizedTitle) <= 2;
}

bool normalizedHaystackHasExactToken(const std::string &haystack,
                                     const std::string &needle) {
  std::istringstream stream(haystack);
  std::string token;
  while (stream >> token) {
    if (token == needle) {
      return true;
    }
  }
  return false;
}

bool normalizedHaystackMatchesTitle(const std::string &haystack,
                                    const std::string &normalizedTitle) {
  if (normalizedTitle.empty()) {
    return false;
  }
  if (requiresExactTitleTokenMatch(normalizedTitle)) {
    return normalizedHaystackHasExactToken(haystack, normalizedTitle);
  }
  return haystack.find(normalizedTitle) != std::string::npos;
}

bool allowsRawSubstringTitleMatch(const std::string &normalizedTitle) {
  return !requiresExactTitleTokenMatch(normalizedTitle) &&
         (normalizedTitle.size() >= 4 || containsNonAscii(normalizedTitle));
}

bool isLooseQuerySeparator(char c) {
  switch (c) {
  case '-':
  case '_':
  case '/':
  case ',':
  case ':':
  case ';':
  case '|':
  case '~':
  case '(':
  case ')':
  case '[':
  case ']':
  case '{':
  case '}':
    return true;
  default:
    return false;
  }
}

std::string collapseWhitespaceCopy(const std::string &value) {
  std::string result;
  bool lastWasSpace = true;
  for (unsigned char c : value) {
    if (std::isspace(c) != 0) {
      if (!lastWasSpace) {
        result.push_back(' ');
        lastWasSpace = true;
      }
      continue;
    }
    result.push_back(static_cast<char>(c));
    lastWasSpace = false;
  }
  if (!result.empty() && result.back() == ' ') {
    result.pop_back();
  }
  return result;
}

std::string cleanupDecoratedQueryText(std::string value) {
  value = collapseWhitespaceCopy(trimCopy(value));
  while (!value.empty() &&
         isLooseQuerySeparator(static_cast<unsigned char>(value.front()))) {
    value.erase(value.begin());
    value = trimCopy(value);
  }
  while (!value.empty() &&
         isLooseQuerySeparator(static_cast<unsigned char>(value.back()))) {
    value.pop_back();
    value = trimCopy(value);
  }
  return collapseWhitespaceCopy(value);
}

bool looksLikeTitleDecoration(const std::string &value) {
  const auto tokens = splitSearchTokens(value);
  if (tokens.empty()) {
    return false;
  }

  static constexpr std::string_view kDecorationTokens[] = {
      "another", "hyper", "normal", "beginner", "easy", "hard",
      "insane",  "black", "lunatic", "legend", "extra",    "ex",
      "ent",     "entry", "entrance", "sp",     "dp",       "spn",
      "sph",     "spa",   "dpn",     "dph",     "dpa",      "iidx",
      "bga"};
  for (const auto &token : tokens) {
    if (token == "key" || token == "keys" ||
        token.find("key") != std::string::npos) {
      return true;
    }
    if (std::find(std::begin(kDecorationTokens), std::end(kDecorationTokens),
                  token) != std::end(kDecorationTokens)) {
      return true;
    }
  }
  return false;
}

bool looksLikeArtistDecoration(const std::string &value) {
  const std::string lower = lowerCopy(value);
  return lower.find("obj.") != std::string::npos ||
         lower.find("obj:") != std::string::npos ||
         lower.find("obj-") != std::string::npos ||
         lower.find("obj ") != std::string::npos ||
         lower.find("object.") != std::string::npos ||
         lower.find("object:") != std::string::npos ||
         lower.find("object-") != std::string::npos ||
         lower.find("object ") != std::string::npos;
}

std::string stripBracketedDecorations(
    std::string value, char open, char close,
    const std::function<bool(const std::string &)> &isDecoration) {
  size_t position = 0;
  while (position < value.size()) {
    const size_t start = value.find(open, position);
    if (start == std::string::npos) {
      break;
    }
    const size_t end = value.find(close, start + 1);
    if (end == std::string::npos) {
      break;
    }

    const std::string inner = value.substr(start + 1, end - start - 1);
    if (isDecoration(inner)) {
      value.erase(start, end - start + 1);
      position = start;
      continue;
    }
    position = end + 1;
  }
  return cleanupDecoratedQueryText(value);
}

std::string stripTrailingSquareBracketDecorations(std::string value) {
  value = trimCopy(std::move(value));
  while (!value.empty() && value.back() == ']') {
    const size_t open = value.rfind('[');
    if (open == std::string::npos) {
      break;
    }

    const std::string prefix = cleanupDecoratedQueryText(value.substr(0, open));
    if (prefix.empty()) {
      break;
    }
    value = prefix;
  }
  return cleanupDecoratedQueryText(value);
}

std::string stripTitleDecorations(const std::string &title) {
  std::string result = trimCopy(title);
  result = stripTrailingSquareBracketDecorations(std::move(result));
  result =
      stripBracketedDecorations(std::move(result), '[', ']',
                                [](const std::string &value) {
                                  return looksLikeTitleDecoration(value);
                                });
  result =
      stripBracketedDecorations(std::move(result), '(', ')',
                                [](const std::string &value) {
                                  return looksLikeTitleDecoration(value);
                                });
  result =
      stripBracketedDecorations(std::move(result), '{', '}',
                                [](const std::string &value) {
                                  return looksLikeTitleDecoration(value);
                                });
  return stripTrailingSquareBracketDecorations(std::move(result));
}

std::string stripArtistDecorations(const std::string &artist) {
  std::string result = trimCopy(artist);
  result =
      stripBracketedDecorations(std::move(result), '[', ']',
                                [](const std::string &value) {
                                  return looksLikeArtistDecoration(value);
                                });
  result =
      stripBracketedDecorations(std::move(result), '(', ')',
                                [](const std::string &value) {
                                  return looksLikeArtistDecoration(value);
                                });
  result =
      stripBracketedDecorations(std::move(result), '{', '}',
                                [](const std::string &value) {
                                  return looksLikeArtistDecoration(value);
                                });

  static const std::regex objectSegmentPattern(
      R"((^|[\s,/;&+|()\[\]{}-])\bobj(?:ect)?\b[.:-]?\s*(?:\([^)]*\)|\[[^\]]*\]|\{[^}]*\}|[^,/;&+|()\[\]{}-]+))",
      std::regex::icase);
  result = std::regex_replace(result, objectSegmentPattern, "$1");
  return cleanupDecoratedQueryText(result);
}

std::string stripArtistAfterSlash(const std::string &artist) {
  const std::string trimmed = trimCopy(artist);
  size_t slash = trimmed.find('/');
  const size_t fullWidthSlash = trimmed.find("／");
  if (fullWidthSlash != std::string::npos &&
      (slash == std::string::npos || fullWidthSlash < slash)) {
    slash = fullWidthSlash;
  }
  if (slash == std::string::npos) {
    return cleanupDecoratedQueryText(trimmed);
  }
  return cleanupDecoratedQueryText(trimmed.substr(0, slash));
}

bool titleNeedsExactCandidateMatch(const std::string &title) {
  const std::string normalizedTitle = normalizedSearchText(title);
  const std::string normalizedMinimalTitle =
      normalizedSearchText(stripTitleDecorations(title));
  return requiresExactTitleTokenMatch(normalizedTitle) ||
         requiresExactTitleTokenMatch(normalizedMinimalTitle);
}

std::optional<HorieLookupTerms>
splitCombinedHorieLookupTerms(const std::string &value) {
  const std::string trimmed = trimCopy(value);
  for (size_t i = trimmed.size(); i > 0; --i) {
    const unsigned char c = static_cast<unsigned char>(trimmed[i - 1]);
    if (std::isspace(c) == 0) {
      continue;
    }

    HorieLookupTerms terms;
    terms.artist = trimCopy(trimmed.substr(0, i - 1));
    terms.title = trimCopy(trimmed.substr(i));
    if (terms.artist.empty() || terms.title.empty()) {
      return std::nullopt;
    }

    const std::string normalizedTitle =
        normalizedSearchText(stripTitleDecorations(terms.title));
    if (!requiresExactTitleTokenMatch(normalizedTitle)) {
      return std::nullopt;
    }

    if (!containsNonAscii(terms.artist) &&
        !looksLikeArtistDecoration(terms.artist)) {
      return std::nullopt;
    }
    return terms;
  }
  return std::nullopt;
}

HorieLookupTerms horieLookupTermsForMeta(const std::string &title,
                                         const std::string &artist) {
  HorieLookupTerms terms{trimCopy(title), trimCopy(artist)};
  if (const auto splitTerms = splitCombinedHorieLookupTerms(terms.title)) {
    if (terms.artist.empty()) {
      return *splitTerms;
    }

    const std::string normalizedArtist =
        normalizedSearchText(stripArtistDecorations(terms.artist));
    const std::string normalizedSplitArtist =
        normalizedSearchText(stripArtistDecorations(splitTerms->artist));
    if (!normalizedArtist.empty() && !normalizedSplitArtist.empty() &&
        (normalizedSplitArtist.find(normalizedArtist) != std::string::npos ||
         normalizedArtist.find(normalizedSplitArtist) != std::string::npos)) {
      terms.title = splitTerms->title;
    }
  }
  return terms;
}

void appendUniqueQuery(std::vector<std::string> &queries,
                       const std::string &query) {
  const std::string trimmed = trimCopy(query);
  if (trimmed.empty()) {
    return;
  }

  const std::string normalized = normalizedSearchText(trimmed);
  const std::string key = normalized.empty() ? lowerCopy(trimmed) : normalized;
  for (const auto &existing : queries) {
    const std::string existingNormalized = normalizedSearchText(existing);
    const std::string existingKey =
        existingNormalized.empty() ? lowerCopy(existing) : existingNormalized;
    if (existingKey == key) {
      return;
    }
  }
  queries.push_back(trimmed);
}

std::vector<std::string> horieArtistQueryVariants(const std::string &artist) {
  std::vector<std::string> variants;
  const std::string trimmedArtist = trimCopy(artist);
  const std::string objectStrippedArtist =
      stripArtistDecorations(trimmedArtist);
  const std::string slashStrippedArtist = stripArtistAfterSlash(trimmedArtist);
  const std::string slashObjectStrippedArtist =
      stripArtistAfterSlash(objectStrippedArtist);

  appendUniqueQuery(variants, trimmedArtist);
  appendUniqueQuery(variants, objectStrippedArtist);
  appendUniqueQuery(variants, slashStrippedArtist);
  appendUniqueQuery(variants, slashObjectStrippedArtist);
  return variants;
}

std::vector<std::string> HorieYuukaDriver::searchQueries(
    const std::string &title, const std::string &artist,
    const std::string &sha256, const std::string &md5) {
  std::vector<std::string> queries;
  const std::string trimmedTitle = trimCopy(title);
  const std::string minimalTitle = stripTitleDecorations(trimmedTitle);
  const auto artistQueries = horieArtistQueryVariants(artist);

  auto appendArtistTitleQueries = [&](const std::string &titleQuery) {
    if (titleQuery.empty()) {
      return;
    }
    for (const auto &artistQuery : artistQueries) {
      if (!artistQuery.empty()) {
        appendUniqueQuery(queries, "[" + artistQuery + "] " + titleQuery);
      }
    }
  };

  appendArtistTitleQueries(trimmedTitle);
  if (normalizedSearchText(minimalTitle) != normalizedSearchText(trimmedTitle)) {
    appendArtistTitleQueries(minimalTitle);
  }

  appendUniqueQuery(queries, trimmedTitle);
  if (normalizedSearchText(minimalTitle) != normalizedSearchText(trimmedTitle)) {
    appendUniqueQuery(queries, minimalTitle);
  }

  appendUniqueQuery(queries, normalizedSearchText(trimmedTitle));
  if (normalizedSearchText(minimalTitle) != normalizedSearchText(trimmedTitle)) {
    appendUniqueQuery(queries, normalizedSearchText(minimalTitle));
  }

  auto titleTokens = splitSearchTokens(trimmedTitle);
  std::sort(titleTokens.begin(), titleTokens.end(),
            [](const std::string &lhs, const std::string &rhs) {
              if (lhs.size() != rhs.size()) {
                return lhs.size() > rhs.size();
              }
              return lhs < rhs;
            });
  int addedTitleTokens = 0;
  for (const auto &token : titleTokens) {
    if (!isMeaningfulSearchToken(token)) {
      continue;
    }
    appendUniqueQuery(queries, token);
    if (++addedTitleTokens >= 6) {
      break;
    }
  }

  appendUniqueQuery(queries, sha256);
  appendUniqueQuery(queries, md5);
  return queries;
}

bool artistMatchesArchiveResult(const json &item, const std::string &artist) {
  const auto artistVariants = horieArtistQueryVariants(artist);
  if (artistVariants.empty()) {
    return false;
  }

  const std::string haystack = normalizedSearchText(
      jsonStringAt(item, "title") + " " + jsonStringAt(item, "name") + " " +
      jsonStringAt(item, "relativePath"));
  for (const auto &variant : artistVariants) {
    const std::string normalizedArtist = normalizedSearchText(variant);
    if (!normalizedArtist.empty() &&
        haystack.find(normalizedArtist) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool titleMatchesArchiveResult(const json &item, const std::string &title) {
  const std::string rawTitle = trimCopy(title);
  const std::string minimalTitle = stripTitleDecorations(rawTitle);
  const std::string normalizedTitle = normalizedSearchText(title);
  const std::string normalizedMinimalTitle =
      normalizedSearchText(minimalTitle);
  if (requiresExactTitleTokenMatch(normalizedTitle) ||
      requiresExactTitleTokenMatch(normalizedMinimalTitle)) {
    const auto nameParts = parseHorieArchiveName(item);
    const std::string archiveTitle =
        normalizedSearchText(stripTitleDecorations(nameParts.title));
    return (!normalizedTitle.empty() && archiveTitle == normalizedTitle) ||
           (!normalizedMinimalTitle.empty() &&
            archiveTitle == normalizedMinimalTitle);
  }

  const std::string rawHaystack =
      jsonStringAt(item, "title") + " " + jsonStringAt(item, "name") + " " +
      jsonStringAt(item, "relativePath");
  if (allowsRawSubstringTitleMatch(normalizedTitle) && rawTitle.size() >= 4 &&
      lowerCopy(rawHaystack).find(lowerCopy(rawTitle)) != std::string::npos) {
    return true;
  }
  if (allowsRawSubstringTitleMatch(normalizedMinimalTitle) &&
      minimalTitle.size() >= 4 &&
      lowerCopy(rawHaystack).find(lowerCopy(minimalTitle)) !=
          std::string::npos) {
    return true;
  }

  if (normalizedTitle.size() < 4 && !containsNonAscii(normalizedTitle) &&
      normalizedMinimalTitle.size() < 4 &&
      !containsNonAscii(normalizedMinimalTitle)) {
    return false;
  }
  const std::string haystack = normalizedSearchText(rawHaystack);
  if (normalizedHaystackMatchesTitle(haystack, normalizedTitle)) {
    return true;
  }
  if (normalizedHaystackMatchesTitle(haystack, normalizedMinimalTitle)) {
    return true;
  }

  const auto titleTokens = splitSearchTokens(
      normalizedMinimalTitle.empty() ? normalizedTitle : normalizedMinimalTitle);
  if (requiresExactTitleTokenMatch(normalizedMinimalTitle.empty()
                                       ? normalizedTitle
                                       : normalizedMinimalTitle)) {
    return false;
  }
  std::vector<std::string> meaningfulTokens;
  for (const auto &token : titleTokens) {
    if (isMeaningfulSearchToken(token)) {
      meaningfulTokens.push_back(token);
    }
  }
  if (meaningfulTokens.empty()) {
    return false;
  }

  int matchedTokens = 0;
  for (const auto &token : meaningfulTokens) {
    if (haystack.find(token) != std::string::npos) {
      ++matchedTokens;
    }
  }
  if (meaningfulTokens.size() == 1) {
    return matchedTokens == 1;
  }
  const int requiredMatches =
      std::max(2, static_cast<int>((meaningfulTokens.size() * 2 + 2) / 3));
  return matchedTokens >= requiredMatches;
}

bool BmsSearchDriver::isHost(const ParsedUrl &url) {
  return hostMatches(url.host, "bmssearch.net");
}

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

std::optional<std::string> htmlAttributeValue(const std::string &tag,
                                              const std::string &attribute) {
  const std::regex attributePattern(
      attribute + R"(\s*=\s*["']([^"']*)["'])", std::regex::icase);
  std::smatch match;
  if (std::regex_search(tag, match, attributePattern) && match.size() >= 2) {
    return htmlDecode(match[1].str());
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

bool safeArchivePath(const std::string &name, std::filesystem::path &outPath) {
  if (name.empty() || name.find('\0') != std::string::npos) {
    return false;
  }
  std::string normalized = replaceAll(name, "\\", "/");
  std::filesystem::path relative(normalized);
  if (relative.empty() || relative.is_absolute() || relative.has_root_path()) {
    return false;
  }
  for (const auto &part : relative) {
    if (part == ".." || part == ".") {
      return false;
    }
  }
  outPath = relative;
  return true;
}

bool isValidUtf8(const std::string &value) {
  const auto *bytes = reinterpret_cast<const unsigned char *>(value.data());
  size_t i = 0;
  while (i < value.size()) {
    const unsigned char c = bytes[i];
    if (c <= 0x7f) {
      ++i;
      continue;
    }
    if (c >= 0xc2 && c <= 0xdf) {
      if (i + 1 >= value.size() || (bytes[i + 1] & 0xc0) != 0x80) {
        return false;
      }
      i += 2;
      continue;
    }
    if (c == 0xe0) {
      if (i + 2 >= value.size() || bytes[i + 1] < 0xa0 ||
          bytes[i + 1] > 0xbf || (bytes[i + 2] & 0xc0) != 0x80) {
        return false;
      }
      i += 3;
      continue;
    }
    if ((c >= 0xe1 && c <= 0xec) || c == 0xee || c == 0xef) {
      if (i + 2 >= value.size() || (bytes[i + 1] & 0xc0) != 0x80 ||
          (bytes[i + 2] & 0xc0) != 0x80) {
        return false;
      }
      i += 3;
      continue;
    }
    if (c == 0xed) {
      if (i + 2 >= value.size() || bytes[i + 1] < 0x80 ||
          bytes[i + 1] > 0x9f || (bytes[i + 2] & 0xc0) != 0x80) {
        return false;
      }
      i += 3;
      continue;
    }
    if (c == 0xf0) {
      if (i + 3 >= value.size() || bytes[i + 1] < 0x90 ||
          bytes[i + 1] > 0xbf || (bytes[i + 2] & 0xc0) != 0x80 ||
          (bytes[i + 3] & 0xc0) != 0x80) {
        return false;
      }
      i += 4;
      continue;
    }
    if (c >= 0xf1 && c <= 0xf3) {
      if (i + 3 >= value.size() || (bytes[i + 1] & 0xc0) != 0x80 ||
          (bytes[i + 2] & 0xc0) != 0x80 ||
          (bytes[i + 3] & 0xc0) != 0x80) {
        return false;
      }
      i += 4;
      continue;
    }
    if (c == 0xf4) {
      if (i + 3 >= value.size() || bytes[i + 1] < 0x80 ||
          bytes[i + 1] > 0x8f || (bytes[i + 2] & 0xc0) != 0x80 ||
          (bytes[i + 3] & 0xc0) != 0x80) {
        return false;
      }
      i += 4;
      continue;
    }
    return false;
  }
  return true;
}

#if ASOBMSHOW_HAS_ICONV
std::optional<std::string> convertTextToUtf8(const std::string &input,
                                             const char *fromEncoding) {
  iconv_t converter = iconv_open("UTF-8", fromEncoding);
  if (converter == reinterpret_cast<iconv_t>(-1)) {
    return std::nullopt;
  }

  std::string output(std::max<size_t>(input.size() * 4, 32), '\0');
  char *inputPtr = const_cast<char *>(input.data());
  size_t inputBytes = input.size();
  size_t outputOffset = 0;

  while (inputBytes > 0) {
    char *outputPtr = output.data() + outputOffset;
    size_t outputBytes = output.size() - outputOffset;
    const size_t status =
        iconv(converter, &inputPtr, &inputBytes, &outputPtr, &outputBytes);
    outputOffset = output.size() - outputBytes;
    if (status != static_cast<size_t>(-1)) {
      continue;
    }
    if (errno != E2BIG) {
      iconv_close(converter);
      return std::nullopt;
    }
    output.resize(output.size() * 2);
  }

  iconv_close(converter);
  output.resize(outputOffset);
  if (!isValidUtf8(output)) {
    return std::nullopt;
  }
  return output;
}
#endif

#if ASOBMSHOW_HAS_LIBARCHIVE
std::string archiveEntryPathnameUtf8(archive_entry *entry) {
  if (entry == nullptr) {
    return "";
  }
  if (const char *utf8Name = archive_entry_pathname_utf8(entry);
      utf8Name != nullptr && utf8Name[0] != '\0') {
    return utf8Name;
  }
  if (const wchar_t *wideName = archive_entry_pathname_w(entry);
      wideName != nullptr && wideName[0] != L'\0') {
    return ws2s_utf8(wideName);
  }

  const char *rawName = archive_entry_pathname(entry);
  if (rawName == nullptr || rawName[0] == '\0') {
    return "";
  }

  const std::string raw(rawName);
  if (isValidUtf8(raw)) {
    return raw;
  }

#if ASOBMSHOW_HAS_ICONV
  for (const char *encoding : {"CP932", "SHIFT_JIS", "SHIFT-JIS", "SJIS"}) {
    if (const auto converted = convertTextToUtf8(raw, encoding)) {
      return *converted;
    }
  }
#endif
  return raw;
}
#endif

#if !ASOBMSHOW_HAS_LIBARCHIVE
bool hasZipSignature(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  std::array<unsigned char, 4> bytes{};
  file.read(reinterpret_cast<char *>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  if (file.gcount() < 4) {
    return false;
  }
  return bytes == std::array<unsigned char, 4>{'P', 'K', 0x03, 0x04} ||
         bytes == std::array<unsigned char, 4>{'P', 'K', 0x05, 0x06} ||
         bytes == std::array<unsigned char, 4>{'P', 'K', 0x07, 0x08};
}

bool extractZipArchive(const std::filesystem::path &archivePath,
                       const std::filesystem::path &outputPath,
                       std::string &errorMessage,
                       BmsSearchDownloadProgressCallback progressCallback) {
  std::error_code fsError;
  std::filesystem::create_directories(outputPath, fsError);
  if (fsError) {
    errorMessage = "Could not create output folder: " + fsError.message();
    return false;
  }

  mz_zip_archive archive{};
  mz_zip_zero_struct(&archive);
  const std::string archiveText =
      path_t_to_utf8(fspath_to_path_t(archivePath));
  if (!mz_zip_reader_init_file(&archive, archiveText.c_str(), 0)) {
    errorMessage = "Could not open ZIP archive.";
    return false;
  }

  const mz_uint fileCount = mz_zip_reader_get_num_files(&archive);
  int extractedFiles = 0;
  bool ok = true;
  for (mz_uint i = 0; i < fileCount; ++i) {
    mz_zip_archive_file_stat stat{};
    if (!mz_zip_reader_file_stat(&archive, i, &stat)) {
      ok = false;
      errorMessage = "Could not read ZIP directory entry.";
      break;
    }

    std::filesystem::path relativePath;
    if (!safeArchivePath(stat.m_filename, relativePath)) {
      continue;
    }
    const std::filesystem::path destination = outputPath / relativePath;
    if (mz_zip_reader_is_file_a_directory(&archive, i)) {
      std::filesystem::create_directories(destination, fsError);
      if (fsError) {
        ok = false;
        errorMessage = "Could not create archive folder: " + fsError.message();
        break;
      }
      continue;
    }

    std::filesystem::create_directories(destination.parent_path(), fsError);
    if (fsError) {
      ok = false;
      errorMessage = "Could not create archive folder: " + fsError.message();
      break;
    }

    if (progressCallback) {
      progressCallback({.message = "Extracting " + relativePath.string(),
                        .downloadedBytes = i,
                        .totalBytes = fileCount});
    }

    const std::string destinationText =
        path_t_to_utf8(fspath_to_path_t(destination));
    if (!mz_zip_reader_extract_to_file(&archive, i, destinationText.c_str(),
                                       0)) {
      ok = false;
      errorMessage = "Could not extract " + relativePath.string();
      break;
    }
    ++extractedFiles;
  }
  mz_zip_reader_end(&archive);

  if (!ok) {
    return false;
  }
  if (extractedFiles == 0) {
    errorMessage = "ZIP archive did not contain extractable files.";
    return false;
  }
  return true;
}
#endif

#if ASOBMSHOW_HAS_LIBARCHIVE
bool localeNameLooksUtf8(const char *localeName) {
  if (localeName == nullptr || localeName[0] == '\0') {
    return false;
  }
  const std::string lower = lowerCopy(localeName);
  return lower.find("utf-8") != std::string::npos ||
         lower.find("utf8") != std::string::npos;
}

bool localeNameLooksShiftJis(const char *localeName) {
  if (localeName == nullptr || localeName[0] == '\0') {
    return false;
  }
  const std::string lower = lowerCopy(localeName);
  return lower.find("shift") != std::string::npos ||
         lower.find("sjis") != std::string::npos ||
         lower.find("cp932") != std::string::npos ||
         lower.find("ms_kanji") != std::string::npos;
}

bool localeNameLooksArchiveCompatible(const char *localeName) {
  return localeNameLooksUtf8(localeName) ||
         localeNameLooksShiftJis(localeName);
}

void ensureArchiveFilenameLocale() {
  static std::once_flag localeInitFlag;
  std::call_once(localeInitFlag, []() {
    if (localeNameLooksArchiveCompatible(std::setlocale(LC_CTYPE, nullptr))) {
      return;
    }

    for (const char *candidate :
         {"", "C.UTF-8", "en_US.UTF-8", "ja_JP.UTF-8", "ko_KR.UTF-8",
          "UTF-8", "ja_JP.SJIS", "ja_JP.Shift_JIS", "ja_JP.CP932",
          "Shift_JIS", "CP932", "SJIS"}) {
      const char *selected = std::setlocale(LC_CTYPE, candidate);
      if (localeNameLooksArchiveCompatible(selected)) {
        return;
      }
    }

    SDL_Log("Could not set UTF-8 or Shift-JIS LC_CTYPE locale for archive "
            "filenames.");
  });
}

std::string archiveErrorString(archive *archiveHandle,
                               const std::string &fallback) {
  if (archiveHandle == nullptr ||
      archive_error_string(archiveHandle) == nullptr) {
    return fallback;
  }
  return archive_error_string(archiveHandle);
}

bool trySetArchiveHeaderCharset(archive *archiveHandle, const char *charset) {
  if (archiveHandle == nullptr || charset == nullptr || charset[0] == '\0') {
    return false;
  }

  bool applied = false;
  for (const char *format : {"zip", "rar", "lha", "tar", "cab", "cpio"}) {
    const int status =
        archive_read_set_option(archiveHandle, format, "hdrcharset", charset);
    if (status == ARCHIVE_OK || status == ARCHIVE_WARN) {
      applied = true;
    }
  }
  return applied;
}

void preferJapaneseArchiveHeaderCharset(archive *archiveHandle) {
  for (const char *charset : {"CP932", "SHIFT_JIS", "SHIFT-JIS", "SJIS"}) {
    if (trySetArchiveHeaderCharset(archiveHandle, charset)) {
      return;
    }
  }
}

bool extractArchiveWithLibarchive(
    const std::filesystem::path &archivePath,
    const std::filesystem::path &outputPath, std::string &errorMessage,
    BmsSearchDownloadProgressCallback progressCallback) {
  std::error_code fsError;
  std::filesystem::create_directories(outputPath, fsError);
  if (fsError) {
    errorMessage = "Could not create output folder: " + fsError.message();
    return false;
  }

  ensureArchiveFilenameLocale();
  archive *archiveHandle = archive_read_new();
  if (archiveHandle == nullptr) {
    errorMessage = "Could not initialize archive reader.";
    return false;
  }
  archive_read_support_filter_all(archiveHandle);
  archive_read_support_format_all(archiveHandle);
  archive_read_support_format_raw(archiveHandle);
  preferJapaneseArchiveHeaderCharset(archiveHandle);

  const std::string archiveText =
      path_t_to_utf8(fspath_to_path_t(archivePath));
  int status =
      archive_read_open_filename(archiveHandle, archiveText.c_str(), 10240);
  if (status != ARCHIVE_OK) {
    errorMessage =
        "Could not open archive: " + archiveErrorString(archiveHandle, "");
    archive_read_free(archiveHandle);
    return false;
  }

  bool ok = true;
  int extractedFiles = 0;
  int skippedInvalidPaths = 0;
  int skippedUnsupportedTypes = 0;
  int directoryEntries = 0;
  std::uint64_t entryIndex = 0;
  archive_entry *entry = nullptr;
  for (;;) {
    status = archive_read_next_header(archiveHandle, &entry);
    if (status == ARCHIVE_EOF) {
      break;
    }
    if (status == ARCHIVE_RETRY) {
      continue;
    }
    if (status < ARCHIVE_WARN) {
      ok = false;
      errorMessage =
          "Could not read archive: " + archiveErrorString(archiveHandle, "");
      break;
    }
    if (status == ARCHIVE_WARN) {
      SDL_Log("Continuing after archive warning: %s",
              archiveErrorString(archiveHandle, "").c_str());
    }
    if (entry == nullptr) {
      archive_read_data_skip(archiveHandle);
      continue;
    }

    ++entryIndex;
    const std::string entryName = archiveEntryPathnameUtf8(entry);
    const bool entryNameLooksDirectory =
        !entryName.empty() &&
        (entryName.back() == '/' || entryName.back() == '\\');

    std::filesystem::path relativePath;
    if (!safeArchivePath(entryName, relativePath)) {
      ++skippedInvalidPaths;
      archive_read_data_skip(archiveHandle);
      continue;
    }

    const std::filesystem::path destination = outputPath / relativePath;
    const auto fileType = archive_entry_filetype(entry);
    const bool fileTypeIsSet = archive_entry_filetype_is_set(entry) != 0;
    const bool unknownFileType = !fileTypeIsSet || fileType == 0;
    if (fileType == AE_IFDIR ||
        (unknownFileType && entryNameLooksDirectory)) {
      ++directoryEntries;
      std::filesystem::create_directories(destination, fsError);
      if (fsError) {
        ok = false;
        errorMessage = "Could not create archive folder: " + fsError.message();
        break;
      }
      continue;
    }
    if (!unknownFileType && fileType != AE_IFREG) {
      ++skippedUnsupportedTypes;
      archive_read_data_skip(archiveHandle);
      continue;
    }

    std::filesystem::create_directories(destination.parent_path(), fsError);
    if (fsError) {
      ok = false;
      errorMessage = "Could not create archive folder: " + fsError.message();
      break;
    }

    if (progressCallback) {
      progressCallback({.message = "Extracting " + relativePath.string(),
                        .downloadedBytes = entryIndex,
                        .totalBytes = 0});
    }

    std::ofstream output(destination, std::ios::binary);
    if (!output) {
      ok = false;
      errorMessage = "Could not create " + relativePath.string();
      break;
    }

    std::array<char, 64 * 1024> buffer{};
    for (;;) {
      const la_ssize_t bytes =
          archive_read_data(archiveHandle, buffer.data(), buffer.size());
      if (bytes == 0) {
        break;
      }
      if (bytes < 0) {
        ok = false;
        errorMessage = "Could not extract " + relativePath.string() + ": " +
                       archiveErrorString(archiveHandle, "");
        break;
      }
      output.write(buffer.data(), static_cast<std::streamsize>(bytes));
      if (!output) {
        ok = false;
        errorMessage = "Could not write " + relativePath.string();
        break;
      }
    }
    if (!ok) {
      break;
    }
    ++extractedFiles;
  }

  archive_read_free(archiveHandle);

  if (!ok) {
    return false;
  }
  if (extractedFiles == 0) {
    errorMessage = "Archive did not contain extractable files.";
    if (entryIndex > 0) {
      errorMessage += " Entries=" + std::to_string(entryIndex) +
                      ", folders=" + std::to_string(directoryEntries) +
                      ", unsafe=" + std::to_string(skippedInvalidPaths) +
                      ", unsupported=" +
                      std::to_string(skippedUnsupportedTypes) + ".";
    }
    return false;
  }
  return true;
}
#endif

bool extractDownloadedArchive(
    const std::filesystem::path &archivePath,
    const std::filesystem::path &outputPath, std::string &errorMessage,
    BmsSearchDownloadProgressCallback progressCallback) {
#if ASOBMSHOW_HAS_LIBARCHIVE
  return extractArchiveWithLibarchive(archivePath, outputPath, errorMessage,
                                      progressCallback);
#else
  if (!hasZipSignature(archivePath)) {
    errorMessage =
        "Downloaded response was not a ZIP archive. Open the source manually.";
    return false;
  }
  return extractZipArchive(archivePath, outputPath, errorMessage,
                           progressCallback);
#endif
}

#if (TARGET_OS_IOS || TARGET_OS_SIMULATOR) && defined(DEBUG)
void writeArchiveEntryDiagnostics(const std::filesystem::path &archivePath,
                                  const std::filesystem::path &outputPath) {
#if ASOBMSHOW_HAS_LIBARCHIVE
  std::ofstream output(outputPath);
  if (!output) {
    return;
  }

  ensureArchiveFilenameLocale();
  archive *archiveHandle = archive_read_new();
  if (archiveHandle == nullptr) {
    output << "Could not initialize archive reader.\n";
    return;
  }
  archive_read_support_filter_all(archiveHandle);
  archive_read_support_format_all(archiveHandle);
  archive_read_support_format_raw(archiveHandle);
  preferJapaneseArchiveHeaderCharset(archiveHandle);

  const std::string archiveText =
      path_t_to_utf8(fspath_to_path_t(archivePath));
  int status =
      archive_read_open_filename(archiveHandle, archiveText.c_str(), 10240);
  if (status != ARCHIVE_OK) {
    output << "Could not open archive: "
           << archiveErrorString(archiveHandle, "") << '\n';
    archive_read_free(archiveHandle);
    return;
  }

  std::uint64_t entryIndex = 0;
  archive_entry *entry = nullptr;
  for (;;) {
    status = archive_read_next_header(archiveHandle, &entry);
    if (status == ARCHIVE_EOF) {
      break;
    }
    if (status == ARCHIVE_RETRY) {
      continue;
    }
    if (status < ARCHIVE_WARN) {
      output << "Could not read archive: "
             << archiveErrorString(archiveHandle, "") << '\n';
      break;
    }
    ++entryIndex;
    output << entryIndex << '\t';
    if (entry == nullptr) {
      output << "entry=null\n";
      archive_read_data_skip(archiveHandle);
      continue;
    }
    output << "name=" << archiveEntryPathnameUtf8(entry)
           << "\ttype_set=" << archive_entry_filetype_is_set(entry)
           << "\ttype=" << archive_entry_filetype(entry)
           << "\tsize_set=" << archive_entry_size_is_set(entry)
           << "\tsize=" << archive_entry_size(entry) << '\n';
    archive_read_data_skip(archiveHandle);
  }
  archive_read_free(archiveHandle);
#else
  (void)archivePath;
  (void)outputPath;
#endif
}
#endif

bool isBmsChartPath(const std::filesystem::path &path) {
  const std::string ext = lowerCopy(path.extension().string());
  return ext == ".bms" || ext == ".bme" || ext == ".bml";
}

bool containsBmsFile(const std::filesystem::path &root) {
  std::error_code error;
  std::filesystem::recursive_directory_iterator iterator(
      root, std::filesystem::directory_options::skip_permission_denied, error);
  for (const auto end = std::filesystem::recursive_directory_iterator();
       !error && iterator != end; iterator.increment(error)) {
    if (!iterator->is_regular_file(error) || error) {
      error.clear();
      continue;
    }
    if (isBmsChartPath(iterator->path())) {
      return true;
    }
  }
  return false;
}

bool isHexStringOfLength(const std::string &value, size_t length) {
  return value.size() == length &&
         std::all_of(value.begin(), value.end(), [](unsigned char c) {
           return std::isxdigit(c) != 0;
         });
}

std::optional<std::vector<unsigned char>>
readFileBytes(const std::filesystem::path &path, std::string &errorMessage) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error) {
    errorMessage = "Could not read chart size: " + error.message();
    return std::nullopt;
  }

  std::ifstream file(path, std::ios::binary);
  if (!file) {
    errorMessage = "Could not open chart file for hash verification.";
    return std::nullopt;
  }

  std::vector<unsigned char> bytes(static_cast<size_t>(size));
  if (!bytes.empty()) {
    file.read(reinterpret_cast<char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
  }
  if (!file && !file.eof()) {
    errorMessage = "Could not read chart file for hash verification.";
    return std::nullopt;
  }
  return bytes;
}

std::optional<std::filesystem::path> findMatchingBmsChartByHash(
    const std::filesystem::path &root, const std::string &archiveKey,
    std::string &errorMessage) {
  const std::string key = lowerCopy(trimCopy(archiveKey));
  const bool matchSha256 = isHexStringOfLength(key, 64);
  const bool matchMd5 = isHexStringOfLength(key, 32);
  if (!matchSha256 && !matchMd5) {
    return std::nullopt;
  }

  bool sawBmsFile = false;
  std::error_code error;
  std::filesystem::recursive_directory_iterator iterator(
      root, std::filesystem::directory_options::skip_permission_denied, error);
  for (const auto end = std::filesystem::recursive_directory_iterator();
       !error && iterator != end; iterator.increment(error)) {
    if (!iterator->is_regular_file(error) || error) {
      error.clear();
      continue;
    }
    if (!isBmsChartPath(iterator->path())) {
      continue;
    }
    sawBmsFile = true;

    std::string readError;
    const auto bytes = readFileBytes(iterator->path(), readError);
    if (!bytes) {
      if (!readError.empty()) {
        SDL_Log("Skipping BMS hash verification for %s: %s",
                iterator->path().string().c_str(), readError.c_str());
      }
      continue;
    }

    if (matchSha256 && lowerCopy(bms_parser::sha256(*bytes)) == key) {
      return iterator->path();
    }
    if (matchMd5) {
      const std::string text(bytes->begin(), bytes->end());
      if (lowerCopy(bms_parser::md5(text)) == key) {
        return iterator->path();
      }
    }
  }

  if (error) {
    errorMessage = "Could not scan extracted archive: " + error.message();
  } else if (sawBmsFile) {
    errorMessage = "Archive did not contain the selected BMS chart.";
  } else {
    errorMessage = "Archive did not contain a BMS chart file.";
  }
  return std::nullopt;
}

std::optional<std::string>
htmlBodyFromDownloadedFile(const std::filesystem::path &path) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error || size == 0 || size > 4 * 1024 * 1024) {
    return std::nullopt;
  }

  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return std::nullopt;
  }
  std::string body(size, '\0');
  file.read(body.data(), static_cast<std::streamsize>(body.size()));
  if (!file && !file.eof()) {
    return std::nullopt;
  }

  const std::string leading =
      lowerCopy(trimCopy(body.substr(0, std::min<size_t>(body.size(), 512))));
  if (leading.starts_with("<!doctype html") ||
      leading.starts_with("<html") || leading.find("<html") != std::string::npos ||
      leading.find("<head") != std::string::npos ||
      leading.find("<body") != std::string::npos) {
    return body;
  }
  return std::nullopt;
}

class ScopedFileRemoval {
public:
  explicit ScopedFileRemoval(std::filesystem::path path)
      : path(std::move(path)) {}

  ~ScopedFileRemoval() {
    if (path.empty()) {
      return;
    }

    std::error_code error;
    if (!std::filesystem::exists(path, error) || error) {
      return;
    }
    error.clear();
    std::filesystem::remove(path, error);
    if (error) {
      SDL_Log("Could not delete downloaded archive %s: %s",
              path_t_to_utf8(fspath_to_path_t(path)).c_str(),
              error.message().c_str());
    }
  }

private:
  std::filesystem::path path;
};

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

std::optional<std::filesystem::path> saveIosDebugArtifacts(
    const std::string &key, const std::string &downloadUrl,
    const std::string &displayUrl, const std::filesystem::path &archivePath,
    const std::filesystem::path &extractDirectory, std::string &errorMessage) {
#if (TARGET_OS_IOS || TARGET_OS_SIMULATOR) && defined(DEBUG)
  const std::string attemptId =
      key + "-" +
      std::to_string(
          std::chrono::system_clock::now().time_since_epoch().count());
  const std::filesystem::path debugDirectory =
      Utils::GetDocumentsPath("BMSSEARCH_DEBUG") / attemptId;
  std::error_code fsError;
  std::filesystem::create_directories(debugDirectory, fsError);
  if (fsError) {
    errorMessage = "Could not create debug folder: " + fsError.message();
    return std::nullopt;
  }

  if (!archivePath.empty() && std::filesystem::exists(archivePath, fsError)) {
    fsError.clear();
    std::filesystem::copy_file(
        archivePath, debugDirectory / archivePath.filename(),
        std::filesystem::copy_options::overwrite_existing, fsError);
    if (fsError) {
      errorMessage = "Could not copy downloaded archive: " + fsError.message();
      return std::nullopt;
    }
    writeArchiveEntryDiagnostics(archivePath,
                                 debugDirectory / "archive_entries.txt");
  }
  fsError.clear();

  if (!extractDirectory.empty() &&
      std::filesystem::exists(extractDirectory, fsError)) {
    fsError.clear();
    std::filesystem::copy(
        extractDirectory, debugDirectory / "extracted",
        std::filesystem::copy_options::recursive |
            std::filesystem::copy_options::overwrite_existing,
        fsError);
    if (fsError) {
      errorMessage = "Could not copy extracted files: " + fsError.message();
      return std::nullopt;
    }
  }
  fsError.clear();

  std::ofstream metadata(debugDirectory / "metadata.txt");
  if (!metadata) {
    errorMessage = "Could not create debug metadata.";
    return std::nullopt;
  }
  metadata << "download_url=" << downloadUrl << '\n';
  metadata << "display_url=" << displayUrl << '\n';
  metadata << "archive_path="
           << path_t_to_utf8(fspath_to_path_t(archivePath)) << '\n';
  metadata << "extract_path="
           << path_t_to_utf8(fspath_to_path_t(extractDirectory)) << '\n';
  metadata.close();
  if (!metadata) {
    errorMessage = "Could not write debug metadata.";
    return std::nullopt;
  }

  return debugDirectory;
#else
  (void)key;
  (void)downloadUrl;
  (void)displayUrl;
  (void)archivePath;
  (void)extractDirectory;
  (void)errorMessage;
  return std::nullopt;
#endif
}

#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
struct IOSDownloadProgressContext {
  BmsSearchDownloadProgressCallback *progressCallback = nullptr;
};

void reportIOSDownloadProgress(void *context, std::uint64_t downloadedBytes,
                               std::uint64_t totalBytes) {
  auto *progressContext =
      static_cast<IOSDownloadProgressContext *>(context);
  if (progressContext == nullptr ||
      progressContext->progressCallback == nullptr ||
      !*progressContext->progressCallback) {
    return;
  }
  (*progressContext->progressCallback)(
      {.message = "Downloading archive",
       .downloadedBytes = downloadedBytes,
       .totalBytes = totalBytes});
}

std::optional<std::string> fetchUrlText(const std::string &url,
                                        std::string &errorMessage) {
  std::string body;
  if (!DownloadURLTextIOS(url, body, errorMessage)) {
    return std::nullopt;
  }
  return body;
}

std::optional<std::string> postUrlText(const std::string &url,
                                       std::string &errorMessage) {
  std::string body;
  if (!PostURLTextIOS(url, body, errorMessage)) {
    return std::nullopt;
  }
  return body;
}

bool downloadUrlToFile(const std::string &url, const std::filesystem::path &path,
                       std::atomic_bool &cancelled, std::string &errorMessage,
                       BmsSearchDownloadProgressCallback progressCallback) {
  if (cancelled.load()) {
    errorMessage = "Download cancelled.";
    return false;
  }
  if (progressCallback) {
    progressCallback({.message = "Downloading archive"});
  }
  std::vector<unsigned char> data;
  IOSDownloadProgressContext progressContext{
      .progressCallback = &progressCallback};
  if (!DownloadURLBinaryIOS(url, data, errorMessage,
                            reportIOSDownloadProgress, &progressContext)) {
    return false;
  }
  if (cancelled.load()) {
    errorMessage = "Download cancelled.";
    return false;
  }
  if (data.size() > kMaxDownloadBytes) {
    errorMessage = "Download is too large.";
    return false;
  }
  std::ofstream file(path, std::ios::binary);
  if (!file) {
    errorMessage = "Could not create downloaded archive.";
    return false;
  }
  file.write(reinterpret_cast<const char *>(data.data()),
             static_cast<std::streamsize>(data.size()));
  if (!file) {
    errorMessage = "Could not write downloaded archive.";
    return false;
  }
  if (progressCallback) {
    progressCallback({.message = "Download complete",
                      .downloadedBytes = data.size(),
                      .totalBytes = data.size()});
  }
  return true;
}
#else
std::once_flag curlInitFlag;

size_t appendCurlResponse(char *ptr, size_t size, size_t nmemb,
                          void *userdata) {
  const size_t byteCount = size * nmemb;
  auto *response = static_cast<std::string *>(userdata);
  response->append(ptr, byteCount);
  return byteCount;
}

std::optional<std::string> fetchUrlText(const std::string &url,
                                        std::string &errorMessage) {
  std::call_once(curlInitFlag, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
  CURL *curl = curl_easy_init();
  if (curl == nullptr) {
    errorMessage = "Failed to initialize HTTP client.";
    return std::nullopt;
  }

  std::string body;
  char curlError[CURL_ERROR_SIZE] = {};
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 8L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "AsoBMaShow");
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 25L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, appendCurlResponse);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curlError);
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");

  const CURLcode result = curl_easy_perform(curl);
  long statusCode = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &statusCode);
  curl_easy_cleanup(curl);

  if (result != CURLE_OK) {
    errorMessage = curlError[0] != '\0' ? curlError : curl_easy_strerror(result);
    return std::nullopt;
  }
  if (statusCode >= 400) {
    errorMessage = "HTTP " + std::to_string(statusCode) + " while downloading " +
                   url;
    return std::nullopt;
  }
  return body;
}

std::optional<std::string> postUrlText(const std::string &url,
                                       std::string &errorMessage) {
  std::call_once(curlInitFlag, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
  CURL *curl = curl_easy_init();
  if (curl == nullptr) {
    errorMessage = "Failed to initialize HTTP client.";
    return std::nullopt;
  }

  std::string body;
  char curlError[CURL_ERROR_SIZE] = {};
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 8L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "AsoBMaShow");
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 25L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, appendCurlResponse);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curlError);
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");

  const CURLcode result = curl_easy_perform(curl);
  long statusCode = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &statusCode);
  curl_easy_cleanup(curl);

  if (result != CURLE_OK) {
    errorMessage = curlError[0] != '\0' ? curlError : curl_easy_strerror(result);
    return std::nullopt;
  }
  if (statusCode >= 400) {
    errorMessage = "HTTP " + std::to_string(statusCode) + " while posting " +
                   url;
    return std::nullopt;
  }
  return body;
}

struct CurlDownloadContext {
  std::ofstream *file = nullptr;
  std::atomic_bool *cancelled = nullptr;
  BmsSearchDownloadProgressCallback *progressCallback = nullptr;
};

size_t writeCurlFile(char *ptr, size_t size, size_t nmemb, void *userdata) {
  const size_t byteCount = size * nmemb;
  auto *context = static_cast<CurlDownloadContext *>(userdata);
  if (context->cancelled != nullptr && context->cancelled->load()) {
    return 0;
  }
  context->file->write(ptr, static_cast<std::streamsize>(byteCount));
  return *context->file ? byteCount : 0;
}

int curlProgress(void *userdata, curl_off_t downloadTotal,
                 curl_off_t downloadNow, curl_off_t, curl_off_t) {
  auto *context = static_cast<CurlDownloadContext *>(userdata);
  if (context->cancelled != nullptr && context->cancelled->load()) {
    return 1;
  }
  if (context->progressCallback != nullptr && *context->progressCallback) {
    (*context->progressCallback)(
        {.message = "Downloading archive",
         .downloadedBytes = static_cast<std::uint64_t>(downloadNow),
         .totalBytes = static_cast<std::uint64_t>(downloadTotal)});
  }
  if (downloadTotal > 0 &&
      static_cast<std::uint64_t>(downloadTotal) > kMaxDownloadBytes) {
    return 1;
  }
  return 0;
}

bool downloadUrlToFile(const std::string &url, const std::filesystem::path &path,
                       std::atomic_bool &cancelled, std::string &errorMessage,
                       BmsSearchDownloadProgressCallback progressCallback) {
  std::call_once(curlInitFlag, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
  CURL *curl = curl_easy_init();
  if (curl == nullptr) {
    errorMessage = "Failed to initialize HTTP client.";
    return false;
  }

  std::ofstream file(path, std::ios::binary);
  if (!file) {
    curl_easy_cleanup(curl);
    errorMessage = "Could not create downloaded archive.";
    return false;
  }

  CurlDownloadContext context{.file = &file,
                              .cancelled = &cancelled,
                              .progressCallback = &progressCallback};
  char curlError[CURL_ERROR_SIZE] = {};
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 8L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "AsoBMaShow");
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 180L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCurlFile);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &context);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curlProgress);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &context);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curlError);
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");

  const CURLcode result = curl_easy_perform(curl);
  long statusCode = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &statusCode);
  curl_easy_cleanup(curl);
  file.close();

  if (cancelled.load()) {
    errorMessage = "Download cancelled.";
    return false;
  }
  if (result != CURLE_OK) {
    errorMessage = curlError[0] != '\0' ? curlError : curl_easy_strerror(result);
    return false;
  }
  if (statusCode >= 400) {
    errorMessage = "HTTP " + std::to_string(statusCode) + " while downloading " +
                   url;
    return false;
  }
  return true;
}
#endif

std::filesystem::path makeDownloadDirectory(
    const std::filesystem::path &libraryRoot) {
  if (!libraryRoot.empty()) {
    return libraryRoot / "BMSSEARCH";
  }
  return Utils::GetDocumentsPath("BMS") / "BMSSEARCH";
}

bool downloadAndExtractArchive(
    const std::string &downloadUrl, const std::string &displayUrl,
    const std::string &archiveKey, const std::filesystem::path &libraryRoot,
    std::atomic_bool &cancelled,
    BmsSearchDownloadProgressCallback progressCallback,
    BmsSearchResult &result, const std::string &suggestedArchiveName = "",
    bool *downloadedArchive = nullptr) {
  if (downloadedArchive != nullptr) {
    *downloadedArchive = false;
  }
  result.downloadUrl = displayUrl.empty() ? downloadUrl : displayUrl;
  std::string archiveExtension = archiveExtensionFromUrl(result.downloadUrl);
  if (archiveExtension.empty()) {
    archiveExtension = archiveExtensionFromUrl(downloadUrl);
  }
  if (archiveExtension.empty()) {
    archiveExtension = archiveExtensionFromName(suggestedArchiveName);
  }
  if (archiveExtension.empty()) {
    archiveExtension = ".archive";
  }
  const std::string archiveName = preferredArchiveName(
      suggestedArchiveName, result.downloadUrl, downloadUrl, archiveKey,
      archiveExtension);
  const std::string key = storageKeyFromArchiveName(archiveName);
  const std::filesystem::path baseDirectory = makeDownloadDirectory(libraryRoot);
  const std::filesystem::path archiveDirectory = baseDirectory / "_archives";
  const std::filesystem::path extractDirectory = baseDirectory / key;
  std::error_code fsError;
  std::filesystem::create_directories(archiveDirectory, fsError);
  if (fsError) {
    result.status = BmsSearchResult::Status::DownloadFailed;
    result.message = "Could not create download folder: " + fsError.message();
    return false;
  }

  const std::filesystem::path archivePath = archiveDirectory / archiveName;
  ScopedFileRemoval archiveCleanup(archivePath);
  if (progressCallback) {
    progressCallback({.message = "Downloading archive"});
  }

  std::string downloadError;
  if (!downloadUrlToFile(downloadUrl, archivePath, cancelled, downloadError,
                         progressCallback)) {
    result.status = BmsSearchResult::Status::DownloadFailed;
    result.message =
        downloadError.empty() ? "Download failed." : downloadError;
    return false;
  }
  if (downloadedArchive != nullptr) {
    *downloadedArchive = true;
  }

  std::string driveWarningError;
  if (!GoogleDriveDriver::resolveWarningDownload(
          downloadUrl, result.downloadUrl, archivePath, cancelled,
          driveWarningError, progressCallback)) {
    std::string debugError;
    if (const auto debugPath = saveIosDebugArtifacts(
            key, downloadUrl, result.downloadUrl, archivePath, extractDirectory,
            debugError)) {
      result.debugPath = *debugPath;
    } else if (!debugError.empty()) {
      SDL_Log("Failed to save BMS Search debug artifacts: %s",
              debugError.c_str());
    }
    result.status = BmsSearchResult::Status::DownloadFailed;
    result.message =
        driveWarningError.empty() ? "Google Drive download failed."
                                  : driveWarningError;
    return false;
  }

  if (htmlBodyFromDownloadedFile(archivePath)) {
    std::string debugError;
    if (const auto debugPath = saveIosDebugArtifacts(
            key, downloadUrl, result.downloadUrl, archivePath, extractDirectory,
            debugError)) {
      result.debugPath = *debugPath;
    } else if (!debugError.empty()) {
      SDL_Log("Failed to save BMS Search debug artifacts: %s",
              debugError.c_str());
    }
    result.status = BmsSearchResult::Status::DownloadFailed;
    result.message =
        "Downloaded response was an HTML page instead of an archive.";
    return false;
  }

  if (progressCallback) {
    progressCallback({.message = "Extracting archive"});
  }

  std::string extractError;
  if (!extractDownloadedArchive(archivePath, extractDirectory, extractError,
                                progressCallback)) {
    std::string debugError;
    if (const auto debugPath = saveIosDebugArtifacts(
            key, downloadUrl, result.downloadUrl, archivePath, extractDirectory,
            debugError)) {
      result.debugPath = *debugPath;
    } else if (!debugError.empty()) {
      SDL_Log("Failed to save BMS Search debug artifacts: %s",
              debugError.c_str());
    }
    result.status = BmsSearchResult::Status::DownloadFailed;
    result.message =
        extractError.empty() ? "Archive extraction failed." : extractError;
    return false;
  }

  const std::string normalizedArchiveKey = lowerCopy(trimCopy(archiveKey));
  const bool requiresHashMatch =
      isHexStringOfLength(normalizedArchiveKey, 64) ||
      isHexStringOfLength(normalizedArchiveKey, 32);
  const bool foundBmsFile = containsBmsFile(extractDirectory);
  if (requiresHashMatch) {
    std::string hashError;
    if (!findMatchingBmsChartByHash(extractDirectory, normalizedArchiveKey,
                                    hashError)) {
      std::string debugError;
      if (const auto debugPath = saveIosDebugArtifacts(
              key, downloadUrl, result.downloadUrl, archivePath,
              extractDirectory, debugError)) {
        result.debugPath = *debugPath;
      } else if (!debugError.empty()) {
        SDL_Log("Failed to save BMS Search debug artifacts: %s",
                debugError.c_str());
      }
      result.status = BmsSearchResult::Status::HashMismatch;
      result.outputPath = extractDirectory;
      result.message = hashError.empty()
                           ? "Archive did not contain the selected BMS chart."
                           : hashError;
      return true;
    }
  } else if (!foundBmsFile) {
    std::string debugError;
    if (const auto debugPath = saveIosDebugArtifacts(
            key, downloadUrl, result.downloadUrl, archivePath, extractDirectory,
            debugError)) {
      result.debugPath = *debugPath;
    } else if (!debugError.empty()) {
      SDL_Log("Failed to save BMS Search debug artifacts: %s",
              debugError.c_str());
    }
  }

  result.status = BmsSearchResult::Status::Downloaded;
  result.outputPath = extractDirectory;
  result.message =
      foundBmsFile ? "Downloaded and extracted BMS archive."
                   : "Archive extracted, but no BMS file was found.";
  return true;
}

DownloadCandidate packageDownloadCandidate(const std::string &downloadUrl,
                                           const std::string &archiveName,
                                           const std::string &md5) {
  DownloadCandidate candidate = classifyLink(downloadUrl);
  candidate.originalUrl = downloadUrl;
  if (!candidate.supported) {
    candidate.downloadUrl = downloadUrl;
    candidate.supported = true;
    candidate.knownUnsupportedArchive = false;
    candidate.reason.clear();
  }

  const std::string suggestedArchiveName = trimCopy(archiveName);
  if (!suggestedArchiveName.empty() &&
      !archiveExtensionFromName(suggestedArchiveName).empty()) {
    candidate.archiveName = suggestedArchiveName;
  } else if (candidate.archiveName.empty()) {
    candidate.archiveName = md5 + ".7z";
  }
  return candidate;
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
    BmsSearchResult &result) {
  const std::string md5Hash = lowerCopy(trimCopy(md5));
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
    const bool finished = downloadAndExtractArchive(
        lookup.candidate->downloadUrl, lookup.candidate->originalUrl,
        archiveKey.empty() ? md5Hash : archiveKey, libraryRoot, cancelled,
        progressCallback, attempt, lookup.candidate->archiveName,
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

std::string HorieYuukaDriver::searchUrl(const std::string &folder,
                                        const std::string &query) {
  return std::string(kHorieApiOrigin) + "/api/v1/folders/" + folder +
         "/files?limit=5&offset=0&q=" + urlEncode(query);
}

BmsSearchCandidate HorieYuukaDriver::candidateFromJson(
    const json &item, const std::string &query, const std::string &sourceUrl) {
  BmsSearchCandidate candidate;
  candidate.source = BmsSearchCandidate::Source::Horie;
  candidate.id = jsonStringAt(item, "id");
  candidate.name = jsonStringAt(item, "name");
  const auto nameParts = parseHorieArchiveName(item);
  candidate.title = nameParts.title;
  candidate.artist = nameParts.artist;
  candidate.query = query;
  candidate.sourceUrl = sourceUrl;
  return candidate;
}

HorieCandidateSearchResult HorieYuukaDriver::findCandidates(
    const std::string &query, const std::string &title,
    const std::string &artist, bool requireTitleMatch,
    bool requireArtistMatch) {
  HorieCandidateSearchResult result;
  for (const char *folder : {"Songs"}) {
    result.sourceUrl = searchUrl(folder, query);
    const auto body = fetchUrlText(result.sourceUrl, result.errorMessage);
    if (!body) {
      continue;
    }

    json payload;
    try {
      payload = json::parse(*body);
    } catch (const std::exception &e) {
      result.errorMessage = std::string("Horie archive returned invalid JSON: ") +
                            e.what();
      continue;
    }
    auto itemsIt = payload.find("items");
    if (itemsIt == payload.end() || !itemsIt->is_array()) {
      itemsIt = payload.find("files");
    }
    if (itemsIt == payload.end() || !itemsIt->is_array()) {
      continue;
    }

    for (const auto &item : *itemsIt) {
      const std::string name = jsonStringAt(item, "name");
      const std::string contentType = jsonStringAt(item, "contentType");
      const std::string archiveExtension = archiveExtensionFromName(name);
      const std::string normalizedContentType =
          lowerCopy(trimCopy(contentType));
      const bool isArchive =
          isSupportedArchiveExtension(archiveExtension) ||
          normalizedContentType == "application/zip" ||
          (ASOBMSHOW_HAS_LIBARCHIVE &&
           isArchiveContentType(normalizedContentType));
      if (!isArchive) {
        continue;
      }
      if (requireTitleMatch && !titleMatchesArchiveResult(item, title)) {
        continue;
      }
      if (requireArtistMatch && !artistMatchesArchiveResult(item, artist)) {
        continue;
      }
      result.candidates.push_back(item);
    }
  }
  return result;
}

bool HorieYuukaDriver::tryDownload(
    const std::vector<std::string> &queries, const std::string &title,
    const std::string &artist, bool requireTitleMatch,
    const std::string &archiveKey, const std::filesystem::path &libraryRoot,
    std::atomic_bool &cancelled,
    BmsSearchDownloadProgressCallback progressCallback,
    BmsSearchResult &result) {
  std::optional<std::string> lastError;

  for (const auto &query : queries) {
    const std::string trimmedQuery = trimCopy(query);
    if (cancelled.load()) {
      result.status = BmsSearchResult::Status::DownloadFailed;
      result.message = "Lookup cancelled.";
      return true;
    }
    if (trimmedQuery.empty()) {
      continue;
    }
    if (progressCallback) {
      progressCallback({.message = "Searching Horie archive"});
    }

    const bool shouldRequireTitleMatch = requireTitleMatch;
    const bool shouldRequireArtistMatch =
        shouldRequireTitleMatch && !trimCopy(artist).empty() &&
        titleNeedsExactCandidateMatch(title);
    const auto searchResult = findCandidates(
        trimmedQuery, title, artist, shouldRequireTitleMatch,
        shouldRequireArtistMatch);
    if (searchResult.candidates.empty()) {
      if (!searchResult.errorMessage.empty()) {
        lastError = searchResult.errorMessage;
      }
      continue;
    }

    std::set<std::string> seenFileIds;
    result.candidates.clear();
    for (const auto &item : searchResult.candidates) {
      auto candidate =
          candidateFromJson(item, trimmedQuery, searchResult.sourceUrl);
      if (candidate.id.empty() || seenFileIds.contains(candidate.id)) {
        continue;
      }
      seenFileIds.insert(candidate.id);
      result.candidates.push_back(std::move(candidate));
    }

    if (result.candidates.empty()) {
      result.status = BmsSearchResult::Status::DownloadFailed;
      result.message = "Horie archive returned candidates without file IDs.";
      return true;
    }

    if (result.candidates.size() > 1) {
      result.status = BmsSearchResult::Status::AmbiguousCandidates;
      result.message = "Horie found multiple matching archives.";
      return true;
    }

    if (progressCallback) {
      progressCallback({.message = "Preparing Horie archive download"});
    }
    return downloadCandidateById(result.candidates.front(), archiveKey,
                                 libraryRoot, cancelled, progressCallback,
                                 result);
  }

  result.status = BmsSearchResult::Status::NotFound;
  result.message = lastError.value_or("Horie did not find a matching BMS archive.");
  return false;
}

bool HorieYuukaDriver::downloadCandidateById(
    const BmsSearchCandidate &candidate, const std::string &archiveKey,
    const std::filesystem::path &libraryRoot, std::atomic_bool &cancelled,
    BmsSearchDownloadProgressCallback progressCallback,
    BmsSearchResult &result) {
  if (candidate.source != BmsSearchCandidate::Source::Horie ||
      candidate.id.empty()) {
    result.status = BmsSearchResult::Status::DownloadFailed;
    result.message = "Selected download candidate is not valid.";
    return true;
  }

  const std::string grantUrl =
      std::string(kHorieApiOrigin) + "/api/v1/files/" + candidate.id +
      "/download-grants";
  std::string grantError;
  const auto grantBody = postUrlText(grantUrl, grantError);
  if (!grantBody) {
    result.status = BmsSearchResult::Status::DownloadFailed;
    result.message =
        grantError.empty() ? "Horie archive grant failed." : grantError;
    return true;
  }

  json grant;
  try {
    grant = json::parse(*grantBody);
  } catch (const std::exception &e) {
    result.status = BmsSearchResult::Status::DownloadFailed;
    result.message =
        std::string("Horie archive grant returned invalid JSON: ") + e.what();
    return true;
  }

  const std::string grantDownloadUrl = jsonStringAt(grant, "downloadUrl");
  if (grantDownloadUrl.empty()) {
    result.status = BmsSearchResult::Status::DownloadFailed;
    result.message = "Horie archive did not return a download URL.";
    return true;
  }

  const std::string absoluteUrl = resolveUrl(kHorieApiOrigin, grantDownloadUrl);
  downloadAndExtractArchive(absoluteUrl, absoluteUrl, archiveKey, libraryRoot,
                            cancelled, progressCallback, result,
                            candidate.name);
  result.candidates = {candidate};
  return true;
}

} // namespace

std::string BmsSearchService::patternUrlForSha256(const std::string &sha256) {
  return std::string(kBaseUrl) + "/patterns/" + lowerCopy(trimCopy(sha256));
}

std::string BmsSearchService::searchUrlForText(const std::string &query) {
  return bmsSearchUrlForText(query);
}

std::string
BmsSearchService::googleSearchUrlForSha256(const std::string &sha256) {
  return googleSearchUrlForText(lowerCopy(trimCopy(sha256)));
}

BmsSearchResult BmsSearchService::findAndDownload(
    const std::string &sha256, const std::string &md5,
    const std::filesystem::path &libraryRoot, std::atomic_bool &cancelled,
    BmsSearchDownloadProgressCallback progressCallback,
    const std::string &title, const std::string &artist) const {
  BmsSearchResult result;
  const std::string hash = lowerCopy(trimCopy(sha256));
  const std::string md5Hash = lowerCopy(trimCopy(md5));
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
            packageResult)) {
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
            progressCallback, horieResult)) {
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
                                 progressCallback, result,
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
    BmsSearchDownloadProgressCallback progressCallback) const {
  BmsSearchResult result;
  const std::string hash = lowerCopy(trimCopy(sha256));
  const std::string md5Hash = lowerCopy(trimCopy(md5));
  const std::string archiveKey = hash.empty() ? md5Hash : hash;
  if (progressCallback) {
    progressCallback({.message = "Preparing Horie archive download"});
  }
  HorieYuukaDriver::downloadCandidateById(candidate, archiveKey, libraryRoot,
                                          cancelled, progressCallback, result);
  return result;
}
