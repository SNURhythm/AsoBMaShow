#include "DifficultyTableImporter.h"

#include "BmsMetadataText.h"
#include "path.h"
#include "targets.h"

#include <SDL2/SDL.h>

#include "../yoga/lib/nlohmann/json.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <future>
#include <mutex>
#include <regex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
#include "iOSNatives.hpp"
#elif TARGET_OS_ANDROID
#include "AndroidNatives.h"
#include "CurlRAII.h"
#else
#include "CurlRAII.h"
#endif

namespace {
using json = nlohmann::json;
using asobmshow::bms_metadata::normalizedHash;
using asobmshow::bms_metadata::trimCopy;

constexpr std::size_t kMaxConcurrentDifficultyTableDownloads = 4;

std::string jsonValueToString(const json &value,
                              const std::string &fallback = "") {
  if (value.is_string()) {
    return value.get<std::string>();
  }
  if (value.is_number_integer()) {
    return std::to_string(value.get<long long>());
  }
  if (value.is_number_unsigned()) {
    return std::to_string(value.get<unsigned long long>());
  }
  if (value.is_number_float()) {
    std::ostringstream stream;
    stream << value.get<double>();
    return stream.str();
  }
  if (value.is_boolean()) {
    return value.get<bool>() ? "true" : "false";
  }
  return fallback;
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
  return jsonValueToString(*it, fallback);
}

std::optional<std::vector<std::string>> jsonStringListAt(
    const json &object, const char *key) {
  if (!object.is_object()) return std::nullopt;
  const auto found = object.find(key);
  if (found == object.end() || !found->is_array()) return std::nullopt;
  std::vector<std::string> values;
  values.reserve(found->size());
  for (const auto &value : *found) {
    if (!value.is_string()) return std::vector<std::string>{};
    values.push_back(value.get<std::string>());
  }
  return values;
}

double jsonNumberAt(const json &object, const char *key) {
  if (!object.is_object()) return 0.0;
  const auto found = object.find(key);
  return found != object.end() && found->is_number()
             ? found->get<double>()
             : 0.0;
}

std::optional<std::string> jsonPresentStringAt(const json &object,
                                               const char *key) {
  if (!object.is_object()) return std::nullopt;
  const auto found = object.find(key);
  if (found == object.end() || found->is_null()) return std::nullopt;
  return jsonValueToString(*found);
}

std::optional<std::string> readTextFile(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return std::nullopt;
  }
  std::ostringstream stream;
  stream << file.rdbuf();
  return stream.str();
}

#if !(TARGET_OS_IOS || TARGET_OS_SIMULATOR)
std::once_flag curlInitFlag;

size_t appendCurlResponse(char *ptr, size_t size, size_t nmemb,
                          void *userdata) {
  const size_t byteCount = size * nmemb;
  auto *response = static_cast<std::string *>(userdata);
  response->append(ptr, byteCount);
  return byteCount;
}

int difficultyTableCurlProgress(void *userdata, curl_off_t, curl_off_t,
                                curl_off_t, curl_off_t) {
  const auto *checkpoint =
      static_cast<const DifficultyTableImportCheckpoint *>(userdata);
  return checkpoint != nullptr && *checkpoint && !(*checkpoint)() ? 1 : 0;
}
#endif

bool difficultyTableCheckpoint(
    const DifficultyTableImportCheckpoint &checkpoint,
    std::string *errorMessage) {
  if (!checkpoint || checkpoint()) return true;
  if (errorMessage != nullptr) {
    *errorMessage = "Difficulty table update was interrupted";
  }
  return false;
}

std::optional<std::string> fetchUrlText(const std::string &url,
                                        std::string *errorMessage,
                                        const DifficultyTableImportCheckpoint &
                                            checkpoint) {
  if (!difficultyTableCheckpoint(checkpoint, errorMessage)) {
    return std::nullopt;
  }
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  std::string body;
  std::string iosError;
  if (!DownloadURLTextIOS(url, body, iosError)) {
    if (errorMessage != nullptr) {
      *errorMessage = iosError.empty() ? "Failed to download " + url : iosError;
    }
    return std::nullopt;
  }
  return difficultyTableCheckpoint(checkpoint, errorMessage)
             ? std::optional<std::string>{std::move(body)}
             : std::nullopt;
#elif TARGET_OS_ANDROID
  std::string body;
  std::string androidError;
  if (!DownloadURLTextAndroid(url, body, androidError)) {
    if (errorMessage != nullptr) {
      *errorMessage =
          androidError.empty() ? "Failed to download " + url : androidError;
    }
    return std::nullopt;
  }
  return difficultyTableCheckpoint(checkpoint, errorMessage)
             ? std::optional<std::string>{std::move(body)}
             : std::nullopt;
#else
  std::call_once(curlInitFlag, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
  CurlEasyHandle curl(curl_easy_init());
  if (curl == nullptr) {
    if (errorMessage != nullptr) {
      *errorMessage = "Failed to initialize HTTP client";
    }
    return std::nullopt;
  }

  std::string body;
  char curlError[CURL_ERROR_SIZE] = {};
  curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl.get(), CURLOPT_MAXREDIRS, 8L);
  curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "AsoBMaShow");
  curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 25L);
  curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, appendCurlResponse);
  curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(curl.get(), CURLOPT_ERRORBUFFER, curlError);
  curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "http,https");
  curl_easy_setopt(curl.get(), CURLOPT_REDIR_PROTOCOLS_STR,
                   CurlRedirectProtocolsForInitialUrl(url));
  if (checkpoint) {
    curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION,
                     difficultyTableCurlProgress);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &checkpoint);
  }
  ConfigureCurlTrustStore(curl.get());

  const CURLcode result = curl_easy_perform(curl.get());
  long statusCode = 0;
  curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &statusCode);
  if (result != CURLE_OK) {
    if (errorMessage != nullptr) {
      *errorMessage =
          curlError[0] != '\0' ? curlError : curl_easy_strerror(result);
    }
    return std::nullopt;
  }
  if (statusCode >= 400) {
    if (errorMessage != nullptr) {
      *errorMessage =
          "HTTP " + std::to_string(statusCode) + " while downloading " + url;
    }
    return std::nullopt;
  }
  return difficultyTableCheckpoint(checkpoint, errorMessage)
             ? std::optional<std::string>{std::move(body)}
             : std::nullopt;
#endif
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

constexpr char asciiLower(char value) noexcept {
  return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A'))
                                      : value;
}

bool startsWithAsciiCaseInsensitive(std::string_view value,
                                    std::string_view prefix) noexcept {
  if (value.size() < prefix.size()) {
    return false;
  }
  for (std::size_t index = 0; index < prefix.size(); ++index) {
    if (asciiLower(value[index]) != asciiLower(prefix[index])) {
      return false;
    }
  }
  return true;
}

std::string resolveUrl(const std::string &baseUrl, const std::string &link) {
  const bool linkUsesHttp =
      startsWithAsciiCaseInsensitive(link, "http://");
  const bool linkUsesHttps =
      startsWithAsciiCaseInsensitive(link, "https://");
  if (linkUsesHttp || linkUsesHttps) {
    if (startsWithAsciiCaseInsensitive(baseUrl, "https://") && linkUsesHttp) {
      return "";
    }
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

  std::string resolved = origin + "/";
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i > 0) {
      resolved += "/";
    }
    resolved += parts[i];
  }
  return resolved;
}

std::string jsonUrlAt(const json &object, const char *key) {
  const std::string value = trimCopy(jsonStringAt(object, key));
  if (startsWithAsciiCaseInsensitive(value, "http://") ||
      startsWithAsciiCaseInsensitive(value, "https://") ||
      value.starts_with("//") || value.starts_with("/")) {
    return value;
  }
  if (value.find('/') != std::string::npos ||
      value.find(".html") != std::string::npos ||
      value.find(".json") != std::string::npos) {
    return value;
  }
  return "";
}

bool looksLikeDifficultyTableListItem(const json &item) {
  if (item.is_string()) {
    const std::string url = trimCopy(item.get<std::string>());
    return startsWithAsciiCaseInsensitive(url, "http://") ||
           startsWithAsciiCaseInsensitive(url, "https://") ||
           url.starts_with("//") || url.starts_with("/");
  }
  if (!item.is_object() || item.contains("md5") || item.contains("sha256")) {
    return false;
  }
  if (jsonUrlAt(item, "url").empty()) {
    return false;
  }
  return item.contains("name") || item.contains("symbol") ||
         item.contains("tag1") || item.contains("tag2") ||
         item.contains("comment");
}

struct DifficultyTableListEntry {
  std::string name;
  std::string url;
};

std::string difficultyTableListItemName(const json &item,
                                        const std::string &fallbackUrl) {
  if (!item.is_object()) {
    return fallbackUrl;
  }
  for (const auto *key : {"name", "symbol", "comment", "url"}) {
    const std::string value = trimCopy(jsonStringAt(item, key));
    if (!value.empty()) {
      return value;
    }
  }
  return fallbackUrl;
}

std::vector<DifficultyTableListEntry>
readDifficultyTableListEntries(const json &document, const std::string &url) {
  const json *items = nullptr;
  if (document.is_array()) {
    items = &document;
  } else if (document.is_object()) {
    for (const auto *key :
         {"tables", "tablelist", "table_list", "list", "data"}) {
      const auto it = document.find(key);
      if (it != document.end() && it->is_array()) {
        items = &(*it);
        break;
      }
    }
  }
  if (items == nullptr || !items->is_array()) {
    return {};
  }

  std::vector<DifficultyTableListEntry> entries;
  std::unordered_set<std::string> seen;
  for (const auto &item : *items) {
    if (!looksLikeDifficultyTableListItem(item)) {
      continue;
    }
    std::string tableUrl = item.is_string() ? trimCopy(item.get<std::string>())
                                            : jsonUrlAt(item, "url");
    if (tableUrl.empty()) {
      continue;
    }
    const std::string resolvedUrl = resolveUrl(url, tableUrl);
    if (resolvedUrl.empty()) {
      continue;
    }
    if (seen.insert(resolvedUrl).second) {
      entries.push_back(
          {difficultyTableListItemName(item, resolvedUrl), resolvedUrl});
    }
  }
  return entries;
}

std::optional<std::string> findBmstableHeaderUrl(const std::string &html,
                                                 const std::string &pageUrl) {
  const std::regex metaPattern("<meta\\b[^>]*>", std::regex::icase);
  const std::regex namePattern("\\bname\\s*=\\s*(['\"])bmstable\\1",
                               std::regex::icase);
  const std::regex contentPattern("\\bcontent\\s*=\\s*(['\"])([^'\"]+)\\1",
                                  std::regex::icase);
  const auto begin =
      std::sregex_iterator(html.begin(), html.end(), metaPattern);
  const auto end = std::sregex_iterator();
  for (auto it = begin; it != end; ++it) {
    const std::string tag = it->str();
    if (!std::regex_search(tag, namePattern)) {
      continue;
    }
    std::smatch match;
    if (std::regex_search(tag, match, contentPattern) && match.size() >= 3) {
      const std::string resolved = resolveUrl(pageUrl, match[2].str());
      return resolved.empty() ? std::nullopt
                              : std::optional<std::string>(resolved);
    }
  }
  return std::nullopt;
}

difficulty_table::Chart readChartItem(const json &item,
                                      const std::string &defaultLevel) {
  difficulty_table::Chart chart;
  chart.level = jsonStringAt(item, "level", defaultLevel);
  if (chart.level.empty()) {
    chart.level = defaultLevel;
  }
  chart.md5 = normalizedHash(jsonStringAt(item, "md5"));
  chart.sha256 = normalizedHash(jsonStringAt(item, "sha256"));
  chart.title = jsonStringAt(item, "title");
  chart.subtitle = jsonStringAt(item, "subtitle");
  chart.artist = jsonStringAt(item, "artist");
  chart.subartist = jsonStringAt(item, "subartist");
  chart.url = jsonStringAt(item, "url");
  chart.urlDiff = jsonStringAt(item, "url_diff");
  chart.originalMd5s = jsonStringListAt(item, "org_md5");
  return chart;
}

course_identity::ChartIdentity
chartIdentity(const difficulty_table::Chart &chart) {
  return {.sha256 = chart.sha256, .md5 = chart.md5};
}

struct UnambiguousHashEvidence {
  std::string value;
  bool conflicting = false;

  void observe(const std::string &candidate) {
    const std::string normalized = normalizedHash(candidate);
    if (normalized.empty()) {
      return;
    }
    if (value.empty()) {
      value = normalized;
    } else if (value != normalized) {
      conflicting = true;
    }
  }

  [[nodiscard]] std::string resolved() const {
    return conflicting ? std::string() : value;
  }
};

struct ChartHashEvidence {
  UnambiguousHashEvidence sha256;
  UnambiguousHashEvidence md5;

  void
  observeMatchingCandidate(const course_identity::ChartIdentity &stored,
                           const course_identity::ChartIdentity &candidate) {
    const course_identity::ChartIdentity normalizedStored{
        .sha256 = normalizedHash(stored.sha256),
        .md5 = normalizedHash(stored.md5),
    };
    const course_identity::ChartIdentity normalizedCandidate{
        .sha256 = normalizedHash(candidate.sha256),
        .md5 = normalizedHash(candidate.md5),
    };
    if (normalizedStored.sha256.empty() && !normalizedStored.md5.empty() &&
        normalizedCandidate.md5 == normalizedStored.md5) {
      sha256.observe(normalizedCandidate.sha256);
    }
    if (normalizedStored.md5.empty() && !normalizedStored.sha256.empty() &&
        normalizedCandidate.sha256 == normalizedStored.sha256) {
      md5.observe(normalizedCandidate.md5);
    }
  }

  void enrichMissing(course_identity::ChartIdentity &stored) const {
    if (stored.sha256.empty()) {
      stored.sha256 = sha256.resolved();
    }
    if (stored.md5.empty()) {
      stored.md5 = md5.resolved();
    }
  }
};

using ChartLookup = std::unordered_map<std::string, difficulty_table::Chart>;
using ChartHashEvidenceLookup =
    std::unordered_map<std::string, ChartHashEvidence>;

std::string chartLookupKey(const std::string &kind, const std::string &hash) {
  return hash.empty() ? "" : kind + ":" + hash;
}

std::string
missingCounterpartEvidenceKey(const course_identity::ChartIdentity &stored) {
  const std::string sha256 = normalizedHash(stored.sha256);
  const std::string md5 = normalizedHash(stored.md5);
  if (sha256.empty() && !md5.empty()) {
    return chartLookupKey("md5", md5);
  }
  if (md5.empty() && !sha256.empty()) {
    return chartLookupKey("sha256", sha256);
  }
  return {};
}

void addToChartLookup(ChartLookup &lookup,
                      const difficulty_table::Chart &chart) {
  const std::string sha256Key = chartLookupKey("sha256", chart.sha256);
  if (!sha256Key.empty()) {
    lookup.emplace(sha256Key, chart);
  }
  const std::string md5Key = chartLookupKey("md5", chart.md5);
  if (!md5Key.empty()) {
    lookup.emplace(md5Key, chart);
  }
}

void addToChartHashEvidenceLookup(ChartHashEvidenceLookup &lookup,
                                  const difficulty_table::Chart &chart) {
  const course_identity::ChartIdentity candidate = chartIdentity(chart);
  if (!chart.md5.empty()) {
    lookup[chartLookupKey("md5", chart.md5)].observeMatchingCandidate(
        course_identity::ChartIdentity{.md5 = chart.md5}, candidate);
  }
  if (!chart.sha256.empty()) {
    lookup[chartLookupKey("sha256", chart.sha256)].observeMatchingCandidate(
        course_identity::ChartIdentity{.sha256 = chart.sha256}, candidate);
  }
}

const difficulty_table::Chart *
findChartInLookup(const ChartLookup &lookup,
                  const difficulty_table::Chart &chart) {
  const std::string sha256Key = chartLookupKey("sha256", chart.sha256);
  if (!sha256Key.empty()) {
    const auto it = lookup.find(sha256Key);
    if (it != lookup.end()) {
      return &it->second;
    }
  }
  const std::string md5Key = chartLookupKey("md5", chart.md5);
  if (!md5Key.empty()) {
    const auto it = lookup.find(md5Key);
    if (it != lookup.end()) {
      return &it->second;
    }
  }
  return nullptr;
}

void fillUnambiguousCourseChartHash(
    difficulty_table::Chart &courseChart,
    const ChartHashEvidenceLookup &evidenceByHash) {
  const std::string lookupKey =
      missingCounterpartEvidenceKey(chartIdentity(courseChart));
  if (lookupKey.empty()) {
    return;
  }
  const auto evidence = evidenceByHash.find(lookupKey);
  if (evidence == evidenceByHash.end()) {
    return;
  }
  course_identity::ChartIdentity identity = chartIdentity(courseChart);
  evidence->second.enrichMissing(identity);
  if (courseChart.sha256.empty()) {
    courseChart.sha256 = std::move(identity.sha256);
  }
  if (courseChart.md5.empty()) {
    courseChart.md5 = std::move(identity.md5);
  }
}

void fillMissingCourseChartMetadata(difficulty_table::Chart &courseChart,
                                    const difficulty_table::Chart &tableChart) {
  if ((courseChart.level.empty() || courseChart.level == "0") &&
      !tableChart.level.empty()) {
    courseChart.level = tableChart.level;
  }
  if (courseChart.title.empty()) {
    courseChart.title = tableChart.title;
  }
  if (courseChart.subtitle.empty()) {
    courseChart.subtitle = tableChart.subtitle;
  }
  if (courseChart.artist.empty()) {
    courseChart.artist = tableChart.artist;
  }
  if (courseChart.subartist.empty()) {
    courseChart.subartist = tableChart.subartist;
  }
  if (courseChart.url.empty()) {
    courseChart.url = tableChart.url;
  }
  if (courseChart.urlDiff.empty()) {
    courseChart.urlDiff = tableChart.urlDiff;
  }
}

std::vector<std::string> readLevelOrder(const json &header) {
  std::vector<std::string> levels;
  if (!header.is_object()) {
    return levels;
  }
  const auto it = header.find("level_order");
  if (it == header.end() || !it->is_array()) {
    return levels;
  }

  std::unordered_set<std::string> seen;
  for (const auto &levelValue : *it) {
    const std::string level = trimCopy(jsonValueToString(levelValue));
    if (!level.empty() && seen.insert(level).second) {
      levels.push_back(level);
    }
  }
  return levels;
}

std::vector<difficulty_table::Chart> readCourseCharts(const json &course) {
  std::vector<difficulty_table::Chart> charts;
  if (!course.is_object()) {
    return charts;
  }
  const auto chartIt = course.find("charts");
  if (chartIt != course.end() && chartIt->is_array()) {
    for (const auto &chartValue : *chartIt) {
      if (chartValue.is_object()) {
        charts.push_back(readChartItem(chartValue, ""));
      }
    }
  }
  const auto md5It = course.find("md5");
  if (md5It != course.end() && md5It->is_array()) {
    for (const auto &md5Value : *md5It) {
      const std::string md5 = normalizedHash(jsonValueToString(md5Value));
      if (!md5.empty()) {
        charts.push_back({.md5 = md5});
      }
    }
  }
  const auto sha256It = course.find("sha256");
  if (sha256It != course.end() && sha256It->is_array()) {
    for (const auto &sha256Value : *sha256It) {
      const std::string sha256 = normalizedHash(jsonValueToString(sha256Value));
      if (!sha256.empty()) {
        charts.push_back({.sha256 = sha256});
      }
    }
  }
  return charts;
}

void collectCourses(const json &value, std::vector<const json *> &courses) {
  if (value.is_object() && value.contains("name")) {
    courses.push_back(&value);
    return;
  }
  if (!value.is_array()) {
    return;
  }
  for (const auto &child : value) {
    collectCourses(child, courses);
  }
}

std::pair<std::string, std::string>
splitCourseFolderAndLevel(const std::string &courseName,
                          const std::string &symbol) {
  if (!symbol.empty()) {
    const std::string needle = " " + symbol;
    const auto pos = courseName.rfind(needle);
    if (pos != std::string::npos && pos + 1 < courseName.size()) {
      return {trimCopy(courseName.substr(0, pos)),
              trimCopy(courseName.substr(pos + 1))};
    }
  }
  return {"", courseName};
}

struct ResolvedDifficultyHeader {
  std::string headerUrl;
  std::string headerJson;
  std::string tableName;
  std::string dataUrl;
};

std::optional<ResolvedDifficultyHeader>
resolveDifficultyHeader(const std::string &tableUrl,
                        const std::string &pageBody,
                        const DifficultyTableControlledTextFetcher &fetchText,
                        bool rejectNestedList, std::string &errorMessage,
                        const DifficultyTableImportCheckpoint &checkpoint) {
  ResolvedDifficultyHeader resolved;
  try {
    const json maybeHeader = json::parse(pageBody);
    if (rejectNestedList &&
        !readDifficultyTableListEntries(maybeHeader, tableUrl).empty()) {
      errorMessage = "Nested table lists are not supported";
      return std::nullopt;
    }
    if (maybeHeader.is_object() && maybeHeader.contains("name") &&
        maybeHeader.contains("symbol") && maybeHeader.contains("data_url")) {
      resolved.headerUrl = tableUrl;
      resolved.headerJson = pageBody;
    }
  } catch (...) {
  }

  if (resolved.headerJson.empty()) {
    const auto headerUrl = findBmstableHeaderUrl(pageBody, tableUrl);
    if (!headerUrl.has_value()) {
      errorMessage = "Could not find a bmstable header link";
      return std::nullopt;
    }
    resolved.headerUrl = *headerUrl;
    std::string headerError;
    const auto headerBody =
        fetchText(resolved.headerUrl, &headerError, checkpoint);
    if (!headerBody.has_value()) {
      errorMessage =
          headerError.empty() ? "Failed to download table header" : headerError;
      return std::nullopt;
    }
    resolved.headerJson = *headerBody;
  }

  try {
    const json header = json::parse(resolved.headerJson);
    resolved.dataUrl = jsonStringAt(header, "data_url");
    resolved.tableName = jsonStringAt(header, "name", tableUrl);
  } catch (const std::exception &error) {
    errorMessage =
        std::string("Failed to parse table header JSON: ") + error.what();
    return std::nullopt;
  }
  if (resolved.dataUrl.empty()) {
    errorMessage = "Table header does not contain data_url";
    return std::nullopt;
  }
  return resolved;
}

struct DifficultyTableDownloadResult {
  std::optional<difficulty_table::Document> document;
  std::string sourceUrl;
  std::string tableName;
  std::string errorMessage;
};

DifficultyTableDownloadResult
downloadDifficultyTable(const std::string &tableUrl,
                        const DifficultyTableControlledTextFetcher &fetchText,
                        const DifficultyTableImportCheckpoint &checkpoint) {
  DifficultyTableDownloadResult result;
  result.sourceUrl = tableUrl;
  result.tableName = tableUrl;

  std::string pageError;
  const auto pageBody = fetchText(tableUrl, &pageError, checkpoint);
  if (!pageBody.has_value()) {
    result.errorMessage =
        pageError.empty() ? "Failed to download table page" : pageError;
    return result;
  }

  auto resolved = resolveDifficultyHeader(tableUrl, *pageBody, fetchText, true,
                                          result.errorMessage, checkpoint);
  if (!resolved.has_value()) {
    return result;
  }
  result.tableName = resolved->tableName;
  const std::string dataUrl =
      resolveUrl(resolved->headerUrl, resolved->dataUrl);
  if (dataUrl.empty()) {
    result.errorMessage =
        "HTTPS difficulty tables cannot load data over HTTP";
    return result;
  }
  std::string dataError;
  const auto dataBody = fetchText(dataUrl, &dataError, checkpoint);
  if (!dataBody.has_value()) {
    result.errorMessage =
        dataError.empty() ? "Failed to download table data" : dataError;
    return result;
  }

  result.document = difficulty_table::Parse(resolved->headerJson, *dataBody,
                                            tableUrl, result.errorMessage);
  if (!result.document.has_value()) {
    result.errorMessage = "downloaded table data could not be imported";
  }
  return result;
}

} // namespace

std::optional<difficulty_table::Document> difficulty_table::Parse(
    const std::string &headerJson, const std::string &dataJson,
    const std::string &sourceUrl, std::string &errorMessage) {
  errorMessage.clear();
  json header;
  json data;
  try {
    header = json::parse(headerJson);
    data = dataJson.empty() ? json::array() : json::parse(dataJson);
  } catch (const std::exception &error) {
    errorMessage =
        std::string("Failed to parse difficulty table JSON: ") + error.what();
    return std::nullopt;
  }

  Document document{
      .name = jsonStringAt(header, "name"),
      .symbol = jsonStringAt(header, "symbol"),
      .sourceUrl = sourceUrl,
      .dataUrl = jsonStringAt(header, "data_url"),
      .levelOrder = readLevelOrder(header),
  };
  if (document.name.empty() || document.symbol.empty()) {
    errorMessage = "Difficulty table is missing name or symbol";
    return std::nullopt;
  }

  const json *charts = nullptr;
  if (data.is_array()) {
    charts = &data;
  } else if (data.is_object()) {
    const auto chartsIt = data.find("charts");
    if (chartsIt != data.end() && chartsIt->is_array()) {
      charts = &(*chartsIt);
    }
  }
  if (charts != nullptr) {
    for (const auto &chartValue : *charts) {
      if (!chartValue.is_object()) {
        continue;
      }
      auto chart = readChartItem(chartValue, "");
      if (!chart.md5.empty() || !chart.sha256.empty()) {
        document.charts.push_back(std::move(chart));
      }
    }
  }

  if (!document.levelOrder.empty()) {
    std::unordered_map<std::string, int> orderByLevel;
    for (std::size_t i = 0; i < document.levelOrder.size(); ++i) {
      orderByLevel.emplace(document.levelOrder[i], static_cast<int>(i));
    }
    std::stable_sort(
        document.charts.begin(), document.charts.end(),
        [&](const Chart &left, const Chart &right) {
          const auto leftOrder = orderByLevel.find(trimCopy(left.level));
          const auto rightOrder = orderByLevel.find(trimCopy(right.level));
          const int fallback = static_cast<int>(orderByLevel.size());
          return (leftOrder == orderByLevel.end() ? fallback
                                                  : leftOrder->second) <
                 (rightOrder == orderByLevel.end() ? fallback
                                                   : rightOrder->second);
        });
  }

  ChartLookup chartLookup;
  chartLookup.reserve(document.charts.size() * 2);
  ChartHashEvidenceLookup chartHashEvidence;
  chartHashEvidence.reserve(document.charts.size() * 2);
  for (const auto &chart : document.charts) {
    addToChartLookup(chartLookup, chart);
    addToChartHashEvidenceLookup(chartHashEvidence, chart);
  }

  std::vector<const json *> courses;
  const auto courseIt = header.find("course");
  if (courseIt != header.end()) {
    collectCourses(*courseIt, courses);
  }
  for (const auto *courseValue : courses) {
    const std::string courseName = jsonStringAt(*courseValue, "name");
    if (courseName.empty()) {
      continue;
    }
    const auto [groupName, level] =
        splitCourseFolderAndLevel(courseName, document.symbol);
    std::string constraintJson = "[]";
    const auto constraintIt = courseValue->find("constraint");
    if (constraintIt != courseValue->end()) {
      constraintJson = constraintIt->dump();
    }

    Course course{
        .name = courseName,
        .groupName = groupName,
        .level = level,
        .constraintJson = std::move(constraintJson),
    };
    const auto trophyIt = courseValue->find("trophy");
    if (trophyIt != courseValue->end() && trophyIt->is_array()) {
      for (const auto &value : *trophyIt) {
        const auto name = jsonPresentStringAt(value, "name");
        Trophy trophy{.name = name.value_or(""),
                      .missRate = jsonNumberAt(value, "missrate"),
                      .scoreRate = jsonNumberAt(value, "scorerate")};
        // CourseData.TrophyData.validate is the pinned source contract.
        if (name.has_value() && trophy.missRate > 0.0 &&
            trophy.scoreRate < 100.0) {
          course.trophies.push_back(std::move(trophy));
        }
      }
    }
    auto courseCharts = readCourseCharts(*courseValue);
    course.charts.reserve(courseCharts.size());
    for (auto &chart : courseCharts) {
      if (chart.md5.empty() && chart.sha256.empty()) {
        continue;
      }
      if (const auto *tableChart = findChartInLookup(chartLookup, chart)) {
        fillMissingCourseChartMetadata(chart, *tableChart);
      }
      fillUnambiguousCourseChartHash(chart, chartHashEvidence);
      course.charts.push_back(std::move(chart));
    }
    document.courses.push_back(std::move(course));
  }
  return document;
}

DifficultyTableImporter::DifficultyTableImporter() : fetchText_(fetchUrlText) {}

DifficultyTableImporter::DifficultyTableImporter(
    DifficultyTableTextFetcher fetchText)
    : fetchText_([fetchText = std::move(fetchText)](
                     const std::string &url, std::string *errorMessage,
                     const DifficultyTableImportCheckpoint &checkpoint)
                     -> std::optional<std::string> {
        if (!difficultyTableCheckpoint(checkpoint, errorMessage)) {
          return std::nullopt;
        }
        const auto result = fetchText
                                ? fetchText(url, errorMessage)
                                : fetchUrlText(url, errorMessage, checkpoint);
        return result && difficultyTableCheckpoint(checkpoint, errorMessage)
                   ? result
                   : std::nullopt;
      }) {}

bool DifficultyTableImporter::ImportFromUrl(
    ChartRepository::Session &session, const std::string &pageUrl,
    std::string *errorMessage,
    DifficultyTableImportProgressCallback progressCallback,
    DifficultyTableImportCheckpoint checkpoint) {
  if (!difficultyTableCheckpoint(checkpoint, errorMessage)) {
    return false;
  }
  const std::string trimmedUrl = trimCopy(pageUrl);
  if (trimmedUrl.empty()) {
    if (errorMessage != nullptr) {
      *errorMessage = "Table URL is empty";
    }
    return false;
  }
  if (!startsWithAsciiCaseInsensitive(trimmedUrl, "http://") &&
      !startsWithAsciiCaseInsensitive(trimmedUrl, "https://")) {
    if (errorMessage != nullptr) {
      *errorMessage = "Table URL must start with http:// or https://";
    }
    return false;
  }

  const auto pageBody = fetchText_(trimmedUrl, errorMessage, checkpoint);
  if (!pageBody.has_value()) {
    return false;
  }

  try {
    const json maybeList = json::parse(*pageBody);
    const auto tableEntries =
        readDifficultyTableListEntries(maybeList, trimmedUrl);
    if (!tableEntries.empty()) {
      struct InFlightDownload {
        DifficultyTableListEntry entry;
        std::future<DifficultyTableDownloadResult> future;
      };

      std::unordered_set<std::string> existingSources;
      for (const auto &table : session.SelectDifficultyTables()) {
        if (!table.sourceUrl.empty()) {
          existingSources.insert(table.sourceUrl);
        }
      }

      int imported = 0;
      int failed = 0;
      int skipped = 0;
      int completed = 0;
      std::string firstError;
      std::size_t nextIndex = 0;
      std::vector<InFlightDownload> inFlight;
      inFlight.reserve(kMaxConcurrentDifficultyTableDownloads);
      const int total = static_cast<int>(tableEntries.size());
      if (progressCallback) {
        progressCallback({0, total, "Preparing table downloads"});
      }

      const auto markSkipped = [&](const DifficultyTableListEntry &table,
                                   const std::string &reason) {
        ++skipped;
        ++completed;
        if (firstError.empty() && !reason.empty()) {
          firstError = reason;
        }
        if (progressCallback) {
          progressCallback(
              {completed, total,
               "Skipped: " + (table.name.empty() ? table.url : table.name)});
        }
      };

      while (nextIndex < tableEntries.size() || !inFlight.empty()) {
        if (!difficultyTableCheckpoint(checkpoint, errorMessage)) {
          return false;
        }
        while (nextIndex < tableEntries.size() &&
               inFlight.size() < kMaxConcurrentDifficultyTableDownloads) {
          const auto &table = tableEntries[nextIndex];
          if (table.url == trimmedUrl) {
            markSkipped(table, "Skipped recursive table list URL");
            ++nextIndex;
            continue;
          }
          if (existingSources.contains(table.url)) {
            markSkipped(table, "");
            ++nextIndex;
            continue;
          }
          if (progressCallback) {
            progressCallback(
                {completed, total,
                 "Fetching: " + (table.name.empty() ? table.url : table.name)});
          }
          const DifficultyTableControlledTextFetcher fetchText = fetchText_;
          inFlight.push_back(
              {table, std::async(std::launch::async, [tableUrl = table.url,
                                                      fetchText,
                                                      checkpoint]() {
                 return downloadDifficultyTable(tableUrl, fetchText,
                                                checkpoint);
               })});
          ++nextIndex;
        }

        bool drainedResult = false;
        for (auto it = inFlight.begin(); it != inFlight.end(); ++it) {
          if (it->future.wait_for(std::chrono::milliseconds(0)) !=
              std::future_status::ready) {
            continue;
          }
          DifficultyTableDownloadResult result = it->future.get();
          const std::string displayName =
              result.tableName.empty()
                  ? (it->entry.name.empty() ? it->entry.url : it->entry.name)
                  : result.tableName;
          if (result.document.has_value()) {
            if (progressCallback) {
              progressCallback({completed, total, "Importing: " + displayName});
            }
            if (!difficultyTableCheckpoint(checkpoint, errorMessage)) {
              return false;
            }
            if (session.ReplaceDifficultyTable(*result.document)) {
              ++imported;
              existingSources.insert(result.document->sourceUrl);
            } else {
              ++failed;
              if (firstError.empty()) {
                firstError = result.sourceUrl +
                             ": downloaded table data could not be imported";
              }
            }
          } else {
            ++failed;
            if (firstError.empty()) {
              firstError = it->entry.url + (result.errorMessage.empty()
                                                ? ""
                                                : ": " + result.errorMessage);
            }
          }
          ++completed;
          if (progressCallback) {
            progressCallback({completed, total, displayName});
          }
          inFlight.erase(it);
          drainedResult = true;
          break;
        }
        if (!drainedResult && !inFlight.empty()) {
          inFlight.front().future.wait_for(std::chrono::milliseconds(50));
        }
      }

      if (imported > 0 || skipped > 0) {
        if (errorMessage != nullptr) {
          *errorMessage = "Imported " + std::to_string(imported) +
                          ", skipped " + std::to_string(skipped) + " of " +
                          std::to_string(tableEntries.size()) + " tables.";
          if (failed > 0) {
            *errorMessage += " Failed " + std::to_string(failed) + ".";
          }
        }
        return true;
      }
      if (errorMessage != nullptr) {
        *errorMessage = firstError.empty()
                            ? "Table list did not contain importable tables"
                            : "Failed to import table list: " + firstError;
      }
      return false;
    }
  } catch (...) {
  }

  std::string headerError;
  auto resolved = resolveDifficultyHeader(trimmedUrl, *pageBody, fetchText_,
                                          false, headerError, checkpoint);
  if (!resolved.has_value()) {
    if (errorMessage != nullptr) {
      *errorMessage = headerError == "Could not find a bmstable header link"
                          ? "Could not find a bmstable header link in the "
                            "table webpage"
                          : headerError;
    }
    return false;
  }
  if (progressCallback) {
    progressCallback(
        {1, 1, resolved->tableName.empty() ? trimmedUrl : resolved->tableName});
  }

  const std::string dataUrl =
      resolveUrl(resolved->headerUrl, resolved->dataUrl);
  if (dataUrl.empty()) {
    if (errorMessage != nullptr) {
      *errorMessage = "HTTPS difficulty tables cannot load data over HTTP";
    }
    return false;
  }
  const auto dataBody = fetchText_(dataUrl, errorMessage, checkpoint);
  if (!dataBody.has_value()) {
    return false;
  }
  if (!difficultyTableCheckpoint(checkpoint, errorMessage)) {
    return false;
  }
  std::string parseError;
  auto document = difficulty_table::Parse(resolved->headerJson, *dataBody,
                                          trimmedUrl, parseError);
  if (!difficultyTableCheckpoint(checkpoint, errorMessage)) {
    return false;
  }
  if (!document.has_value() || !session.ReplaceDifficultyTable(*document)) {
    if (errorMessage != nullptr) {
      *errorMessage = "Downloaded table data could not be imported";
    }
    return false;
  }
  return true;
}

bool DifficultyTableImporter::UpdateFromSourceUrl(
    ChartRepository::Session &session, int tableId, std::string *errorMessage,
    DifficultyTableImportCheckpoint checkpoint) {
  if (!difficultyTableCheckpoint(checkpoint, errorMessage)) {
    return false;
  }
  const auto tables = session.SelectDifficultyTables();
  const auto table =
      std::find_if(tables.begin(), tables.end(), [tableId](const auto &value) {
        return value.id == tableId;
      });
  if (table == tables.end()) {
    if (errorMessage != nullptr) {
      *errorMessage = "Difficulty table does not have an updateable source URL";
    }
    return false;
  }
  const std::string sourceUrl = trimCopy(table->sourceUrl);
  if (!startsWithAsciiCaseInsensitive(sourceUrl, "http://") &&
      !startsWithAsciiCaseInsensitive(sourceUrl, "https://")) {
    if (errorMessage != nullptr) {
      *errorMessage = "Difficulty table does not have an updateable source URL";
    }
    return false;
  }
  return ImportFromUrl(session, sourceUrl, errorMessage, nullptr,
                       std::move(checkpoint));
}

int DifficultyTableImporter::ImportFromDirectory(
    ChartRepository::Session &session, const std::filesystem::path &directory) {
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) {
    SDL_Log("Failed to create difficulty table directory %s: %s",
            fspath_to_utf8(directory).c_str(), error.message().c_str());
    return 0;
  }

  int imported = 0;
  std::unordered_set<std::string> importedHeaders;
  for (std::filesystem::recursive_directory_iterator it(directory, error), end;
       !error && it != end; it.increment(error)) {
    std::error_code typeError;
    if (!it->is_regular_file(typeError)) {
      if (typeError) {
        SDL_Log("Failed to read difficulty table path type %s: %s",
                fspath_to_utf8(it->path()).c_str(),
                typeError.message().c_str());
      }
      continue;
    }
    const auto &path = it->path();
    if (path.extension() != ".json") {
      continue;
    }
    const auto raw = readTextFile(path);
    if (!raw.has_value()) {
      continue;
    }

    json parsedJson;
    try {
      parsedJson = json::parse(*raw);
    } catch (...) {
      continue;
    }
    const std::string sourceUrl = fspath_to_utf8(path);
    if (parsedJson.is_object() && parsedJson.contains("header") &&
        (parsedJson.contains("data") || parsedJson.contains("charts"))) {
      const json header = parsedJson["header"];
      const json data = parsedJson.contains("data")
                            ? parsedJson["data"]
                            : json{{"charts", parsedJson["charts"]}};
      std::string parseError;
      const auto document = difficulty_table::Parse(header.dump(), data.dump(),
                                                    sourceUrl, parseError);
      if (document.has_value() && session.ReplaceDifficultyTable(*document)) {
        ++imported;
      }
      continue;
    }

    if (!parsedJson.is_object() || !parsedJson.contains("name") ||
        !parsedJson.contains("symbol") || !parsedJson.contains("data_url")) {
      continue;
    }
    std::error_code canonicalError;
    const auto canonicalPath =
        std::filesystem::weakly_canonical(path, canonicalError);
    const std::filesystem::path headerPath =
        canonicalError ? path.lexically_normal() : canonicalPath;
    const std::string headerKey = fspath_to_utf8(headerPath);
    if (canonicalError) {
      SDL_Log("Failed to canonicalize difficulty table header %s: %s",
              fspath_to_utf8(path).c_str(), canonicalError.message().c_str());
    }
    if (!importedHeaders.insert(headerKey).second) {
      continue;
    }

    const std::string dataUrl = jsonStringAt(parsedJson, "data_url");
    const std::filesystem::path dataPath = path.parent_path() / dataUrl;
    std::error_code dataPathError;
    if (!std::filesystem::exists(dataPath, dataPathError) || dataPathError) {
      continue;
    }
    const auto dataRaw = readTextFile(dataPath);
    if (!dataRaw.has_value()) {
      continue;
    }
    std::string parseError;
    const auto document =
        difficulty_table::Parse(*raw, *dataRaw, sourceUrl, parseError);
    if (document.has_value() && session.ReplaceDifficultyTable(*document)) {
      ++imported;
    }
  }
  if (error) {
    SDL_Log("Failed while scanning difficulty table directory %s: %s",
            fspath_to_utf8(directory).c_str(), error.message().c_str());
  }
  return imported;
}
