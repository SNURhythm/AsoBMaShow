#pragma once

#include "../BmsSearchService.h"
#include "../Utils.h"
#include "../bms_parser.hpp"
#include "../path.h"
#include "../targets.h"

#include <SDL2/SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
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
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "../../yoga/lib/nlohmann/json.hpp"

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
#include "../../bgfx/bimg/3rdparty/tinyexr/deps/miniz/miniz.h"
#endif

#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
#include "../iOSNatives.hpp"
#else
#include <curl/curl.h>
#endif

namespace asobmshow::bms_search {
using json = nlohmann::json;

inline constexpr std::uint64_t kMaxDownloadBytes =
    1024ull * 1024ull * 1024ull * 4ull;
inline constexpr const char *kHorieApiOrigin = "https://horie.synology.me:8443";

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

std::string trimCopy(const std::string &value);
std::string lowerCopy(std::string value);
bool endsWith(std::string_view value, std::string_view suffix);
bool hostMatches(const std::string &host, std::string_view domain);
std::string replaceAll(std::string value, std::string_view needle,
                       std::string_view replacement);
std::string htmlDecode(std::string value);
std::string normalizeEmbeddedUrl(std::string value);
std::optional<ParsedUrl> parseUrl(const std::string &url);
bool hasUrlScheme(const std::string &value);
std::string trimUrlForBase(std::string url);
std::string resolveUrl(const std::string &baseUrl, const std::string &link);
std::string queryParam(const ParsedUrl &url, const std::string &name);
std::string urlDecode(const std::string &value, bool plusAsSpace = false);
std::string setQueryParameter(std::string url, const std::string &name,
                              const std::string &value,
                              const std::set<std::string> &removeNames = {});
std::string jsonStringAt(const json &object, const char *key,
                         const std::string &fallback = "");
std::string urlEncode(const std::string &value);
std::string fileNameFromUrl(const std::string &url);
std::string displayFileNameFromUrl(const std::string &url);
std::string archiveExtensionFromName(std::string name);
std::string stripArchiveExtension(std::string name);
std::string safeStorageKey(const std::string &value);
std::string archiveNameFromUrl(const std::string &url);
std::string preferredArchiveName(const std::string &suggestedArchiveName,
                                 const std::string &displayUrl,
                                 const std::string &downloadUrl,
                                 const std::string &fallbackKey,
                                 const std::string &extension);
std::string storageKeyFromArchiveName(const std::string &archiveName);
std::string collapseWhitespace(const std::string &value);
std::string plainTextFromHtmlFragment(const std::string &html);
std::string archiveNameFromText(const std::string &text);
HorieArchiveNameParts parseHorieArchiveName(const json &item);
std::string archiveExtensionFromUrl(const std::string &url);
bool isRecognizedArchiveExtension(const std::string &extension);
bool isSupportedArchiveExtension(const std::string &extension);
bool isArchiveContentType(std::string contentType);
std::string googleSearchUrlForText(const std::string &query);
std::string bmsSearchUrlForText(const std::string &query);
std::string normalizedSearchText(const std::string &value);
std::vector<std::string> splitSearchTokens(const std::string &value);
bool containsNonAscii(const std::string &value);
bool isMeaningfulSearchToken(const std::string &token);
bool requiresExactTitleTokenMatch(const std::string &normalizedTitle);
bool normalizedHaystackHasExactToken(const std::string &haystack,
                                     const std::string &token);
bool normalizedHaystackMatchesTitle(const std::string &haystack,
                                    const std::string &title);
bool allowsRawSubstringTitleMatch(const std::string &normalizedTitle);
bool isLooseQuerySeparator(char c);
std::string collapseWhitespaceCopy(const std::string &value);
std::string cleanupDecoratedQueryText(std::string value);
bool looksLikeTitleDecoration(const std::string &value);
bool looksLikeArtistDecoration(const std::string &value);
std::string stripBracketedDecorations(
    std::string value, bool (*looksLikeDecoration)(const std::string &));
std::string stripTrailingSquareBracketDecorations(std::string value);
std::string stripTitleDecorations(const std::string &title);
std::string stripArtistDecorations(const std::string &artist);
std::string stripArtistAfterSlash(const std::string &artist);
bool titleNeedsExactCandidateMatch(const std::string &title);
HorieLookupTerms horieLookupTermsForMeta(
    const std::string &title, const std::string &artist);
void appendUniqueQuery(std::vector<std::string> &queries,
                       const std::string &query);
std::vector<std::string> horieArtistQueryVariants(const std::string &artist);
bool artistMatchesArchiveResult(const json &item, const std::string &artist);
bool titleMatchesArchiveResult(const json &item, const std::string &title);

std::optional<std::string> htmlAttributeValue(const std::string &tag,
                                              const std::string &attribute);
DownloadCandidate classifyLink(const std::string &url);
std::vector<std::string> extractLinks(const std::string &baseUrl,
                                      const std::string &html);
std::vector<ExtractedLink> extractLinkRefs(const std::string &baseUrl,
                                           const std::string &html);

bool safeArchivePath(const std::string &name, std::filesystem::path &outPath);
bool extractDownloadedArchive(
    const std::filesystem::path &archivePath,
    const std::filesystem::path &extractDirectory, std::string &errorMessage,
    BmsSearchDownloadProgressCallback progressCallback);
void writeArchiveEntryDiagnostics(const std::filesystem::path &archivePath,
                                  const std::filesystem::path &outputPath);
bool containsBmsFile(const std::filesystem::path &root);
bool isHexStringOfLength(const std::string &value, size_t length);
std::optional<std::filesystem::path> findMatchingBmsChartByHash(
    const std::filesystem::path &root, const std::string &expectedHash,
    std::string &errorMessage);
std::optional<std::string>
htmlBodyFromDownloadedFile(const std::filesystem::path &path);

std::optional<std::string> fetchUrlText(const std::string &url,
                                        std::string &errorMessage);
std::optional<std::string> postUrlText(const std::string &url,
                                       std::string &errorMessage);
bool downloadUrlToFile(const std::string &url, const std::filesystem::path &path,
                       std::atomic_bool &cancelled, std::string &errorMessage,
                       BmsSearchDownloadProgressCallback progressCallback);
std::filesystem::path makeDownloadDirectory(
    const std::filesystem::path &libraryRoot);
bool downloadAndExtractArchive(
    const std::string &downloadUrl, const std::string &displayUrl,
    const std::string &archiveKey, const std::filesystem::path &libraryRoot,
    std::atomic_bool &cancelled,
    BmsSearchDownloadProgressCallback progressCallback,
    BmsSearchResult &result, const std::string &suggestedArchiveName = "",
    bool *downloadedArchive = nullptr);
DownloadCandidate packageDownloadCandidate(const std::string &downloadUrl,
                                           const std::string &archiveName,
                                           const std::string &md5);

} // namespace asobmshow::bms_search
