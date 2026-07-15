// Fill out your copyright notice in the Description page of Project Settings.

#include "ChartRepository.h"
#include "ChartRepositoryInternal.h"
#include "../BmsMetadataText.h"
#include "ChartMetaSql.h"
#include "ChartSqlExpressions.h"
#include "ChartStorageIdentity.h"
#include "../LongNoteModeUtils.h"
#include "ScoreRepository.h"
#include "ScoreCacheQueries.h"
#include "SqliteRAII.h"
#include "../Utils.h"
#include <SDL2/SDL.h>
#include "../path.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <fstream>
#include <iostream>
#include "../../yoga/lib/nlohmann/json.hpp"
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include "../targets.h"
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
#include "../iOSNatives.hpp"
#elif TARGET_OS_ANDROID
#include "../AndroidNatives.h"
#include "../CurlRAII.h"
#else
#include "../CurlRAII.h"
#endif

namespace {
using json = nlohmann::json;
using asobmshow::chart_sql::chartArtworkOrderBy;
using asobmshow::chart_sql::chartSourceOrderBy;
using asobmshow::chart_sql::defaultChartMetaBeforeTargetPredicate;
using asobmshow::chart_sql::kChartMetaColumnCount;
using asobmshow::chart_sql::kChartMetaSelectColumns;
using asobmshow::chart_sql::matchedChartPathSubquery;
using asobmshow::chart_sql::normalizedSqlHash;
using asobmshow::chart_sql::preferredChartPredicate;
using asobmshow::chart_sql::sqlTextHasValue;

constexpr int kNoPlayClearMarkRank = -1;

constexpr const char *kDifficultyEntrySelectColumns =
    "COALESCE(cm.path, ''),"
    "COALESCE(NULLIF(dte.md5, ''), cm.md5, ''),"
    "COALESCE(NULLIF(dte.sha256, ''), cm.sha256, ''),"
    "COALESCE(NULLIF(cm.title, ''), NULLIF(dte.title, ''), "
    "NULLIF(dte.sha256, ''), NULLIF(dte.md5, ''), 'Untitled'),"
    "COALESCE(NULLIF(cm.subtitle, ''), dte.subtitle, ''),"
    "COALESCE(cm.genre, ''),"
    "COALESCE(NULLIF(cm.artist, ''), dte.artist, ''),"
    "COALESCE(NULLIF(cm.sub_artist, ''), dte.subartist, ''),"
    "COALESCE(cm.folder, ''),"
    "COALESCE(cm.stage_file, ''),"
    "COALESCE(cm.banner, ''),"
    "COALESCE(cm.back_bmp, ''),"
    "COALESCE(cm.preview, ''),"
    "COALESCE(cm.level, 0),"
    "COALESCE(cm.difficulty, 0),"
    "COALESCE(cm.total, 100),"
    "COALESCE(cm.has_total, 0),"
    "COALESCE(cm.bpm, 0),"
    "COALESCE(cm.max_bpm, 0),"
    "COALESCE(cm.min_bpm, 0),"
    "COALESCE(cm.length, 0),"
    "COALESCE(cm.rank, 3),"
    "COALESCE(cm.player, 1),"
    "COALESCE(cm.keys, 5),"
    "COALESCE(cm.total_notes, 0),"
    "COALESCE(cm.total_long_notes, 0),"
    "COALESCE(cm.total_scratch_notes, 0),"
    "COALESCE(cm.total_backspin_notes, 0),"
    "COALESCE(cm.ln_mode, 0),"
    "dt.symbol || dte.level,"
    "CASE WHEN cm.path IS NULL THEN 1 ELSE 0 END";

constexpr const char *kDifficultyEntrySearchText =
    "rtrim(COALESCE(NULLIF(cm.title, ''), dte.title, '') || ' ' || "
    "COALESCE(NULLIF(cm.subtitle, ''), dte.subtitle, '') || ' ' || "
    "COALESCE(NULLIF(cm.artist, ''), dte.artist, '') || ' ' || "
    "COALESCE(NULLIF(cm.sub_artist, ''), dte.subartist, '') || ' ' || "
    "COALESCE(cm.genre, ''))";
constexpr const char *kDifficultyCourseEntrySelectColumns =
    "COALESCE(cm.path, ''),"
    "COALESCE(NULLIF(dce.md5, ''), NULLIF(dte.md5, ''), cm.md5, ''),"
    "COALESCE(NULLIF(dce.sha256, ''), NULLIF(dte.sha256, ''), cm.sha256, ''),"
    "COALESCE(NULLIF(cm.title, ''), NULLIF(dce.title, ''), "
    "NULLIF(dte.title, ''), NULLIF(dce.sha256, ''), NULLIF(dce.md5, ''), "
    "'Course chart ' || (dce.sort_order + 1)),"
    "COALESCE(NULLIF(cm.subtitle, ''), NULLIF(dce.subtitle, ''), "
    "dte.subtitle, ''),"
    "COALESCE(cm.genre, ''),"
    "COALESCE(NULLIF(cm.artist, ''), NULLIF(dce.artist, ''), dte.artist, ''),"
    "COALESCE(NULLIF(cm.sub_artist, ''), NULLIF(dce.subartist, ''), "
    "dte.subartist, ''),"
    "COALESCE(cm.folder, ''),"
    "COALESCE(cm.stage_file, ''),"
    "COALESCE(cm.banner, ''),"
    "COALESCE(cm.back_bmp, ''),"
    "COALESCE(cm.preview, ''),"
    "CASE WHEN cm.path IS NOT NULL THEN COALESCE(cm.level, 0) "
    "ELSE COALESCE(CAST(NULLIF(NULLIF(dce.level, ''), '0') AS REAL), "
    "CAST(NULLIF(dte.level, '') AS REAL), "
    "CAST(NULLIF(dce.level, '') AS REAL), 0) END,"
    "COALESCE(cm.difficulty, 0),"
    "COALESCE(cm.total, 100),"
    "COALESCE(cm.has_total, 0),"
    "COALESCE(cm.bpm, 0),"
    "COALESCE(cm.max_bpm, 0),"
    "COALESCE(cm.min_bpm, 0),"
    "COALESCE(cm.length, 0),"
    "COALESCE(cm.rank, 3),"
    "COALESCE(cm.player, 1),"
    "COALESCE(cm.keys, 5),"
    "COALESCE(cm.total_notes, 0),"
    "COALESCE(cm.total_long_notes, 0),"
    "COALESCE(cm.total_scratch_notes, 0),"
    "COALESCE(cm.total_backspin_notes, 0),"
    "COALESCE(cm.ln_mode, 0),"
    "COALESCE(NULLIF(dt.symbol || NULLIF(NULLIF(dce.level, ''), '0'), "
    "dt.symbol), NULLIF(dt.symbol || NULLIF(dte.level, ''), dt.symbol), "
    "NULLIF(dt.symbol || NULLIF(dce.level, ''), dt.symbol), "
    "NULLIF(dt.symbol || NULLIF(dc.level, ''), dt.symbol), ''),"
    "CASE WHEN cm.path IS NULL THEN 1 ELSE 0 END";
constexpr const char *kDifficultyCourseEntrySearchText =
    "rtrim(COALESCE(NULLIF(cm.title, ''), NULLIF(dce.title, ''), "
    "dte.title, '') || ' ' || "
    "COALESCE(NULLIF(cm.subtitle, ''), NULLIF(dce.subtitle, ''), "
    "dte.subtitle, '') || ' ' || "
    "COALESCE(NULLIF(cm.artist, ''), NULLIF(dce.artist, ''), "
    "dte.artist, '') || ' ' || "
    "COALESCE(NULLIF(cm.sub_artist, ''), NULLIF(dce.subartist, ''), "
    "dte.subartist, '') || ' ' || "
    "COALESCE(cm.genre, '') || ' ' || dc.name || ' ' || dc.group_name)";
constexpr const char *kSolidArchiveSelectColumns =
    "sa.path, sa.name, sa.archive_size, sa.uncompressed_size, sa.file_count";
constexpr size_t kMaxConcurrentDifficultyTableDownloads = 4;
constexpr int kChartDatabaseSchemaVersion = 3;

using asobmshow::bms_metadata::lowerCopy;
using asobmshow::bms_metadata::normalizedHash;
using asobmshow::bms_metadata::trimCopy;

std::string columnString(sqlite3_stmt *stmt, int idx);

path_t readStoredPath(sqlite3_stmt *stmt, int idx) {
#ifdef _WIN32
  if (sqlite3_column_type(stmt, idx) == SQLITE_NULL) {
    return {};
  }
  const int size = sqlite3_column_bytes(stmt, idx);
  const auto utf8 = std::string(
      reinterpret_cast<const char *>(sqlite3_column_text(stmt, idx)), size);
  return utf8_to_path_t(utf8);
#else
  const auto *text =
      reinterpret_cast<const char *>(sqlite3_column_text(stmt, idx));
  return text != nullptr ? path_t(text) : path_t();
#endif
}

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

std::optional<std::string> readTextFile(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return std::nullopt;
  }
  std::ostringstream stream;
  stream << file.rdbuf();
  return stream.str();
}

bool pathIsInsideDirectory(const std::filesystem::path &path,
                           const std::filesystem::path &directory) {
  if (path.empty() || directory.empty()) {
    return false;
  }

  const std::filesystem::path normalizedPath = path.lexically_normal();
  const std::filesystem::path normalizedDirectory =
      directory.lexically_normal();
  if (normalizedPath == normalizedDirectory) {
    return false;
  }

  const std::filesystem::path relative =
      normalizedPath.lexically_relative(normalizedDirectory);
  if (relative.empty() || relative.is_absolute()) {
    return false;
  }

  const auto first = relative.begin();
  if (first == relative.end()) {
    return false;
  }
  return *first != std::filesystem::path("..") &&
         *first != std::filesystem::path(".");
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
#endif

std::optional<std::string> fetchUrlText(const std::string &url,
                                        std::string *errorMessage) {
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  std::string body;
  std::string iosError;
  if (!DownloadURLTextIOS(url, body, iosError)) {
    if (errorMessage != nullptr) {
      *errorMessage = iosError.empty() ? "Failed to download " + url : iosError;
    }
    return std::nullopt;
  }
  return body;
#elif TARGET_OS_ANDROID
  std::string body;
  std::string androidError;
  if (!DownloadURLTextAndroid(url, body, androidError)) {
    if (errorMessage != nullptr) {
      *errorMessage = androidError.empty() ? "Failed to download " + url
                                           : androidError;
    }
    return std::nullopt;
  }
  return body;
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
  curl_easy_setopt(curl.get(), CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
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
  return body;
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

std::string resolveUrl(const std::string &baseUrl, const std::string &link) {
  if (link.starts_with("http://") || link.starts_with("https://")) {
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
  std::string directory = slash == std::string::npos
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
  for (size_t i = 0; i < parts.size(); i++) {
    if (i > 0) {
      resolved += "/";
    }
    resolved += parts[i];
  }
  return resolved;
}

std::string jsonUrlAt(const json &object, const char *key) {
  const std::string value = trimCopy(jsonStringAt(object, key));
  if (value.starts_with("http://") || value.starts_with("https://") ||
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
    return url.starts_with("http://") || url.starts_with("https://") ||
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
    for (const auto *key : {"tables", "tablelist", "table_list", "list",
                            "data"}) {
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

    std::string tableUrl;
    if (item.is_string()) {
      tableUrl = trimCopy(item.get<std::string>());
    } else {
      tableUrl = jsonUrlAt(item, "url");
    }
    if (tableUrl.empty()) {
      continue;
    }

    const std::string resolvedUrl = resolveUrl(url, tableUrl);
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
  auto begin = std::sregex_iterator(html.begin(), html.end(), metaPattern);
  auto end = std::sregex_iterator();
  for (auto it = begin; it != end; ++it) {
    const std::string tag = it->str();
    if (!std::regex_search(tag, namePattern)) {
      continue;
    }
    std::smatch match;
    if (std::regex_search(tag, match, contentPattern) && match.size() >= 3) {
      return resolveUrl(pageUrl, match[2].str());
    }
  }
  return std::nullopt;
}

struct DifficultyTableDownloadResult {
  bool success = false;
  std::string sourceUrl;
  std::string headerJson;
  std::string dataJson;
  std::string tableName;
  std::string errorMessage;
};

DifficultyTableDownloadResult
downloadDifficultyTablePayloadFromBody(const std::string &tableUrl,
                                       const std::string &pageBody) {
  DifficultyTableDownloadResult result;
  result.sourceUrl = tableUrl;
  result.tableName = tableUrl;

  std::string headerUrl;
  std::string headerJsonText;

  try {
    json maybeHeader = json::parse(pageBody);
    const auto nestedEntries =
        readDifficultyTableListEntries(maybeHeader, tableUrl);
    if (!nestedEntries.empty()) {
      result.errorMessage = "Nested table lists are not supported";
      return result;
    }

    if (maybeHeader.is_object() && maybeHeader.contains("name") &&
        maybeHeader.contains("symbol") && maybeHeader.contains("data_url")) {
      headerUrl = tableUrl;
      headerJsonText = pageBody;
    }
  } catch (...) {
  }

  if (headerJsonText.empty()) {
    auto discoveredHeaderUrl = findBmstableHeaderUrl(pageBody, tableUrl);
    if (!discoveredHeaderUrl) {
      result.errorMessage = "Could not find a bmstable header link";
      return result;
    }

    headerUrl = *discoveredHeaderUrl;
    std::string headerError;
    auto headerBody = fetchUrlText(headerUrl, &headerError);
    if (!headerBody) {
      result.errorMessage =
          headerError.empty() ? "Failed to download table header" : headerError;
      return result;
    }
    headerJsonText = *headerBody;
  }

  json header;
  try {
    header = json::parse(headerJsonText);
  } catch (const std::exception &e) {
    result.errorMessage =
        std::string("Failed to parse table header JSON: ") + e.what();
    return result;
  }

  const std::string dataUrl = jsonStringAt(header, "data_url");
  if (dataUrl.empty()) {
    result.errorMessage = "Table header does not contain data_url";
    return result;
  }

  result.tableName = jsonStringAt(header, "name", tableUrl);
  const std::string resolvedDataUrl = resolveUrl(headerUrl, dataUrl);
  std::string dataError;
  auto dataBody = fetchUrlText(resolvedDataUrl, &dataError);
  if (!dataBody) {
    result.errorMessage =
        dataError.empty() ? "Failed to download table data" : dataError;
    return result;
  }

  result.headerJson = std::move(headerJsonText);
  result.dataJson = std::move(*dataBody);
  result.success = true;
  return result;
}

DifficultyTableDownloadResult
downloadDifficultyTablePayload(const std::string &tableUrl) {
  DifficultyTableDownloadResult result;
  result.sourceUrl = tableUrl;
  result.tableName = tableUrl;

  std::string pageError;
  auto pageBody = fetchUrlText(tableUrl, &pageError);
  if (!pageBody) {
    result.errorMessage =
        pageError.empty() ? "Failed to download table page" : pageError;
    return result;
  }
  return downloadDifficultyTablePayloadFromBody(tableUrl, *pageBody);
}

struct TableChartItem {
  std::string level;
  std::string md5;
  std::string sha256;
  std::string title;
  std::string subtitle;
  std::string artist;
  std::string subartist;
  std::string url;
  std::string urlDiff;
};

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

  void merge(const UnambiguousHashEvidence &other) {
    observe(other.value);
    conflicting = conflicting || other.conflicting;
  }

  [[nodiscard]] std::string resolved() const {
    return conflicting ? std::string() : value;
  }
};

struct ChartHashEvidence {
  UnambiguousHashEvidence sha256;
  UnambiguousHashEvidence md5;

  void observeMatchingCandidate(
      const course_identity::ChartIdentity &stored,
      const course_identity::ChartIdentity &candidate) {
    const course_identity::ChartIdentity normalizedStored{
        .sha256 = normalizedHash(stored.sha256),
        .md5 = normalizedHash(stored.md5),
    };
    const course_identity::ChartIdentity normalizedCandidate{
        .sha256 = normalizedHash(candidate.sha256),
        .md5 = normalizedHash(candidate.md5),
    };
    if (normalizedStored.sha256.empty() &&
        !normalizedStored.md5.empty() &&
        normalizedCandidate.md5 == normalizedStored.md5) {
      sha256.observe(normalizedCandidate.sha256);
    }
    if (normalizedStored.md5.empty() &&
        !normalizedStored.sha256.empty() &&
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

  void merge(const ChartHashEvidence &other) {
    sha256.merge(other.sha256);
    md5.merge(other.md5);
  }
};

course_identity::ChartIdentity chartIdentity(const TableChartItem &chart) {
  return {.sha256 = chart.sha256, .md5 = chart.md5};
}

using RetainedDifficultyCourses =
    std::vector<course_identity::Definition>;

std::vector<course_identity::ChartIdentity>
courseChartIdentities(const std::vector<TableChartItem> &charts) {
  std::vector<course_identity::ChartIdentity> identities;
  identities.reserve(charts.size());
  for (const auto &chart : charts) {
    identities.push_back(chartIdentity(chart));
  }
  return identities;
}

std::optional<course_identity::Definition> takeRetainedDifficultyCourse(
    RetainedDifficultyCourses &retainedCourses,
    const course_identity::Definition &definition) {
  const auto retained = std::find_if(
      retainedCourses.begin(), retainedCourses.end(),
      [&](const course_identity::Definition &candidate) {
        return course_identity::sameDefinition(candidate, definition);
      });
  if (retained == retainedCourses.end()) {
    return std::nullopt;
  }
  course_identity::Definition retainedDefinition = std::move(*retained);
  retainedCourses.erase(retained);
  return retainedDefinition;
}

TableChartItem readChartItem(const json &item,
                             const std::string &defaultLevel) {
  TableChartItem chart;
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
  return chart;
}

using TableChartItemLookup = std::unordered_map<std::string, TableChartItem>;
using ChartHashEvidenceLookup =
    std::unordered_map<std::string, ChartHashEvidence>;

std::string chartLookupKey(const std::string &kind, const std::string &hash) {
  return hash.empty() ? "" : kind + ":" + hash;
}

std::string missingCounterpartEvidenceKey(
    const course_identity::ChartIdentity &stored,
    const std::string &keyPrefix = "") {
  const std::string sha256 = normalizedHash(stored.sha256);
  const std::string md5 = normalizedHash(stored.md5);
  if (sha256.empty() && !md5.empty()) {
    return keyPrefix + chartLookupKey("md5", md5);
  }
  if (md5.empty() && !sha256.empty()) {
    return keyPrefix + chartLookupKey("sha256", sha256);
  }
  return {};
}

void addToChartItemLookup(TableChartItemLookup &lookup,
                          const TableChartItem &chart) {
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
                                  const TableChartItem &chart) {
  const course_identity::ChartIdentity candidate = chartIdentity(chart);
  if (!chart.md5.empty()) {
    lookup[chartLookupKey("md5", chart.md5)].observeMatchingCandidate(
        course_identity::ChartIdentity{.md5 = chart.md5}, candidate);
  }
  if (!chart.sha256.empty()) {
    lookup[chartLookupKey("sha256", chart.sha256)]
        .observeMatchingCandidate(
            course_identity::ChartIdentity{.sha256 = chart.sha256},
            candidate);
  }
}

const TableChartItem *
findChartItemInLookup(const TableChartItemLookup &lookup,
                      const TableChartItem &chart) {
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
    TableChartItem &courseChart,
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

void fillMissingCourseChartMetadata(TableChartItem &courseChart,
                                    const TableChartItem &tableChart) {
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

std::unordered_map<std::string, int> readLevelOrder(const json &header) {
  std::unordered_map<std::string, int> orderByLevel;
  if (!header.is_object()) {
    return orderByLevel;
  }

  const auto it = header.find("level_order");
  if (it == header.end() || !it->is_array()) {
    return orderByLevel;
  }

  int order = 0;
  for (const auto &levelValue : *it) {
    const std::string level = trimCopy(jsonValueToString(levelValue));
    if (level.empty() || orderByLevel.contains(level)) {
      continue;
    }
    orderByLevel[level] = order++;
  }
  return orderByLevel;
}

int levelOrderFor(const std::unordered_map<std::string, int> &orderByLevel,
                  const std::string &level) {
  const auto it = orderByLevel.find(trimCopy(level));
  return it == orderByLevel.end() ? static_cast<int>(orderByLevel.size())
                                  : it->second;
}


std::vector<TableChartItem> readCourseCharts(const json &course) {
  std::vector<TableChartItem> charts;
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

void logSqlErrorText(const char *context, const std::string &error) {
  std::cerr << "SQL error while " << context << ": " << error << "\n";
}

void logSqlError(const char *context, sqlite3 *db) {
  logSqlErrorText(context, sqliteDatabaseError(db));
}

void logSdlSqlErrorText(const char *context, const std::string &error) {
  SDL_Log("SQL error while %s: %s", context, error.c_str());
}

void logSdlSqlError(const char *context, sqlite3 *db) {
  logSdlSqlErrorText(context, sqliteDatabaseError(db));
}

bool execSql(sqlite3 *db, const char *query, const char *context) {
  return executeSqliteLogged(db, query, context, logSqlErrorText);
}

bool execSqlAllowDuplicateColumn(sqlite3 *db, const char *query,
                                 const char *context) {
  return executeSqliteLogged(db, query, context, logSqlErrorText,
                             "duplicate column name");
}

int databaseUserVersion(sqlite3 *db) {
  SqliteStatementHandle stmt;
  const int rc = prepareSqliteStatement(db, "PRAGMA user_version", stmt);
  if (rc != SQLITE_OK) {
    logSqlError("reading chart database version", db);
    return 0;
  }
  if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
    return 0;
  }
  return sqlite3_column_int(stmt.get(), 0);
}

bool setDatabaseUserVersion(sqlite3 *db, int version) {
  const std::string query =
      "PRAGMA user_version = " + std::to_string(std::max(0, version));
  return execSql(db, query.c_str(), "updating chart database version");
}

bool sqliteTableExists(sqlite3 *db, const char *tableName, bool &exists,
                       const char *context) {
  if (const auto error = querySqliteTableExists(db, tableName, exists)) {
    logSqlErrorText(context, *error);
    return false;
  }
  return true;
}

bool createChartMetaTableSchema(sqlite3 *db) {
  const char *query =
      "CREATE TABLE IF NOT EXISTS chart_meta ("
      "path       TEXT primary key,"
      "md5        TEXT not null,"
      "sha256     TEXT not null,"
      "title      TEXT,"
      "subtitle   TEXT,"
      "genre      TEXT,"
      "artist     TEXT,"
      "sub_artist  TEXT,"
      "folder     TEXT,"
      "stage_file  TEXT,"
      "banner     TEXT,"
      "back_bmp    TEXT,"
      "preview    TEXT,"
      "level      REAL,"
      "difficulty INTEGER,"
      "total     REAL,"
      "has_total INTEGER NOT NULL DEFAULT 0,"
      "bpm       REAL,"
      "max_bpm     REAL,"
      "min_bpm     REAL,"
      "length     INTEGER,"
      "rank      INTEGER,"
      "player    INTEGER,"
      "keys     INTEGER,"
      "total_notes INTEGER,"
      "total_long_notes INTEGER,"
      "total_scratch_notes INTEGER,"
      "total_backspin_notes INTEGER,"
      "ln_mode INTEGER NOT NULL DEFAULT 0,"
      "source_priority INTEGER,"
      "source_archive_size INTEGER"
      ")";
  return execSql(db, query, "creating chart meta table");
}

std::optional<std::string> normalizedPathTextForStorage(
    const std::string &original) {
  if (original.empty()) {
    return std::nullopt;
  }

  std::filesystem::path path(utf8_to_path_t(original));
  if (path.empty()) {
    return std::nullopt;
  }
  chart_storage_identity::ToAbsolutePath(path);
  chart_storage_identity::ToRelativePath(path);
  path = path.lexically_normal();

  const std::string normalized = fspath_to_utf8(path);
  if (normalized == original) {
    return std::nullopt;
  }
  return normalized;
}

bool normalizedPathValueExists(sqlite3_stmt *stmt,
                               const std::string &normalized) {
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  bindSqliteText(stmt, 1, normalized);
  const bool exists = sqlite3_step(stmt) == SQLITE_ROW;
  return exists;
}

bool normalizeStoredPathColumn(sqlite3 *db, const char *table,
                               const char *column, bool primaryKey) {
  struct PendingNormalization {
    sqlite3_int64 rowid = 0;
    std::string normalized;
  };

  std::string selectQuery = "SELECT rowid, ";
  selectQuery += column;
  selectQuery += " FROM ";
  selectQuery += table;
  selectQuery += " WHERE ";
  selectQuery += column;
  selectQuery += " IS NOT NULL AND ";
  selectQuery += column;
  selectQuery += " != ''";

  SqliteStatementHandle selectStmt;
  int rc = prepareSqliteStatement(db, selectQuery, selectStmt);
  if (rc != SQLITE_OK) {
    return false;
  }

  std::vector<PendingNormalization> pending;
  while (sqlite3_step(selectStmt.get()) == SQLITE_ROW) {
    const sqlite3_int64 rowid = sqlite3_column_int64(selectStmt.get(), 0);
    const auto *text = reinterpret_cast<const char *>(
        sqlite3_column_text(selectStmt.get(), 1));
    if (text == nullptr) {
      continue;
    }

    const auto normalized = normalizedPathTextForStorage(text);
    if (normalized.has_value()) {
      pending.push_back({.rowid = rowid, .normalized = *normalized});
    }
  }
  if (pending.empty()) {
    return false;
  }

  std::string updateQuery =
      std::string(primaryKey ? "UPDATE OR IGNORE " : "UPDATE ") + table +
      " SET " + column + " = ? WHERE rowid = ?";
  SqliteStatementHandle updateStmt;
  rc = prepareSqliteStatement(db, updateQuery, updateStmt);
  if (rc != SQLITE_OK) {
    return false;
  }

  SqliteStatementHandle existsStmt;
  SqliteStatementHandle deleteStmt;
  if (primaryKey) {
    std::string existsQuery = "SELECT 1 FROM ";
    existsQuery += table;
    existsQuery += " WHERE ";
    existsQuery += column;
    existsQuery += " = ? LIMIT 1";
    rc = prepareSqliteStatement(db, existsQuery, existsStmt);
    if (rc != SQLITE_OK) {
      return false;
    }

    std::string deleteQuery = "DELETE FROM ";
    deleteQuery += table;
    deleteQuery += " WHERE rowid = ?";
    rc = prepareSqliteStatement(db, deleteQuery, deleteStmt);
    if (rc != SQLITE_OK) {
      return false;
    }
  }

  bool changed = false;
  for (const auto &item : pending) {
    sqlite3_reset(updateStmt.get());
    sqlite3_clear_bindings(updateStmt.get());
    bindSqliteText(updateStmt.get(), 1, item.normalized);
    sqlite3_bind_int64(updateStmt.get(), 2, item.rowid);
    rc = sqlite3_step(updateStmt.get());
    if (rc == SQLITE_DONE && sqlite3_changes(db) > 0) {
      changed = true;
      continue;
    }

    if (!primaryKey ||
        !normalizedPathValueExists(existsStmt.get(), item.normalized)) {
      continue;
    }
    sqlite3_reset(deleteStmt.get());
    sqlite3_clear_bindings(deleteStmt.get());
    sqlite3_bind_int64(deleteStmt.get(), 1, item.rowid);
    if (sqlite3_step(deleteStmt.get()) == SQLITE_DONE &&
        sqlite3_changes(db) > 0) {
      changed = true;
    }
  }

  if (changed) {
    SDL_Log("Normalized stored app document paths in %s.%s", table, column);
  }
  return changed;
}

bool normalizeStoredHashColumnChecked(sqlite3 *db, const char *table,
                                      const char *column, bool &changed) {
  int changedCount = 0;
  if (!updateSqliteColumnWithExpressionLogged(
          db, table, column, normalizedSqlHash(column),
          "normalizing stored chart hash column", logSqlErrorText,
          &changedCount)) {
    return false;
  }

  if (changedCount > 0) {
    SDL_Log("Normalized %d stored chart hashes in %s.%s", changedCount, table,
            column);
    changed = true;
  }
  return true;
}

sqlite3_int64 clampSqlInteger(std::uint64_t value) {
  return value > static_cast<std::uint64_t>(
                     std::numeric_limits<sqlite3_int64>::max())
             ? std::numeric_limits<sqlite3_int64>::max()
             : static_cast<sqlite3_int64>(value);
}

bool createChartMetadataRebuildStateTable(sqlite3 *db) {
  const char *query =
      "CREATE TABLE IF NOT EXISTS chart_meta_rebuild_state ("
      "id INTEGER PRIMARY KEY CHECK(id = 1),"
      "required INTEGER NOT NULL DEFAULT 0,"
      "updated_at TEXT DEFAULT CURRENT_TIMESTAMP"
      ")";
  return execSql(db, query, "creating chart metadata rebuild state table");
}

bool setChartMetadataRebuildRequired(sqlite3 *db, bool required) {
  if (!createChartMetadataRebuildStateTable(db)) {
    return false;
  }
  const char *query =
      "INSERT INTO chart_meta_rebuild_state (id, required, updated_at) "
      "VALUES (1, ?, CURRENT_TIMESTAMP) "
      "ON CONFLICT(id) DO UPDATE SET required = excluded.required, "
      "updated_at = CURRENT_TIMESTAMP";
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt,
                                    "preparing chart metadata rebuild state",
                                    logSqlErrorText)) {
    return false;
  }
  sqlite3_bind_int(stmt.get(), 1, required ? 1 : 0);
  if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
    logSqlError("updating chart metadata rebuild state", db);
    return false;
  }
  return true;
}

bool clearChartMetadataRebuildRequiredIfPresent(sqlite3 *db) {
  bool tableExists = false;
  if (!sqliteTableExists(db, "chart_meta_rebuild_state", tableExists,
                         "checking chart metadata rebuild state table")) {
    return false;
  }
  if (!tableExists) {
    return true;
  }
  return setChartMetadataRebuildRequired(db, false);
}

bool invalidateChartMetadataForNormalScan(sqlite3 *db, bool &completed) {
  completed = false;
  if (!execSql(db, "SAVEPOINT chart_metadata_rebuild_migration",
               "starting chart metadata rebuild migration")) {
    return false;
  }

  bool ok = true;
  const char *queries[] = {
      "DROP TABLE IF EXISTS chart_meta",
      "DROP TABLE IF EXISTS solid_archives",
      "DROP TABLE IF EXISTS archive_scan_cache",
      "DROP TABLE IF EXISTS chart_scan_checkpoint",
  };
  for (const auto *query : queries) {
    if (!execSql(db, query, "invalidating chart metadata cache")) {
      ok = false;
      break;
    }
  }
  if (ok) {
    ok = createChartMetaTableSchema(db);
  }
  if (ok) {
    ok = setChartMetadataRebuildRequired(db, true);
  }

  if (ok) {
    ok = execSql(db, "RELEASE chart_metadata_rebuild_migration",
                 "committing chart metadata rebuild migration");
  } else {
    execSql(db, "ROLLBACK TO chart_metadata_rebuild_migration",
            "rolling back chart metadata rebuild migration");
    execSql(db, "RELEASE chart_metadata_rebuild_migration",
            "releasing chart metadata rebuild migration");
    return false;
  }

  SDL_Log("Chart metadata cache invalidated; normal library scan will rebuild");
  chart_repository_detail::BumpLibraryRevision();
  completed = true;
  return true;
}

class ChartDatabaseMigrationPass {
public:
  using RunFunction = bool (*)(sqlite3 *, bool &completed);

  constexpr ChartDatabaseMigrationPass(int targetVersion, const char *name,
                                       RunFunction run)
      : targetVersion_(targetVersion), name_(name), run_(run) {}

  int targetVersion() const { return targetVersion_; }
  const char *name() const { return name_; }

  bool run(sqlite3 *db, bool &completed) const { return run_(db, completed); }

private:
  int targetVersion_;
  const char *name_;
  RunFunction run_;
};

bool migrateChartDatabaseToVersion1(sqlite3 *db, bool &completed) {
  return invalidateChartMetadataForNormalScan(db, completed);
}

bool normalizeExistingChartTablePaths(sqlite3 *db, const char *table,
                                      const char *column, bool primaryKey,
                                      bool &changed) {
  bool exists = false;
  if (!sqliteTableExists(db, table, exists,
                         "checking chart path normalization table")) {
    return false;
  }
  if (!exists) {
    return true;
  }
  changed = normalizeStoredPathColumn(db, table, column, primaryKey) || changed;
  return true;
}

bool normalizeExistingChartTableHashes(sqlite3 *db, const char *table,
                                       const char *md5Column,
                                       const char *sha256Column,
                                       bool &changed) {
  bool exists = false;
  if (!sqliteTableExists(db, table, exists,
                         "checking chart hash normalization table")) {
    return false;
  }
  if (!exists) {
    return true;
  }
  return normalizeStoredHashColumnChecked(db, table, md5Column, changed) &&
         normalizeStoredHashColumnChecked(db, table, sha256Column, changed);
}

bool migrateChartDatabaseToVersion2(sqlite3 *db, bool &completed) {
  bool changed = false;
  if (!normalizeExistingChartTablePaths(db, "chart_meta", "path", true,
                                        changed) ||
      !normalizeExistingChartTablePaths(db, "chart_meta", "folder", false,
                                        changed) ||
      !normalizeExistingChartTablePaths(db, "chart_favorites", "chart_path",
                                        true, changed) ||
      !normalizeExistingChartTablePaths(db, "solid_archives", "path", true,
                                        changed) ||
      !normalizeExistingChartTablePaths(db, "entries", "path", true,
                                        changed) ||
      !normalizeExistingChartTablePaths(db, "chart_scan_checkpoint",
                                        "last_path", false, changed) ||
      !normalizeExistingChartTablePaths(db, "chart_scan_checkpoint",
                                        "archive_path", false, changed) ||
      !normalizeExistingChartTablePaths(db, "archive_scan_cache", "path",
                                        true, changed) ||
      !normalizeExistingChartTableHashes(db, "chart_meta", "md5", "sha256",
                                         changed) ||
      !normalizeExistingChartTableHashes(db, "chart_favorites", "chart_md5",
                                         "chart_sha256", changed) ||
      !normalizeExistingChartTableHashes(db, "difficulty_table_entries", "md5",
                                         "sha256", changed) ||
      !normalizeExistingChartTableHashes(db, "difficulty_course_entries",
                                         "md5", "sha256", changed)) {
    return false;
  }
  if (changed) {
    chart_repository_detail::InvalidateDifficultyLabelCache();
    chart_repository_detail::BumpLibraryRevision();
  }
  completed = true;
  return true;
}

bool migrateChartDatabaseToVersion3(sqlite3 *db, bool &completed) {
  return invalidateChartMetadataForNormalScan(db, completed);
}

bool runChartDatabaseMigrationPasses(
    sqlite3 *db, const ChartDatabaseMigrationPass *passes,
    std::size_t passCount, int latestVersion) {
  int currentVersion = databaseUserVersion(db);
  if (currentVersion >= latestVersion) {
    return true;
  }

  for (std::size_t i = 0; i < passCount; ++i) {
    const ChartDatabaseMigrationPass &pass = passes[i];
    if (currentVersion >= pass.targetVersion()) {
      continue;
    }

    bool completed = false;
    if (!pass.run(db, completed)) {
      std::cerr << "Chart database migration failed for version "
                << pass.targetVersion() << " (" << pass.name() << ")\n";
      return false;
    }
    if (!completed) {
      return true;
    }
    if (!setDatabaseUserVersion(db, pass.targetVersion())) {
      return false;
    }
    currentVersion = pass.targetVersion();
  }

  if (currentVersion < latestVersion) {
    std::cerr << "No chart database migration pass reached version "
              << latestVersion << "\n";
    return false;
  }
  return true;
}

bool migrateChartDatabaseSchema(sqlite3 *db) {
  static constexpr ChartDatabaseMigrationPass kMigrationPasses[] = {
      {1, "chart metadata rebuild", migrateChartDatabaseToVersion1},
      {2, "normalize chart identity storage", migrateChartDatabaseToVersion2},
      {3, "persist authored TOTAL metadata", migrateChartDatabaseToVersion3},
  };
  return runChartDatabaseMigrationPasses(
      db, kMigrationPasses,
      sizeof(kMigrationPasses) / sizeof(kMigrationPasses[0]),
      kChartDatabaseSchemaVersion);
}

bool createChartScanCheckpointTable(sqlite3 *db) {
  const char *query =
      "CREATE TABLE IF NOT EXISTS chart_scan_checkpoint ("
      "id INTEGER PRIMARY KEY CHECK(id = 1),"
      "scan_signature TEXT NOT NULL DEFAULT '',"
      "phase TEXT NOT NULL DEFAULT '',"
      "next_index INTEGER NOT NULL DEFAULT 0,"
      "sub_index INTEGER NOT NULL DEFAULT 0,"
      "last_path TEXT NOT NULL DEFAULT '',"
      "archive_path TEXT NOT NULL DEFAULT '',"
      "archive_size INTEGER NOT NULL DEFAULT 0,"
      "archive_mtime_ns INTEGER NOT NULL DEFAULT 0,"
      "last_inner_path TEXT NOT NULL DEFAULT '',"
      "updated_at TEXT DEFAULT CURRENT_TIMESTAMP"
      ")";
  if (!execSql(db, query, "creating chart scan checkpoint table")) {
    return false;
  }
  return true;
}

bool clearChartScanCheckpoint(sqlite3 *db) {
  if (!createChartScanCheckpointTable(db)) {
    return false;
  }
  return execSql(db, "DELETE FROM chart_scan_checkpoint",
                 "clearing chart scan checkpoint");
}

bool createArchiveScanCacheTable(sqlite3 *db) {
  const char *query =
      "CREATE TABLE IF NOT EXISTS archive_scan_cache ("
      "path TEXT PRIMARY KEY,"
      "archive_size INTEGER NOT NULL DEFAULT 0,"
      "mtime_ns INTEGER NOT NULL DEFAULT 0,"
      "solid INTEGER NOT NULL DEFAULT 0,"
      "uncompressed_size INTEGER NOT NULL DEFAULT 0,"
      "file_count INTEGER NOT NULL DEFAULT 0,"
      "chart_count INTEGER NOT NULL DEFAULT -1,"
      "updated_at TEXT DEFAULT CURRENT_TIMESTAMP"
      ")";
  if (!execSql(db, query, "creating archive scan cache table")) {
    return false;
  }
  if (!execSqlAllowDuplicateColumn(
          db,
          "ALTER TABLE archive_scan_cache "
          "ADD COLUMN chart_count INTEGER NOT NULL DEFAULT -1",
          "migrating archive scan cache chart count")) {
    return false;
  }

  const char *indexes[] = {
      "CREATE INDEX IF NOT EXISTS idx_archive_scan_cache_state "
      "ON archive_scan_cache(path, archive_size, mtime_ns)",
      "CREATE INDEX IF NOT EXISTS idx_archive_scan_cache_solid "
      "ON archive_scan_cache(solid)",
  };
  for (const auto *indexQuery : indexes) {
    if (!execSql(db, indexQuery, "creating archive scan cache index")) {
      return false;
    }
  }
  return true;
}

std::vector<std::filesystem::path> selectArchiveScanCachePaths(sqlite3 *db) {
  std::vector<std::filesystem::path> paths;
  SqliteStatementHandle stmt;
  int rc =
      prepareSqliteStatement(db, "SELECT path FROM archive_scan_cache", stmt);
  if (rc != SQLITE_OK) {
    return paths;
  }
  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    const auto *text =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt.get(), 0));
    if (text == nullptr) {
      continue;
    }
    std::filesystem::path path(utf8_to_path_t(text));
    chart_storage_identity::ToAbsolutePath(path);
    paths.push_back(path);
  }
  return paths;
}

bool deleteArchiveScanCache(sqlite3 *db,
                            const std::filesystem::path &archivePath) {
  const std::string pathText =
      chart_storage_identity::StoredPathText(archivePath);
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(
          db, "DELETE FROM archive_scan_cache WHERE path = ?", stmt,
          "preparing archive scan cache delete", logSqlErrorText)) {
    return false;
  }
  bindSqliteText(stmt.get(), 1, pathText);
  int rc = sqlite3_step(stmt.get());
  if (rc != SQLITE_DONE) {
    logSqlError("deleting archive scan cache", db);
    return false;
  }
  const bool changed = sqlite3_changes(db) > 0;
  return changed;
}

bool deleteSolidArchive(sqlite3 *db, const std::filesystem::path &archivePath) {
  const std::string pathText =
      chart_storage_identity::StoredPathText(archivePath);
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(
          db, "DELETE FROM solid_archives WHERE path = ?", stmt,
          "preparing solid archive delete", logSqlErrorText)) {
    return false;
  }
  bindSqliteText(stmt.get(), 1, pathText);
  int rc = sqlite3_step(stmt.get());
  if (rc != SQLITE_DONE) {
    logSqlError("deleting solid archive", db);
    return false;
  }
  const bool changed = sqlite3_changes(db) > 0;
  return changed;
}

bool deleteChartMetaInArchive(sqlite3 *db,
                              const std::filesystem::path &archivePath) {
  std::vector<std::filesystem::path> chartPaths;
  SqliteStatementHandle selectStmt;
  if (!prepareSqliteStatementLogged(db, "SELECT path FROM chart_meta",
                                    selectStmt,
                                    "selecting archive chart paths",
                                    logSqlErrorText)) {
    return false;
  }
  while (sqlite3_step(selectStmt.get()) == SQLITE_ROW) {
    std::filesystem::path path(utf8_to_path_t(
        sqliteColumnString(selectStmt.get(), 0)));
    chart_storage_identity::ToAbsolutePath(path);
    chartPaths.push_back(std::move(path));
  }

  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(
          db, "DELETE FROM chart_meta WHERE path = ?", stmt,
          "preparing archive chart delete", logSqlErrorText)) {
    return false;
  }

  bool changed = false;
  const std::filesystem::path targetArchive = archivePath.lexically_normal();
  for (const auto &path : chartPaths) {
    if (!pathIsInsideDirectory(path, targetArchive)) {
      continue;
    }

    const std::string pathText = chart_storage_identity::StoredPathText(path);
    sqlite3_reset(stmt.get());
    sqlite3_clear_bindings(stmt.get());
    bindSqliteText(stmt.get(), 1, pathText);
    const int rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE) {
      logSqlError("deleting archive chart", db);
      return changed;
    }
    changed = sqlite3_changes(db) > 0 || changed;
  }
  return changed;
}

std::vector<std::filesystem::path> selectSolidArchivePaths(sqlite3 *db) {
  std::vector<std::filesystem::path> paths;
  SqliteStatementHandle stmt;
  int rc = prepareSqliteStatement(db, "SELECT path FROM solid_archives", stmt);
  if (rc != SQLITE_OK) {
    return paths;
  }
  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    const auto *text =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt.get(), 0));
    if (text == nullptr) {
      continue;
    }
    std::filesystem::path path(utf8_to_path_t(text));
    chart_storage_identity::ToAbsolutePath(path);
    paths.push_back(path);
  }
  return paths;
}

int findDifficultyTable(sqlite3 *db, const std::string &name,
                        const std::string &symbol,
                        const std::string &sourceUrl) {
  auto query =
      "SELECT id FROM difficulty_tables WHERE name = @name AND symbol = "
      "@symbol AND source_url = @source_url";
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt,
                                    "looking up difficulty table",
                                    logSqlErrorText)) {
    return 0;
  }
  bindSqliteText(stmt.get(), 1, name);
  bindSqliteText(stmt.get(), 2, symbol);
  bindSqliteText(stmt.get(), 3, sourceUrl);
  int id = 0;
  if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    id = sqlite3_column_int(stmt.get(), 0);
  }
  return id;
}

int findDifficultyTableBySourceUrl(sqlite3 *db, const std::string &sourceUrl) {
  if (sourceUrl.empty()) {
    return 0;
  }

  auto query = "SELECT id FROM difficulty_tables WHERE source_url = "
               "@source_url ORDER BY id LIMIT 1";
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt,
                                    "looking up difficulty table source URL",
                                    logSqlErrorText)) {
    return 0;
  }
  bindSqliteText(stmt.get(), 1, sourceUrl);
  int id = 0;
  if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    id = sqlite3_column_int(stmt.get(), 0);
  }
  return id;
}

bool readDifficultyTableSourceUrl(sqlite3 *db, int tableId,
                                  std::string &sourceUrl,
                                  std::string *errorMessage) {
  auto query = "SELECT source_url FROM difficulty_tables WHERE id = @id";
  SqliteStatementHandle stmt;
  const int rc = prepareSqliteStatement(db, query, stmt);
  if (rc != SQLITE_OK) {
    if (errorMessage != nullptr) {
      *errorMessage = std::string("Could not read table source URL: ") +
                      sqliteDatabaseError(db);
    }
    return false;
  }
  sqlite3_bind_int(stmt.get(), 1, tableId);

  const int step = sqlite3_step(stmt.get());
  if (step == SQLITE_ROW) {
    sourceUrl = sqliteColumnString(stmt.get(), 0);
    return true;
  }

  if (errorMessage != nullptr) {
    *errorMessage = "Difficulty table was not found";
  }
  return false;
}

bool clearDifficultyTableContent(sqlite3 *db, int tableId) {
  auto deleteCourseEntries =
      "DELETE FROM difficulty_course_entries WHERE course_id IN "
      "(SELECT id FROM difficulty_courses WHERE table_id = @table_id)";
  SqliteStatementHandle stmt;
  int rc = prepareSqliteStatement(db, deleteCourseEntries, stmt);
  if (rc != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_int(stmt.get(), 1, tableId);
  rc = sqlite3_step(stmt.get());
  if (rc != SQLITE_DONE) {
    return false;
  }

  auto deleteCourses =
      "DELETE FROM difficulty_courses WHERE table_id = @table_id";
  rc = prepareSqliteStatement(db, deleteCourses, stmt);
  if (rc != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_int(stmt.get(), 1, tableId);
  rc = sqlite3_step(stmt.get());
  if (rc != SQLITE_DONE) {
    return false;
  }

  auto deleteEntries =
      "DELETE FROM difficulty_table_entries WHERE table_id = @table_id";
  rc = prepareSqliteStatement(db, deleteEntries, stmt);
  if (rc != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_int(stmt.get(), 1, tableId);
  rc = sqlite3_step(stmt.get());
  return rc == SQLITE_DONE;
}

bool readRetainedDifficultyCourses(sqlite3 *db, int tableId,
                                   RetainedDifficultyCourses &courses) {
  const char *query =
      "SELECT dc.id, dc.course_key, dc.name, dc.group_name, "
      "dc.constraint_json, dce.id, dce.md5, dce.sha256 "
      "FROM difficulty_courses dc "
      "LEFT JOIN difficulty_course_entries dce ON dce.course_id = dc.id "
      "WHERE dc.table_id = @table_id "
      "ORDER BY dc.sort_order, dc.id, dce.sort_order, dce.id";
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt,
                                    "reading difficulty course identities",
                                    logSqlErrorText)) {
    return false;
  }
  sqlite3_bind_int(stmt.get(), 1, tableId);

  course_identity::Definition current;
  const auto retainCurrentCourse = [&]() {
    if (current.courseId <= 0) {
      return;
    }
    courses.push_back(std::move(current));
    current = {};
  };

  int rc = SQLITE_OK;
  while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
    const int courseId = sqlite3_column_int(stmt.get(), 0);
    if (courseId != current.courseId) {
      retainCurrentCourse();
      current.courseId = courseId;
      current.courseKey = columnString(stmt.get(), 1);
      current.name = columnString(stmt.get(), 2);
      current.groupName = columnString(stmt.get(), 3);
      current.constraintJson = columnString(stmt.get(), 4);
    }
    if (sqlite3_column_type(stmt.get(), 5) != SQLITE_NULL) {
      current.charts.push_back({.sha256 = columnString(stmt.get(), 7),
                                .md5 = columnString(stmt.get(), 6)});
    }
  }
  if (rc != SQLITE_DONE) {
    return false;
  }
  retainCurrentCourse();
  return true;
}

int upsertDifficultyTable(sqlite3 *db, const std::string &name,
                          const std::string &symbol, const std::string &dataUrl,
                          const std::string &sourceUrl,
                          RetainedDifficultyCourses &retainedCourses) {
  int tableId = findDifficultyTableBySourceUrl(db, sourceUrl);
  if (tableId <= 0) {
    tableId = findDifficultyTable(db, name, symbol, sourceUrl);
  }
  if (tableId > 0) {
    if (!readRetainedDifficultyCourses(db, tableId, retainedCourses)) {
      return 0;
    }
    auto updateQuery =
        "UPDATE difficulty_tables SET name = @name, symbol = @symbol, "
        "data_url = @data_url, updated_at = CURRENT_TIMESTAMP WHERE id = @id";
    SqliteStatementHandle stmt;
    int rc = prepareSqliteStatement(db, updateQuery, stmt);
    if (rc != SQLITE_OK) {
      return 0;
    }
    bindSqliteText(stmt.get(), 1, name);
    bindSqliteText(stmt.get(), 2, symbol);
    bindSqliteText(stmt.get(), 3, dataUrl);
    sqlite3_bind_int(stmt.get(), 4, tableId);
    rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE || !clearDifficultyTableContent(db, tableId)) {
      return 0;
    }
    return tableId;
  }

  auto insertQuery =
      "INSERT INTO difficulty_tables "
      "(name, symbol, data_url, source_url, updated_at) "
      "VALUES (@name, @symbol, @data_url, @source_url, CURRENT_TIMESTAMP)";
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, insertQuery, stmt,
                                    "inserting difficulty table",
                                    logSqlErrorText)) {
    return 0;
  }
  bindSqliteText(stmt.get(), 1, name);
  bindSqliteText(stmt.get(), 2, symbol);
  bindSqliteText(stmt.get(), 3, dataUrl);
  bindSqliteText(stmt.get(), 4, sourceUrl);
  int rc = sqlite3_step(stmt.get());
  if (rc != SQLITE_DONE) {
    return 0;
  }
  return static_cast<int>(sqlite3_last_insert_rowid(db));
}

bool insertDifficultyTableEntry(sqlite3 *db, int tableId,
                                const TableChartItem &chart, int sortOrder) {
  auto query =
      "INSERT INTO difficulty_table_entries "
      "(table_id, level, md5, sha256, title, subtitle, artist, subartist, "
      "url, url_diff, sort_order) "
      "VALUES (@table_id, @level, @md5, @sha256, @title, @subtitle, "
      "@artist, @subartist, @url, @url_diff, @sort_order)";
  SqliteStatementHandle stmt;
  int rc = prepareSqliteStatement(db, query, stmt);
  if (rc != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_int(stmt.get(), 1, tableId);
  bindSqliteText(stmt.get(), 2, chart.level);
  bindSqliteText(stmt.get(), 3, chart.md5);
  bindSqliteText(stmt.get(), 4, chart.sha256);
  bindSqliteText(stmt.get(), 5, chart.title);
  bindSqliteText(stmt.get(), 6, chart.subtitle);
  bindSqliteText(stmt.get(), 7, chart.artist);
  bindSqliteText(stmt.get(), 8, chart.subartist);
  bindSqliteText(stmt.get(), 9, chart.url);
  bindSqliteText(stmt.get(), 10, chart.urlDiff);
  sqlite3_bind_int(stmt.get(), 11, sortOrder);
  rc = sqlite3_step(stmt.get());
  return rc == SQLITE_DONE;
}

int insertDifficultyCourse(sqlite3 *db, int tableId, const std::string &name,
                           const std::string &groupName,
                           const std::string &level,
                           const std::string &constraintJson,
                           const std::string &courseKey, int sortOrder,
                           int retainedCourseId) {
  const bool retainId = retainedCourseId > 0;
  const char *query =
      retainId
          ? "INSERT INTO difficulty_courses "
            "(id, table_id, name, group_name, level, constraint_json, "
            "course_key, sort_order) VALUES (@id, @table_id, @name, "
            "@group_name, @level, @constraint_json, @course_key, @sort_order)"
          : "INSERT INTO difficulty_courses "
            "(table_id, name, group_name, level, constraint_json, course_key, "
            "sort_order) VALUES (@table_id, @name, @group_name, @level, "
            "@constraint_json, @course_key, @sort_order)";
  SqliteStatementHandle stmt;
  int rc = prepareSqliteStatement(db, query, stmt);
  if (rc != SQLITE_OK) {
    return 0;
  }
  int parameter = 1;
  if (retainId) {
    sqlite3_bind_int(stmt.get(), parameter++, retainedCourseId);
  }
  sqlite3_bind_int(stmt.get(), parameter++, tableId);
  bindSqliteText(stmt.get(), parameter++, name);
  bindSqliteText(stmt.get(), parameter++, groupName);
  bindSqliteText(stmt.get(), parameter++, level);
  bindSqliteText(stmt.get(), parameter++, constraintJson);
  bindSqliteText(stmt.get(), parameter++, courseKey);
  sqlite3_bind_int(stmt.get(), parameter, sortOrder);
  rc = sqlite3_step(stmt.get());
  if (rc != SQLITE_DONE) {
    return 0;
  }
  return retainId ? retainedCourseId
                  : static_cast<int>(sqlite3_last_insert_rowid(db));
}

bool insertDifficultyCourseEntry(sqlite3 *db, int courseId,
                                 const TableChartItem &chart, int sortOrder) {
  auto query = "INSERT INTO difficulty_course_entries "
               "(course_id, level, md5, sha256, title, subtitle, artist, "
               "subartist, url, url_diff, sort_order) "
               "VALUES (@course_id, @level, @md5, @sha256, @title, "
               "@subtitle, @artist, @subartist, @url, @url_diff, "
               "@sort_order)";
  SqliteStatementHandle stmt;
  int rc = prepareSqliteStatement(db, query, stmt);
  if (rc != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_int(stmt.get(), 1, courseId);
  bindSqliteText(stmt.get(), 2, chart.level);
  bindSqliteText(stmt.get(), 3, chart.md5);
  bindSqliteText(stmt.get(), 4, chart.sha256);
  bindSqliteText(stmt.get(), 5, chart.title);
  bindSqliteText(stmt.get(), 6, chart.subtitle);
  bindSqliteText(stmt.get(), 7, chart.artist);
  bindSqliteText(stmt.get(), 8, chart.subartist);
  bindSqliteText(stmt.get(), 9, chart.url);
  bindSqliteText(stmt.get(), 10, chart.urlDiff);
  sqlite3_bind_int(stmt.get(), 11, sortOrder);
  rc = sqlite3_step(stmt.get());
  return rc == SQLITE_DONE;
}

int columnInt(sqlite3_stmt *stmt, int idx) {
  return sqlite3_column_type(stmt, idx) == SQLITE_NULL
             ? 0
             : sqlite3_column_int(stmt, idx);
}

std::string columnString(sqlite3_stmt *stmt, int idx) {
  return sqliteColumnString(stmt, idx);
}

} // namespace

ChartRepository::Impl::Impl(std::filesystem::path path)
    : databasePath(std::move(path)) {}

ChartSessionStorage::ChartSessionStorage(sqlite3 *database)
    : connection(database) {}

sqlite3 *ChartSessionStorage::database() const { return connection.get(); }

ChartRepository::Session::Impl::Impl(
    ChartRepository &owner, sqlite3 *database, ScoreRepository *scoresValue)
    : repository(&owner), storage(std::make_shared<ChartSessionStorage>(database)),
      scores(scoresValue) {}

ScoreRepository &ChartRepository::Session::Impl::scoreRepository() {
  return scores != nullptr ? *scores : fallbackScores;
}

sqlite3 *ChartRepository::Session::Impl::database() const {
  return storage->database();
}

ChartRepository::Session::Session(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

ChartRepository::Session::~Session() = default;
ChartRepository::Session::Session(Session &&) noexcept = default;
ChartRepository::Session &
ChartRepository::Session::operator=(Session &&) noexcept = default;

sqlite3 *ChartRepository::Session::NativeHandleForScoreRepository() const {
  return impl_->database();
}

bool ChartRepository::Session::EnsureSchema() {
  sqlite3 *db = impl_->database();
  return chart_repository_detail::EnsureCoreSchema(db) &&
         chart_repository_detail::EnsureDifficultySchema(db);
}

bool ChartRepository::Session::InsertChartMeta(
    bms_parser::ChartMeta &chartMeta) {
  return impl_->repository->InsertChartMeta(impl_->database(), chartMeta);
}

bool ChartRepository::Session::DeleteChartMeta(std::filesystem::path path) {
  return impl_->repository->DeleteChartMeta(impl_->database(),
                                             std::move(path));
}

int ChartRepository::Session::DeleteChartMetaInDirectory(
    const std::filesystem::path &directory) {
  return impl_->repository->DeleteChartMetaInDirectory(impl_->database(),
                                                        directory);
}

bool ChartRepository::Session::DeleteArchiveRecords(
    const std::filesystem::path &archivePath) {
  return impl_->repository->DeleteArchiveRecords(impl_->database(),
                                                  archivePath);
}

bool ChartRepository::Session::ClearChartMeta() {
  return impl_->repository->ClearChartMeta(impl_->database());
}

bool ChartRepository::Session::InsertEntry(
    const std::filesystem::path &path, const std::string &iosBookmark) {
  return impl_->repository->InsertEntry(impl_->database(), path,
                                        iosBookmark);
}

std::vector<ChartEntry> ChartRepository::Session::SelectAllEntries() {
  return impl_->repository->SelectAllEntries(impl_->database());
}

std::vector<ChartEntry> ChartRepository::Session::SelectEffectiveEntries() {
  return impl_->repository->SelectEffectiveEntries(impl_->database());
}

bool ChartRepository::Session::DeleteEntry(
    const std::filesystem::path &path) {
  return impl_->repository->DeleteEntry(impl_->database(), path);
}

bool ChartRepository::Session::DeleteEntryAndChartMetaInDirectory(
    const std::filesystem::path &path, int &removedChartCount) {
  removedChartCount = -1;
  std::string transactionError;
  SqliteTransactionHandle transaction(impl_->database(), "BEGIN",
                                      transactionError);
  if (!transaction.active()) {
    return false;
  }
  removedChartCount =
      impl_->repository->DeleteChartMetaInDirectory(impl_->database(),
                                                     path);
  if (removedChartCount < 0 ||
      !impl_->repository->DeleteEntry(impl_->database(), path)) {
    return false;
  }
  return transaction.commit(transactionError);
}

bool ChartRepository::Session::ClearEntries() {
  return impl_->repository->ClearEntries(impl_->database());
}

bool ChartRepository::Session::ImportDifficultyTable(
    const std::string &headerJson, const std::string &dataJson,
    const std::string &sourceUrl) {
  return impl_->repository->ImportDifficultyTable(
      impl_->database(), headerJson, dataJson, sourceUrl);
}

bool ChartRepository::Session::ImportDifficultyTableFromUrl(
    const std::string &pageUrl, std::string *errorMessage,
    DifficultyTableImportProgressCallback progressCallback) {
  return impl_->repository->ImportDifficultyTableFromUrl(
      impl_->database(), pageUrl, errorMessage,
      std::move(progressCallback));
}

bool ChartRepository::Session::UpdateDifficultyTableFromSourceUrl(
    int tableId, std::string *errorMessage) {
  return impl_->repository->UpdateDifficultyTableFromSourceUrl(
      impl_->database(), tableId, errorMessage);
}

int ChartRepository::Session::ImportDifficultyTablesFromDirectory(
    const std::filesystem::path &directory) {
  return impl_->repository->ImportDifficultyTablesFromDirectory(
      impl_->database(), directory);
}

bool ChartRepository::EnsureReady() {
  std::lock_guard lock(impl_->readinessMutex);
  if (impl_->ready) {
    return true;
  }

  const std::filesystem::path directory = impl_->databasePath.parent_path();
  std::cout << "DB Directory: " << fspath_to_utf8(directory) << "\n";
  std::error_code directoryError;
  if (!directory.empty() &&
      !Utils::EnsureDirectoryExists(directory, directoryError)) {
    std::cerr << "Can't create chart database directory "
              << fspath_to_utf8(directory) << ": "
              << directoryError.message() << "\n";
    return false;
  }
  std::cout << "DB Path: " << fspath_to_utf8(impl_->databasePath) << "\n";

  std::string openError;
  SqliteConnectionHandle connection(openValidatedSqliteDatabase(
      impl_->databasePath, kChartDatabaseSchemaVersion,
      SqliteValidatedOpenPolicy{
          .enableForeignKeys = false,
          .disableCheckpointOnClose = false,
      },
      openError));
  if (!connection) {
    std::cerr << "Can't open chart database: " << openError << "\n";
    return false;
  }
  if (const auto pragmaError =
          applySqlitePragmas(connection.get(), {"PRAGMA synchronous=NORMAL"})) {
    std::cerr << "Could not configure chart database: " << *pragmaError
              << "\n";
    return false;
  }

  const bool ok = chart_repository_detail::EnsureCoreSchema(connection.get()) &&
                  chart_repository_detail::EnsureDifficultySchema(
                      connection.get());
  impl_->ready = ok;
  return ok;
}

std::optional<ChartRepository::Session>
ChartRepository::OpenSession(ScoreRepository *scores) {
  if (!EnsureReady()) {
    return std::nullopt;
  }

  sqlite3 *raw = nullptr;
  const std::string pathText = fspath_to_utf8(impl_->databasePath);
  const int openRc = sqlite3_open_v2(
      pathText.c_str(), &raw,
      SQLITE_OPEN_READWRITE | SQLITE_OPEN_PRIVATECACHE, nullptr);
  SqliteConnectionHandle connection(raw);
  if (openRc != SQLITE_OK || !connection) {
    std::cerr << "Can't open chart database session: "
              << (raw != nullptr ? sqlite3_errmsg(raw) : "unknown error")
              << "\n";
    return std::nullopt;
  }
  sqlite3_busy_timeout(connection.get(), 1000);

  std::string versionError;
  const auto version = readSqliteUserVersion(connection.get(), versionError);
  if (!version.has_value() || *version != kChartDatabaseSchemaVersion) {
    std::cerr << "Can't use chart database session: "
              << (versionError.empty() ? "unexpected schema version"
                                       : versionError)
              << "\n";
    return std::nullopt;
  }
  if (const auto pragmaError = applySqlitePragmas(
          connection.get(),
          {"PRAGMA journal_mode=WAL", "PRAGMA synchronous=NORMAL"})) {
    std::cerr << "Could not configure chart database session: "
              << *pragmaError << "\n";
  }

  sqlite3 *database = connection.release();
  return Session(std::make_unique<Session::Impl>(*this, database, scores));
}

const std::filesystem::path &ChartRepository::DatabasePath() const {
  return impl_->databasePath;
}

static bool createFavoritesTable(sqlite3 *db);

static bool createChartMetaTable(sqlite3 *db) {
  bool existingChartMetaTable = false;
  if (!sqliteTableExists(db, "chart_meta", existingChartMetaTable,
                         "checking chart meta table existence")) {
    return false;
  }

  if (!createChartMetaTableSchema(db)) {
    return false;
  }

  if (existingChartMetaTable) {
    if (!migrateChartDatabaseSchema(db)) {
      return false;
    }
  } else if (!setDatabaseUserVersion(db, kChartDatabaseSchemaVersion)) {
    return false;
  }

  const char *indexes[] = {
      "CREATE INDEX IF NOT EXISTS idx_chart_meta_path ON chart_meta(path)",
      "CREATE INDEX IF NOT EXISTS idx_chart_meta_folder ON chart_meta(folder)",
      "CREATE INDEX IF NOT EXISTS idx_chart_meta_folder_source "
      "ON chart_meta(folder, source_priority, source_archive_size, path)",
      "CREATE INDEX IF NOT EXISTS idx_chart_meta_title ON chart_meta(title)",
      "CREATE INDEX IF NOT EXISTS idx_chart_meta_title_path "
      "ON chart_meta(title, path)",
      "CREATE INDEX IF NOT EXISTS idx_chart_meta_sha256 ON chart_meta(sha256)",
      "CREATE INDEX IF NOT EXISTS idx_chart_meta_md5 ON chart_meta(md5)",
      "CREATE INDEX IF NOT EXISTS idx_chart_meta_sha256_source "
      "ON chart_meta(sha256, source_priority, source_archive_size, path)",
      "CREATE INDEX IF NOT EXISTS idx_chart_meta_md5_source "
      "ON chart_meta(md5, source_priority, source_archive_size, path)",
  };
  for (const auto *indexQuery : indexes) {
    if (!execSql(db, indexQuery, "creating chart meta index")) {
      return false;
    }
  }
  return createFavoritesTable(db);
}

static bool createFavoritesTable(sqlite3 *db) {
  const char *query =
      "CREATE TABLE IF NOT EXISTS chart_favorites ("
      "chart_path TEXT PRIMARY KEY,"
      "chart_md5 TEXT NOT NULL DEFAULT '',"
      "chart_sha256 TEXT NOT NULL DEFAULT '',"
      "added_at TEXT DEFAULT CURRENT_TIMESTAMP"
      ")";
  if (!execSql(db, query, "creating chart favorite table")) {
    return false;
  }

  const char *indexes[] = {
      "CREATE INDEX IF NOT EXISTS idx_chart_favorites_added_at "
      "ON chart_favorites(added_at)",
      "CREATE INDEX IF NOT EXISTS idx_chart_favorites_md5 "
      "ON chart_favorites(chart_md5)",
      "CREATE INDEX IF NOT EXISTS idx_chart_favorites_sha256 "
      "ON chart_favorites(chart_sha256)",
  };
  for (const auto *indexQuery : indexes) {
    if (!execSql(db, indexQuery, "creating chart favorite index")) {
      return false;
    }
  }

  return migrateChartDatabaseSchema(db);
}

static bool createSolidArchiveTable(sqlite3 *db) {
  const char *query =
      "CREATE TABLE IF NOT EXISTS solid_archives ("
      "path TEXT PRIMARY KEY,"
      "name TEXT NOT NULL DEFAULT '',"
      "archive_size INTEGER NOT NULL DEFAULT 0,"
      "uncompressed_size INTEGER NOT NULL DEFAULT 0,"
      "file_count INTEGER NOT NULL DEFAULT 0,"
      "mtime_ns INTEGER NOT NULL DEFAULT 0,"
      "updated_at TEXT DEFAULT CURRENT_TIMESTAMP"
      ")";
  if (!execSql(db, query, "creating solid archive table")) {
    return false;
  }

  const char *indexes[] = {
      "CREATE INDEX IF NOT EXISTS idx_solid_archives_name "
      "ON solid_archives(name)",
      "CREATE INDEX IF NOT EXISTS idx_solid_archives_name_path "
      "ON solid_archives(name COLLATE NOCASE, path)",
      "CREATE INDEX IF NOT EXISTS idx_solid_archives_size "
      "ON solid_archives(uncompressed_size)",
  };
  for (const auto *indexQuery : indexes) {
    if (!execSql(db, indexQuery, "creating solid archive index")) {
      return false;
    }
  }
  return true;
}

static bool createChartStateTables(sqlite3 *db) {
  if (db == nullptr) {
    return false;
  }

  bool ok = true;
  ok = createChartMetadataRebuildStateTable(db) && ok;
  ok = createChartScanCheckpointTable(db) && ok;
  ok = createArchiveScanCacheTable(db) && ok;
  return ok;
}

bool ChartRepository::DeleteChartMeta(sqlite3 *db, std::filesystem::path path) {
  // std::cout << "Deleting chart: " << path.string() << std::endl;
  chart_storage_identity::ToRelativePath(path);
  auto query = "DELETE FROM chart_meta WHERE path = @path";
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt,
                                    "preparing statement to delete a chart",
                                    logSqlErrorText)) {
    return false;
  }
  const auto target = fspath_to_utf8(path);
  SDL_Log("Deleting chart: %s", target.c_str());
  bindSqliteText(stmt, 1, target);
  const int rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE) {
    logSqlError("deleting a chart", db);
    return false;
  }
  if (sqlite3_changes(db) > 0) {
    chart_repository_detail::BumpLibraryRevision();
  }
  return true;
}

int ChartRepository::DeleteChartMetaInDirectory(
    sqlite3 *db, const std::filesystem::path &directory) {
  if (directory.empty()) {
    return -1;
  }

  createSolidArchiveTable(db);
  createArchiveScanCacheTable(db);
  createChartScanCheckpointTable(db);

  std::filesystem::path targetDirectory = directory;
  chart_storage_identity::ToAbsolutePath(targetDirectory);
  const std::filesystem::path normalizedTarget =
      targetDirectory.lexically_normal();
  auto matchesTarget = [&](const std::filesystem::path &path) {
    return path.lexically_normal() == normalizedTarget ||
           pathIsInsideDirectory(path, targetDirectory);
  };

  std::vector<bms_parser::ChartMeta> chartMetas;
  chart_repository_detail::SelectAllChartMeta(db, chartMetas);

  auto query = "DELETE FROM chart_meta WHERE path = @path";
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(
          db, query, stmt, "preparing statement to delete charts in directory",
          logSqlErrorText)) {
    return -1;
  }

  int deletedCount = 0;
  for (const auto &chartMeta : chartMetas) {
    if (!matchesTarget(chartMeta.BmsPath)) {
      continue;
    }

    const std::string target =
        chart_storage_identity::StoredPathText(chartMeta.BmsPath);
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    bindSqliteText(stmt, 1, target);
    const int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
      logSqlError("deleting a chart in directory", db);
      return -1;
    }
    if (sqlite3_changes(db) > 0) {
      ++deletedCount;
    }
  }

  for (const auto &solidArchivePath : selectSolidArchivePaths(db)) {
    if (!matchesTarget(solidArchivePath)) {
      continue;
    }
    if (deleteSolidArchive(db, solidArchivePath)) {
      ++deletedCount;
    }
  }
  for (const auto &archiveCachePath : selectArchiveScanCachePaths(db)) {
    if (!matchesTarget(archiveCachePath)) {
      continue;
    }
    if (deleteArchiveScanCache(db, archiveCachePath)) {
      ++deletedCount;
    }
  }
  if (deletedCount > 0) {
    clearChartScanCheckpoint(db);
    chart_repository_detail::BumpLibraryRevision();
  }
  return deletedCount;
}

bool ChartRepository::DeleteArchiveRecords(
    sqlite3 *db, const std::filesystem::path &archivePath) {
  if (db == nullptr || archivePath.empty()) {
    return false;
  }

  createChartMetaTable(db);
  createSolidArchiveTable(db);
  createArchiveScanCacheTable(db);
  createChartScanCheckpointTable(db);

  int changedCount = 0;
  std::string transactionError;
  SqliteTransactionHandle transaction(db, "BEGIN", transactionError);
  if (!transaction.active()) {
    SDL_Log("Failed to start archive record delete transaction: %s",
            transactionError.c_str());
    return false;
  }
  if (deleteChartMetaInArchive(db, archivePath)) {
    ++changedCount;
  }
  if (deleteSolidArchive(db, archivePath)) {
    ++changedCount;
  }
  if (deleteArchiveScanCache(db, archivePath)) {
    ++changedCount;
  }
  clearChartScanCheckpoint(db);
  if (!transaction.commit(transactionError)) {
    SDL_Log("Failed to commit archive record delete transaction: %s",
            transactionError.c_str());
    return false;
  }

  if (changedCount > 0) {
    chart_repository_detail::BumpLibraryRevision();
  }
  return changedCount > 0;
}

bool ChartRepository::ClearChartMeta(sqlite3 *db) {
  createSolidArchiveTable(db);
  createArchiveScanCacheTable(db);
  createChartScanCheckpointTable(db);

  auto query = "DELETE FROM chart_meta";
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt, "clearing",
                                    logSqlErrorText)) {
    return false;
  }
  int rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE) {

    logSqlError("clearing", db);
    return false;
  }
  const int chartChanges = sqlite3_changes(db);
  bool changed = chartChanges > 0;
  if (!execSql(db, "DELETE FROM solid_archives", "clearing solid archives")) {
    return false;
  }
  changed = sqlite3_changes(db) > 0 || changed;
  if (!execSql(db, "DELETE FROM archive_scan_cache",
               "clearing archive scan cache")) {
    return false;
  }
  changed = sqlite3_changes(db) > 0 || changed;
  if (!execSql(db, "DELETE FROM chart_scan_checkpoint",
               "clearing chart scan checkpoint")) {
    return false;
  }
  changed = sqlite3_changes(db) > 0 || changed;
  if (changed) {
    chart_repository_detail::BumpLibraryRevision();
  }
  return true;
}

static bool createEntriesTable(sqlite3 *db) {
  // save paths to search for charts
  if (!execSql(db,
               "CREATE TABLE IF NOT EXISTS entries ("
               "path       TEXT primary key,"
               "ios_bookmark TEXT DEFAULT ''"
               ")",
               "creating entries table")) {
    return false;
  }

  if (!execSqlAllowDuplicateColumn(
          db, "ALTER TABLE entries ADD COLUMN ios_bookmark TEXT DEFAULT ''",
          "migrating entries table")) {
    return false;
  }
  return true;
}

bool chart_repository_detail::EnsureCoreSchema(sqlite3 *database) {
  bool ok = true;
  ok = createChartMetaTable(database) && ok;
  ok = createSolidArchiveTable(database) && ok;
  ok = createFavoritesTable(database) && ok;
  ok = createEntriesTable(database) && ok;
  ok = createChartStateTables(database) && ok;
  return ok;
}

bool ChartRepository::InsertEntry(sqlite3 *db,
                                const std::filesystem::path &path,
                                const std::string &iosBookmark) {
  createChartScanCheckpointTable(db);
  auto query = "REPLACE INTO entries ("
               "path,"
               "ios_bookmark"
               ") VALUES("
               "@path,"
               "@ios_bookmark"
               ")";
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt,
                                    "preparing statement to insert an entry",
                                    logSqlErrorText)) {
    return false;
  }
  const std::string pathText = chart_storage_identity::StoredPathText(path);
  bindSqliteText(stmt, 1, pathText);
  bindSqliteText(stmt, 2, iosBookmark);
  int rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE) {
    logSqlError("inserting an entry", db);
    return false;
  }
  clearChartScanCheckpoint(db);
  chart_repository_detail::BumpLibraryRevision();
  return true;
}

std::vector<ChartEntry> ChartRepository::SelectAllEntries(sqlite3 *db) {
  auto query = "SELECT "
               "path,"
               "COALESCE(ios_bookmark, '')"
               " FROM entries";
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt, "getting all entries",
                                    logSqlErrorText)) {
    return std::vector<ChartEntry>();
  }
  std::vector<ChartEntry> entries;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    ChartEntry entry;
    std::filesystem::path path(readStoredPath(stmt, 0));
    if (!path.empty()) {
      chart_storage_identity::ToAbsolutePath(path);
    }
    entry.path = fspath_to_path_t(path);
    entry.iosBookmark = sqliteColumnString(stmt, 1);
#if TARGET_OS_ANDROID
    RegisterAndroidChartFolder(path, entry.iosBookmark);
#endif
    entries.push_back(std::move(entry));
  }
  return entries;
}

std::filesystem::path ChartRepository::DefaultBmsFolderPath() {
  return Utils::GetDocumentsPath("BMS");
}

bool ChartRepository::IsDefaultBmsFolderPath(
    const std::filesystem::path &path) {
#if TARGET_OS_ANDROID
  if (path.empty()) {
    return false;
  }
  return path.lexically_normal() == DefaultBmsFolderPath().lexically_normal();
#else
  (void)path;
  return false;
#endif
}

std::vector<ChartEntry> ChartRepository::SelectEffectiveEntries(sqlite3 *db) {
  auto entries = SelectAllEntries(db);

#if TARGET_OS_ANDROID
  const auto defaultPath = DefaultBmsFolderPath();
  std::error_code errorCode;
  if (!Utils::EnsureDirectoryExists(defaultPath, errorCode)) {
    SDL_Log("Failed to create default BMS folder %s: %s",
            fspath_to_utf8(defaultPath).c_str(), errorCode.message().c_str());
  }

  bool hasDefaultEntry = false;
  for (auto &entry : entries) {
    if (IsDefaultBmsFolderPath(std::filesystem::path(entry.path))) {
      entry.removable = false;
      hasDefaultEntry = true;
    }
  }

  if (!hasDefaultEntry) {
    entries.push_back({
        .path = fspath_to_path_t(defaultPath),
        .iosBookmark = "",
        .removable = false,
    });
  }
#endif

  return entries;
}

bool ChartRepository::DeleteEntry(sqlite3 *db,
                                const std::filesystem::path &path) {
  createChartScanCheckpointTable(db);
  auto query = "DELETE FROM entries WHERE path = @path";
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt,
                                    "preparing statement to delete an entry",
                                    logSqlErrorText)) {
    return false;
  }
  const std::string pathText = chart_storage_identity::StoredPathText(path);
  bindSqliteText(stmt, 1, pathText);
  int rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE) {
    logSqlError("deleting an entry", db);
    return false;
  }
  if (sqlite3_changes(db) > 0) {
    clearChartScanCheckpoint(db);
    chart_repository_detail::BumpLibraryRevision();
  }
  return true;
}

bool ChartRepository::ClearEntries(sqlite3 *db) {
  createChartScanCheckpointTable(db);
  auto query = "DELETE FROM entries";
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt, "clearing",
                                    logSqlErrorText)) {
    return false;
  }
  int rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE) {
    logSqlError("clearing", db);
    return false;
  }
  if (sqlite3_changes(db) > 0) {
    clearChartScanCheckpoint(db);
    chart_repository_detail::BumpLibraryRevision();
  }
  return true;
}

bool ChartRepository::ImportDifficultyTable(sqlite3 *db,
                                          const std::string &headerJson,
                                          const std::string &dataJson,
                                          const std::string &sourceUrl) {
  if (!chart_repository_detail::EnsureDifficultySchema(db)) {
    return false;
  }

  json header;
  json data;
  try {
    header = json::parse(headerJson);
    data = dataJson.empty() ? json::array() : json::parse(dataJson);
  } catch (const std::exception &e) {
    SDL_Log("Failed to parse difficulty table JSON from %s: %s",
            sourceUrl.c_str(), e.what());
    return false;
  }

  const std::string name = jsonStringAt(header, "name");
  const std::string symbol = jsonStringAt(header, "symbol");
  const std::string dataUrl = jsonStringAt(header, "data_url");
  if (name.empty() || symbol.empty()) {
    SDL_Log("Skipping difficulty table with missing name or symbol: %s",
            sourceUrl.c_str());
    return false;
  }

  chart_repository_detail::InvalidateDifficultyLabelCache();
  std::string transactionError;
  SqliteTransactionHandle transaction(db, "BEGIN", transactionError);
  if (!transaction.active()) {
    SDL_Log("Failed to start difficulty table import transaction: %s",
            transactionError.c_str());
    return false;
  }
  RetainedDifficultyCourses retainedCourses;
  const int tableId = upsertDifficultyTable(db, name, symbol, dataUrl,
                                            sourceUrl, retainedCourses);
  if (tableId <= 0) {
    return false;
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

  std::vector<TableChartItem> chartItems;
  if (charts != nullptr) {
    for (const auto &chartValue : *charts) {
      if (!chartValue.is_object()) {
        continue;
      }
      auto chart = readChartItem(chartValue, "");
      if (chart.md5.empty() && chart.sha256.empty()) {
        continue;
      }
      chartItems.push_back(std::move(chart));
    }
  }

  const auto levelOrder = readLevelOrder(header);
  if (!levelOrder.empty()) {
    std::stable_sort(chartItems.begin(), chartItems.end(),
                     [&levelOrder](const TableChartItem &a,
                                   const TableChartItem &b) {
                       return levelOrderFor(levelOrder, a.level) <
                              levelOrderFor(levelOrder, b.level);
                     });
  }

  int sortOrder = 0;
  for (const auto &chart : chartItems) {
    if (!insertDifficultyTableEntry(db, tableId, chart, sortOrder++)) {
      return false;
    }
  }
  TableChartItemLookup chartLookup;
  chartLookup.reserve(chartItems.size() * 2);
  ChartHashEvidenceLookup chartHashEvidence;
  chartHashEvidence.reserve(chartItems.size() * 2);
  for (const auto &chart : chartItems) {
    addToChartItemLookup(chartLookup, chart);
    addToChartHashEvidenceLookup(chartHashEvidence, chart);
  }

  std::vector<const json *> courses;
  const auto courseIt = header.find("course");
  if (courseIt != header.end()) {
    collectCourses(*courseIt, courses);
  }

  int courseSortOrder = 0;
  for (const auto *course : courses) {
    const std::string courseName = jsonStringAt(*course, "name");
    if (courseName.empty()) {
      continue;
    }

    const auto [groupName, level] =
        splitCourseFolderAndLevel(courseName, symbol);
    std::string constraintJson = "[]";
    const auto constraintIt = course->find("constraint");
    if (constraintIt != course->end()) {
      constraintJson = constraintIt->dump();
    }

    auto courseCharts = readCourseCharts(*course);
    std::vector<TableChartItem> storedCourseCharts;
    storedCourseCharts.reserve(courseCharts.size());
    for (auto &chart : courseCharts) {
      if (chart.md5.empty() && chart.sha256.empty()) {
        continue;
      }
      if (const auto *tableChart = findChartItemInLookup(chartLookup, chart)) {
        fillMissingCourseChartMetadata(chart, *tableChart);
      }
      fillUnambiguousCourseChartHash(chart, chartHashEvidence);
      storedCourseCharts.push_back(std::move(chart));
    }

    course_identity::Definition importedDefinition{
        .name = courseName,
        .groupName = groupName,
        .constraintJson = constraintJson,
        .charts = courseChartIdentities(storedCourseCharts),
    };
    const auto retainedCourse =
        takeRetainedDifficultyCourse(retainedCourses, importedDefinition);
    const int retainedCourseId =
        retainedCourse.has_value() ? retainedCourse->courseId : 0;
    std::string courseKey = course_identity::makeCourseKey(
        importedDefinition.charts, importedDefinition.constraintJson);
    if (retainedCourse.has_value() && !retainedCourse->courseKey.empty()) {
      courseKey = retainedCourse->courseKey;
    }

    const int courseId = insertDifficultyCourse(
        db, tableId, courseName, groupName, level, constraintJson,
        courseKey, courseSortOrder++, retainedCourseId);
    if (courseId <= 0) {
      return false;
    }

    int chartSortOrder = 0;
    for (const auto &chart : storedCourseCharts) {
      if (!insertDifficultyCourseEntry(db, courseId, chart, chartSortOrder++)) {
        return false;
      }
    }
  }

  if (!transaction.commit(transactionError)) {
    SDL_Log("Failed to commit difficulty table import transaction: %s",
            transactionError.c_str());
    return false;
  }
  chart_repository_detail::BumpLibraryRevision();
  SDL_Log("Imported difficulty table %s (%s) from %s", name.c_str(),
          symbol.c_str(), sourceUrl.c_str());
  return true;
}

bool ChartRepository::ImportDifficultyTableFromUrl(sqlite3 *db,
                                                 const std::string &pageUrl,
                                                 std::string *errorMessage,
                                                 DifficultyTableImportProgressCallback
                                                     progressCallback) {
  const std::string trimmedUrl = trimCopy(pageUrl);
  if (trimmedUrl.empty()) {
    if (errorMessage != nullptr) {
      *errorMessage = "Table URL is empty";
    }
    return false;
  }
  if (!trimmedUrl.starts_with("http://") &&
      !trimmedUrl.starts_with("https://")) {
    if (errorMessage != nullptr) {
      *errorMessage = "Table URL must start with http:// or https://";
    }
    return false;
  }

  auto pageBody = fetchUrlText(trimmedUrl, errorMessage);
  if (!pageBody) {
    return false;
  }

  std::string headerUrl;
  std::string headerJsonText;

  try {
    json maybeHeader = json::parse(*pageBody);
    const auto tableEntries =
        readDifficultyTableListEntries(maybeHeader, trimmedUrl);
    if (!tableEntries.empty()) {
      struct InFlightDownload {
        size_t index = 0;
        DifficultyTableListEntry entry;
        std::future<DifficultyTableDownloadResult> future;
      };

      int imported = 0;
      int failed = 0;
      int skipped = 0;
      int completed = 0;
      std::string firstError;
      size_t nextIndex = 0;
      std::vector<InFlightDownload> inFlight;
      inFlight.reserve(kMaxConcurrentDifficultyTableDownloads);

      const auto total = static_cast<int>(tableEntries.size());
      if (progressCallback) {
        progressCallback({0, total, "Preparing table downloads"});
      }

      auto markSkipped = [&](const DifficultyTableListEntry &table,
                             const std::string &reason) {
        skipped++;
        completed++;
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
        while (nextIndex < tableEntries.size() &&
               inFlight.size() < kMaxConcurrentDifficultyTableDownloads) {
          const auto &table = tableEntries[nextIndex];
          const auto &tableUrl = table.url;
          if (tableUrl == trimmedUrl) {
            markSkipped(table, "Skipped recursive table list URL");
            nextIndex++;
            continue;
          }

          if (findDifficultyTableBySourceUrl(db, tableUrl) > 0) {
            markSkipped(table, "");
            nextIndex++;
            continue;
          }

          if (progressCallback) {
            progressCallback(
                {completed, total,
                 "Fetching: " + (table.name.empty() ? table.url : table.name)});
          }
          inFlight.push_back(
              {nextIndex, table,
               std::async(std::launch::async, [tableUrl]() {
                 return downloadDifficultyTablePayload(tableUrl);
               })});
          nextIndex++;
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
          if (result.success) {
            if (progressCallback) {
              progressCallback(
                  {completed, total, "Importing: " + displayName});
            }
            if (ImportDifficultyTable(db, result.headerJson, result.dataJson,
                                      result.sourceUrl)) {
              imported++;
            } else {
              failed++;
              if (firstError.empty()) {
                firstError = result.sourceUrl +
                             ": downloaded table data could not be imported";
              }
            }
          } else {
            failed++;
            if (firstError.empty()) {
              firstError = it->entry.url +
                           (result.errorMessage.empty()
                                ? ""
                                : ": " + result.errorMessage);
            }
          }

          completed++;
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
          *errorMessage = "Imported " + std::to_string(imported) + ", skipped " +
                          std::to_string(skipped) + " of " +
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

    if (maybeHeader.is_object() && maybeHeader.contains("name") &&
        maybeHeader.contains("symbol") && maybeHeader.contains("data_url")) {
      headerUrl = trimmedUrl;
      headerJsonText = *pageBody;
    }
  } catch (...) {
  }

  if (headerJsonText.empty()) {
    auto discoveredHeaderUrl = findBmstableHeaderUrl(*pageBody, trimmedUrl);
    if (!discoveredHeaderUrl) {
      if (errorMessage != nullptr) {
        *errorMessage =
            "Could not find a bmstable header link in the table webpage";
      }
      return false;
    }
    headerUrl = *discoveredHeaderUrl;
    auto headerBody = fetchUrlText(headerUrl, errorMessage);
    if (!headerBody) {
      return false;
    }
    headerJsonText = *headerBody;
  }

  json header;
  try {
    header = json::parse(headerJsonText);
  } catch (const std::exception &e) {
    if (errorMessage != nullptr) {
      *errorMessage =
          std::string("Failed to parse table header JSON: ") + e.what();
    }
    return false;
  }

  const std::string dataUrl = jsonStringAt(header, "data_url");
  if (dataUrl.empty()) {
    if (errorMessage != nullptr) {
      *errorMessage = "Table header does not contain data_url";
    }
    return false;
  }

  if (progressCallback) {
    const std::string name = jsonStringAt(header, "name", trimmedUrl);
    progressCallback({1, 1, name.empty() ? trimmedUrl : name});
  }

  const std::string resolvedDataUrl = resolveUrl(headerUrl, dataUrl);
  auto dataBody = fetchUrlText(resolvedDataUrl, errorMessage);
  if (!dataBody) {
    return false;
  }

  if (!ImportDifficultyTable(db, headerJsonText, *dataBody, trimmedUrl)) {
    if (errorMessage != nullptr) {
      *errorMessage = "Downloaded table data could not be imported";
    }
    return false;
  }
  return true;
}

bool ChartRepository::UpdateDifficultyTableFromSourceUrl(
    sqlite3 *db, int tableId, std::string *errorMessage) {
  if (!chart_repository_detail::EnsureDifficultySchema(db)) {
    if (errorMessage != nullptr) {
      *errorMessage = "Could not prepare difficulty table database";
    }
    return false;
  }

  std::string sourceUrl;
  if (!readDifficultyTableSourceUrl(db, tableId, sourceUrl, errorMessage)) {
    return false;
  }

  const std::string trimmedUrl = trimCopy(sourceUrl);
  if (!trimmedUrl.starts_with("http://") &&
      !trimmedUrl.starts_with("https://")) {
    if (errorMessage != nullptr) {
      *errorMessage = "Difficulty table does not have an updateable source URL";
    }
    return false;
  }

  return ImportDifficultyTableFromUrl(db, trimmedUrl, errorMessage);
}

int ChartRepository::ImportDifficultyTablesFromDirectory(
    sqlite3 *db, const std::filesystem::path &directory) {
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  if (ec) {
    SDL_Log("Failed to create difficulty table directory %s: %s",
            fspath_to_utf8(directory).c_str(), ec.message().c_str());
    return 0;
  }

  int imported = 0;
  std::unordered_set<std::string> importedHeaders;
  for (std::filesystem::recursive_directory_iterator it(directory, ec), end;
       !ec && it != end; it.increment(ec)) {
    std::error_code typeEc;
    if (!it->is_regular_file(typeEc)) {
      if (typeEc) {
        SDL_Log("Failed to read difficulty table path type %s: %s",
                fspath_to_utf8(it->path()).c_str(), typeEc.message().c_str());
      }
      continue;
    }
    const auto &path = it->path();
    if (path.extension() != ".json") {
      continue;
    }

    const auto raw = readTextFile(path);
    if (!raw) {
      continue;
    }

    json document;
    try {
      document = json::parse(*raw);
    } catch (...) {
      continue;
    }

    if (document.is_object() && document.contains("header") &&
        (document.contains("data") || document.contains("charts"))) {
      json header = document["header"];
      json data = document.contains("data")
                      ? document["data"]
                      : json{{"charts", document["charts"]}};
      if (ImportDifficultyTable(db, header.dump(), data.dump(),
                                fspath_to_utf8(path))) {
        imported++;
      }
      continue;
    }

    if (!document.is_object() || !document.contains("name") ||
        !document.contains("symbol") || !document.contains("data_url")) {
      continue;
    }

    std::error_code canonicalEc;
    const auto canonicalPath =
        std::filesystem::weakly_canonical(path, canonicalEc);
    const std::filesystem::path headerPath =
        canonicalEc ? path.lexically_normal() : canonicalPath;
    const std::string headerKey = fspath_to_utf8(headerPath);
    if (canonicalEc) {
      SDL_Log("Failed to canonicalize difficulty table header %s: %s",
              fspath_to_utf8(path).c_str(), canonicalEc.message().c_str());
    }
    if (importedHeaders.find(headerKey) != importedHeaders.end()) {
      continue;
    }
    importedHeaders.insert(headerKey);

    const std::string dataUrl = jsonStringAt(document, "data_url");
    std::filesystem::path dataPath = path.parent_path() / dataUrl;
    std::error_code dataPathError;
    if (!std::filesystem::exists(dataPath, dataPathError) || dataPathError) {
      continue;
    }
    const auto dataRaw = readTextFile(dataPath);
    if (!dataRaw) {
      continue;
    }
    if (ImportDifficultyTable(db, *raw, *dataRaw, fspath_to_utf8(path))) {
      imported++;
    }
  }

  if (ec) {
    SDL_Log("Failed while scanning difficulty table directory %s: %s",
            fspath_to_utf8(directory).c_str(), ec.message().c_str());
  }
  return imported;
}

ChartRepository::ChartRepository()
    : ChartRepository(Utils::GetDocumentsPath("db") / "chart.db") {}

ChartRepository::ChartRepository(std::filesystem::path databasePath)
    : impl_(std::make_unique<Impl>(std::move(databasePath))) {
  chart_storage_identity::ConfigureArchiveCachePathNormalization();
}

ChartRepository::~ChartRepository() = default;
