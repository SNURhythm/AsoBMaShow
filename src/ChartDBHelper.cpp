// Fill out your copyright notice in the Description page of Project Settings.

#include "ChartDBHelper.h"
#include "ArchiveFile.h"
#include "BmsChartFile.h"
#include "BmsMetadataText.h"
#include "ChartMetaSql.h"
#include "ChartSqlExpressions.h"
#include "LongNoteModeUtils.h"
#include "SqliteRAII.h"
#include "Utils.h"
#include <SDL2/SDL.h>
#include "path.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <future>
#include <fstream>
#include <iostream>
#include "../yoga/lib/nlohmann/json.hpp"
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <sstream>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include "targets.h"
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
using asobmshow::chart_sql::chartArtworkOrderBy;
using asobmshow::chart_sql::chartSourceOrderBy;
using asobmshow::chart_sql::kChartMetaColumnCount;
using asobmshow::chart_sql::kChartMetaSelectColumns;
using asobmshow::chart_sql::preferredChartPredicate;

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
constexpr int kArchiveParseCheckpointInterval = 100;
constexpr int kIndividualParseCheckpointInterval = 1000;
constexpr const char *kScanCheckpointPhaseIndividual = "individual";
constexpr const char *kScanCheckpointPhaseArchive = "archive";
constexpr int kChartDatabaseSchemaVersion = 1;
constexpr const char *kChartFavoriteColumn =
    "CASE WHEN EXISTS (SELECT 1 FROM chart_favorites cf WHERE "
    "cf.chart_path = cm.path) THEN 1 ELSE 0 END";

struct DifficultyLabelCache {
  bool loaded = false;
  std::unordered_map<std::string, std::vector<std::string>> labelsBySha256;
  std::unordered_map<std::string, std::vector<std::string>> labelsByMd5;
};

std::mutex gDifficultyLabelCacheMutex;
DifficultyLabelCache gDifficultyLabelCache;
std::atomic<std::uint64_t> gLibraryRevision{1};

std::string columnString(sqlite3_stmt *stmt, int idx);

void bumpLibraryRevision() {
  gLibraryRevision.fetch_add(1, std::memory_order_relaxed);
}

using asobmshow::bms_metadata::lowerCopy;
using asobmshow::bms_metadata::normalizedHash;
using asobmshow::bms_metadata::trimCopy;

std::string storedChartPathText(std::filesystem::path path) {
  if (path.empty()) {
    return "";
  }
  ChartDBHelper::ToRelativePath(path);
  path = path.lexically_normal();
  return fspath_to_utf8(path);
}

std::string normalizedDifficultyText(const std::string &difficultyText) {
  return lowerCopy(trimCopy(difficultyText));
}

void bindDifficultySearchText(sqlite3_stmt *stmt, int &bindIndex,
                              const std::string &difficultyText) {
  const std::string difficulty = normalizedDifficultyText(difficultyText);
  bindSqliteText(stmt, bindIndex++, difficulty);
  bindSqliteText(stmt, bindIndex++, "%" + difficulty + "%");
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

#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
std::optional<std::filesystem::path>
relativeToCurrentDocumentsPath(const std::filesystem::path &path) {
  if (path.empty() || !path.is_absolute()) {
    return std::nullopt;
  }

  const std::filesystem::path documentsRoot =
      Utils::GetDocumentsPath().lexically_normal();
  const std::filesystem::path normalizedPath = path.lexically_normal();
  const std::string rootText = documentsRoot.generic_string();
  const std::string pathText = normalizedPath.generic_string();
  const std::string rootPrefix = rootText + "/";

  if (pathText == rootText) {
    return std::filesystem::path();
  }
  if (pathText.starts_with(rootPrefix)) {
    return std::filesystem::path(pathText.substr(rootPrefix.size()));
  }
  return std::nullopt;
}

std::filesystem::path
storedDocumentsPath(const std::filesystem::path &relativeToDocuments) {
  std::filesystem::path stored("Documents");
  if (!relativeToDocuments.empty()) {
    stored /= relativeToDocuments;
  }
  return stored;
}

std::optional<std::filesystem::path>
relativeFromStoredDocumentsPath(const std::filesystem::path &path) {
  if (path.empty() || path.is_absolute()) {
    return std::nullopt;
  }

  auto it = path.begin();
  if (it == path.end() || *it != std::filesystem::path("Documents")) {
    return std::nullopt;
  }
  ++it;

  std::filesystem::path relative;
  for (; it != path.end(); ++it) {
    relative /= *it;
  }
  return relative;
}
#endif

bool stopRequested(const std::stop_token *stopToken) {
  return stopToken != nullptr && stopToken->stop_requested();
}

bool hashLooksComplete(const std::string &value, size_t expectedLength) {
  if (value.size() != expectedLength) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
    return std::isxdigit(ch) != 0;
  });
}

bool parsedChartMetaHasStableIdentity(const bms_parser::ChartMeta &meta) {
  const std::string md5 = normalizedHash(meta.MD5);
  const std::string sha256 = normalizedHash(meta.SHA256);
  return !meta.BmsPath.empty() && hashLooksComplete(md5, 32) &&
         hashLooksComplete(sha256, 64);
}

bool parsedChartLooksInsertable(const bms_parser::Chart &chart) {
  return parsedChartMetaHasStableIdentity(chart.Meta) &&
         !chart.Measures.empty();
}

struct ArchiveScanResult {
  bool readable = false;
  bool solid = false;
  int fileCount = 0;
  std::uint64_t uncompressedSize = 0;
  std::vector<std::filesystem::path> chartPaths;
};

ArchiveScanResult scanArchiveForChartsOrSolid(
    const std::filesystem::path &archivePath,
    std::unordered_set<path_t> &knownChartPaths,
    const ChartScanPauseCallback &pauseCallback) {
  auto pauseIfNeeded = [&]() {
    return pauseCallback == nullptr || pauseCallback();
  };
  ArchiveScanResult result;
  std::vector<archive_file::Entry> entries;
  std::string errorMessage;
  const std::string archiveText = fspath_to_utf8(archivePath);
  archive_file::appendDebugLogLine(
      "Scanning archive for BMS charts: " + archiveText);
  if (!pauseIfNeeded()) {
    return result;
  }
  if (!archive_file::listEntries(archivePath, entries, &errorMessage,
                                 pauseCallback)) {
    if (!errorMessage.empty()) {
      SDL_Log("Failed to scan archive %s: %s",
              archiveText.c_str(), errorMessage.c_str());
      archive_file::appendDebugLogLine(
          "Failed to scan archive: " + archiveText + ": " + errorMessage);
    }
    return result;
  }
  result.readable = true;
  for (const auto &entry : entries) {
    if (!pauseIfNeeded()) {
      result.readable = false;
      return result;
    }
    if (entry.directory) {
      continue;
    }
    ++result.fileCount;
    const std::uint64_t remaining =
        std::numeric_limits<std::uint64_t>::max() - result.uncompressedSize;
    result.uncompressedSize += std::min(entry.size, remaining);
    if (entry.solid) {
      result.solid = true;
    }
  }

  if (!result.solid) {
    for (const auto &entry : entries) {
      if (!pauseIfNeeded()) {
        result.readable = false;
        return result;
      }
      if (entry.directory ||
          !asobmshow::bms_chart_file::isBmsChartPath(entry.path)) {
        continue;
      }
      const std::filesystem::path chartPath =
          archive_file::makeVirtualPath(archivePath, entry.path);
      const path_t key = fspath_to_path_t(chartPath);
      if (knownChartPaths.find(key) != knownChartPaths.end()) {
        continue;
      }
      result.chartPaths.push_back(chartPath);
      knownChartPaths.insert(key);
    }
  }

  archive_file::appendDebugLogLine(
      "Archive chart scan complete: " + archiveText +
      " charts=" + std::to_string(result.chartPaths.size()) +
      " entries=" + std::to_string(entries.size()) +
      " files=" + std::to_string(result.fileCount) +
      " solid=" + std::string(result.solid ? "yes" : "no") +
      " estimatedUnpacked=" + std::to_string(result.uncompressedSize));
  if (result.solid) {
    archive_file::appendDebugLogLine(
        "Skipped chart probing for solid archive: " + archiveText);
  }
  return result;
}

struct ArchiveScanCacheRecord {
  bool found = false;
  sqlite3_int64 archiveSize = 0;
  sqlite3_int64 mtimeNs = 0;
  bool solid = false;
  std::uint64_t uncompressedSize = 0;
  int fileCount = 0;
  int chartCount = -1;
};

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

std::string chartLookupKey(const std::string &kind, const std::string &hash) {
  return hash.empty() ? "" : kind + ":" + hash;
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

void fillMissingCourseChartMetadata(TableChartItem &courseChart,
                                    const TableChartItem &tableChart) {
  if ((courseChart.level.empty() || courseChart.level == "0") &&
      !tableChart.level.empty()) {
    courseChart.level = tableChart.level;
  }
  if (courseChart.md5.empty()) {
    courseChart.md5 = tableChart.md5;
  }
  if (courseChart.sha256.empty()) {
    courseChart.sha256 = tableChart.sha256;
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

bool queryNeedsDifficultyTableSchema(const ChartMetaQuery &query) {
  return query.tableId > 0 || query.coursesOnly || query.courseId > 0 ||
         query.courseTableId > 0 || !query.courseGroupName.empty() ||
         !query.difficultyText.empty();
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

void beginSqliteTransaction(sqlite3 *db, const char *context) {
  if (const auto error = executeSqlite(db, "BEGIN")) {
    SDL_Log("Failed to begin %s transaction: %s", context, error->c_str());
  }
}

void commitSqliteTransaction(sqlite3 *db, const char *context) {
  if (const auto error = executeSqlite(db, "COMMIT")) {
    SDL_Log("Failed to commit %s transaction: %s", context, error->c_str());
  }
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
  ChartDBHelper::ToAbsolutePath(path);
  ChartDBHelper::ToRelativePath(path);
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

sqlite3_int64 clampSqlInteger(std::uint64_t value) {
  return value > static_cast<std::uint64_t>(
                     std::numeric_limits<sqlite3_int64>::max())
             ? std::numeric_limits<sqlite3_int64>::max()
             : static_cast<sqlite3_int64>(value);
}

std::string scoreLongNoteModeExpr(const std::string &alias,
                                  int selectedLongNoteMode) {
  const std::string selected =
      std::to_string(
          long_note_mode::normalizeSelectedValue(selectedLongNoteMode));
  const std::string lnModeExpr = "COALESCE(" + alias + ".ln_mode, 0)";
  return "(CASE WHEN COALESCE(" + alias + ".total_long_notes, 0) + COALESCE(" +
         alias + ".total_backspin_notes, 0) <= 0 THEN 0 "
         "WHEN " +
         long_note_mode::sqlValidValuePredicate(lnModeExpr) + " THEN " +
         alias + ".ln_mode ELSE " + selected + " END)";
}

std::string scoreRankLookupExpr(const std::string &kind,
                                const std::string &keyExpr,
                                const std::string &lnModeExpr) {
  return "(SELECT scr.rank FROM temp.score_clear_rank_cache scr WHERE "
         "scr.kind = " +
         kind + " AND scr.key = " + keyExpr + " AND scr.ln_mode = " +
         lnModeExpr + " LIMIT 1)";
}

std::string chartClearMarkPredicate(const std::string &alias,
                                    int selectedLongNoteMode) {
  const std::string lnModeExpr =
      scoreLongNoteModeExpr(alias, selectedLongNoteMode);
  return "COALESCE(" + scoreRankLookupExpr("0", alias + ".sha256", lnModeExpr) +
         ", " + scoreRankLookupExpr("1", alias + ".md5", lnModeExpr) + ", " +
         scoreRankLookupExpr("2", alias + ".path", lnModeExpr) + ", " +
         std::to_string(kNoPlayClearMarkRank) + ") = @clear_mark_rank";
}

std::string difficultyEntryClearMarkPredicate(const std::string &entryAlias,
                                             const std::string &chartAlias,
                                             int selectedLongNoteMode) {
  const std::string lnModeExpr =
      scoreLongNoteModeExpr(chartAlias, selectedLongNoteMode);
  return "COALESCE(" +
         scoreRankLookupExpr("0", entryAlias + ".sha256", lnModeExpr) + ", " +
         scoreRankLookupExpr("1", entryAlias + ".md5", lnModeExpr) + ", " +
         std::to_string(kNoPlayClearMarkRank) + ") = @clear_mark_rank";
}

bool chartMetaQueryHasCourseFilter(const ChartMetaQuery &chartQuery) {
  return chartQuery.coursesOnly || chartQuery.courseId > 0 ||
         !chartQuery.courseGroupName.empty();
}

void appendChartMetaFilters(std::string &query,
                            const ChartMetaQuery &chartQuery) {
  if (!chartQuery.keyword.empty()) {
    query += " AND rtrim(cm.title || ' ' || cm.subtitle || ' ' || cm.artist || "
             "' ' || cm.sub_artist || ' ' || cm.genre) LIKE @text";
  }

  if (chartQuery.tableId > 0) {
    query += " AND (cm.sha256 IN (SELECT dte.sha256 FROM "
             "difficulty_table_entries dte "
             "WHERE dte.table_id = @table_id AND dte.sha256 != ''";
    if (!chartQuery.tableLevel.empty()) {
      query += " AND dte.level = @table_level";
    }
    query += ") OR cm.md5 IN (SELECT dte.md5 FROM difficulty_table_entries dte "
             "WHERE dte.table_id = @table_id AND dte.md5 != ''";
    if (!chartQuery.tableLevel.empty()) {
      query += " AND dte.level = @table_level";
    }
    query += "))";
  }

  if (chartMetaQueryHasCourseFilter(chartQuery)) {
    query += " AND (cm.sha256 IN (SELECT dce.sha256 FROM "
             "difficulty_course_entries dce "
             "JOIN difficulty_courses dc ON dc.id = dce.course_id "
             "WHERE dce.sha256 != ''";
    if (chartQuery.courseId > 0) {
      query += " AND dce.course_id = @course_id";
    }
    if (chartQuery.courseTableId > 0) {
      query += " AND dc.table_id = @course_table_id";
    }
    if (!chartQuery.courseGroupName.empty()) {
      query += " AND dc.group_name = @course_group_name";
    }
    query +=
        ") OR cm.md5 IN (SELECT dce.md5 FROM difficulty_course_entries dce "
        "JOIN difficulty_courses dc ON dc.id = dce.course_id "
        "WHERE dce.md5 != ''";
    if (chartQuery.courseId > 0) {
      query += " AND dce.course_id = @course_id";
    }
    if (chartQuery.courseTableId > 0) {
      query += " AND dc.table_id = @course_table_id";
    }
    if (!chartQuery.courseGroupName.empty()) {
      query += " AND dc.group_name = @course_group_name";
    }
    query += "))";
  }

  if (!chartQuery.difficultyText.empty()) {
    const char *difficultyClause =
        "AND (lower(dt_filter.symbol || dte_filter.level) = @difficulty "
        "OR lower(dte_filter.level) = @difficulty "
        "OR lower(dt_filter.name || ' ' || dt_filter.symbol || "
        "dte_filter.level) LIKE @difficulty_like "
        "OR lower(dt_filter.name || ' ' || dte_filter.level) LIKE "
        "@difficulty_like)";
    query += " AND (cm.sha256 IN (SELECT dte_filter.sha256 FROM "
             "difficulty_table_entries dte_filter "
             "JOIN difficulty_tables dt_filter ON dt_filter.id = "
             "dte_filter.table_id "
             "WHERE dte_filter.sha256 != '' ";
    query += difficultyClause;
    query += ") OR cm.md5 IN (SELECT dte_filter.md5 FROM "
             "difficulty_table_entries dte_filter "
             "JOIN difficulty_tables dt_filter ON dt_filter.id = "
             "dte_filter.table_id "
             "WHERE dte_filter.md5 != '' ";
    query += difficultyClause;
    query += "))";
  }
  if (chartQuery.clearMarkFilter) {
    query += " AND ";
    query += chartClearMarkPredicate("cm", chartQuery.selectedLongNoteMode);
  }
  if (chartQuery.favoritesOnly) {
    query += " AND EXISTS (SELECT 1 FROM chart_favorites cf WHERE "
             "cf.chart_path = cm.path)";
  }

  query += " AND ";
  query += preferredChartPredicate("cm");
}

void bindChartMetaFilterParameters(sqlite3_stmt *stmt, int &bindIndex,
                                   const ChartMetaQuery &chartQuery) {
  if (!chartQuery.keyword.empty()) {
    bindSqliteText(stmt, bindIndex++, "%" + chartQuery.keyword + "%");
  }
  if (chartQuery.tableId > 0) {
    sqlite3_bind_int(stmt, bindIndex++, chartQuery.tableId);
    if (!chartQuery.tableLevel.empty()) {
      bindSqliteText(stmt, bindIndex++, chartQuery.tableLevel);
    }
  }
  if (chartMetaQueryHasCourseFilter(chartQuery)) {
    if (chartQuery.courseId > 0) {
      sqlite3_bind_int(stmt, bindIndex++, chartQuery.courseId);
    }
    if (chartQuery.courseTableId > 0) {
      sqlite3_bind_int(stmt, bindIndex++, chartQuery.courseTableId);
    }
    if (!chartQuery.courseGroupName.empty()) {
      bindSqliteText(stmt, bindIndex++, chartQuery.courseGroupName);
    }
  }
  if (!chartQuery.difficultyText.empty()) {
    bindDifficultySearchText(stmt, bindIndex, chartQuery.difficultyText);
  }
  if (chartQuery.clearMarkFilter) {
    sqlite3_bind_int(stmt, bindIndex++, chartQuery.clearMarkRank);
  }
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
  bumpLibraryRevision();
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
  };
  return runChartDatabaseMigrationPasses(
      db, kMigrationPasses,
      sizeof(kMigrationPasses) / sizeof(kMigrationPasses[0]),
      kChartDatabaseSchemaVersion);
}

bool updateChartSourcePreferenceValues(sqlite3 *db,
                                       const std::filesystem::path &chartPath,
                                       int priority,
                                       sqlite3_int64 archiveSize) {
  const std::string storedPathText = storedChartPathText(chartPath);

  const char *query =
      "UPDATE chart_meta SET source_priority = ?, source_archive_size = ? "
      "WHERE path = ? AND (source_priority IS NULL OR source_priority != ? "
      "OR source_archive_size IS NULL OR source_archive_size != ?)";
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt,
                                    "preparing chart source preference update",
                                    logSqlErrorText)) {
    return false;
  }
  sqlite3_bind_int(stmt.get(), 1, priority);
  sqlite3_bind_int64(stmt.get(), 2, archiveSize);
  bindSqliteText(stmt.get(), 3, storedPathText);
  sqlite3_bind_int(stmt.get(), 4, priority);
  sqlite3_bind_int64(stmt.get(), 5, archiveSize);
  int rc = sqlite3_step(stmt.get());
  if (rc != SQLITE_DONE) {
    logSqlError("updating chart source preference", db);
    return false;
  }
  const bool changed = sqlite3_changes(db) > 0;
  return changed;
}

bool updateChartSourcePreference(sqlite3 *db,
                                 const std::filesystem::path &chartPath) {
  const archive_file::SourcePreference preference =
      archive_file::sourcePreferenceForPath(chartPath);
  return updateChartSourcePreferenceValues(
      db, chartPath, preference.priority,
      clampSqlInteger(preference.archiveSize));
}

sqlite3_int64 fileTimeToSqlNs(std::filesystem::file_time_type time) {
  const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         time.time_since_epoch())
                         .count();
  if (nanos > std::numeric_limits<sqlite3_int64>::max()) {
    return std::numeric_limits<sqlite3_int64>::max();
  }
  if (nanos < std::numeric_limits<sqlite3_int64>::min()) {
    return std::numeric_limits<sqlite3_int64>::min();
  }
  return static_cast<sqlite3_int64>(nanos);
}

bool archiveFileStateForDb(const std::filesystem::path &path,
                           sqlite3_int64 &archiveSize,
                           sqlite3_int64 &mtimeNs) {
  std::error_code error;
  const bool regularFile = std::filesystem::is_regular_file(path, error);
  if (error || !regularFile) {
    return false;
  }
  error.clear();
  const auto size = std::filesystem::file_size(path, error);
  if (error) {
    return false;
  }
  error.clear();
  const auto mtime = std::filesystem::last_write_time(path, error);
  if (error) {
    return false;
  }
  archiveSize = clampSqlInteger(
      static_cast<std::uint64_t>(
          std::min<std::uintmax_t>(
              size, std::numeric_limits<std::uint64_t>::max())));
  mtimeNs = fileTimeToSqlNs(mtime);
  return true;
}

std::string archivePathTextForDb(const std::filesystem::path &archivePath) {
  return storedChartPathText(archivePath);
}

std::string checkpointPathTextForDb(const std::filesystem::path &path) {
  return storedChartPathText(path);
}

std::filesystem::path checkpointPathFromDbText(const std::string &text) {
  std::filesystem::path path(utf8_to_path_t(text));
  if (!path.empty()) {
    ChartDBHelper::ToAbsolutePath(path);
  }
  return path;
}

std::string checkpointInnerPathText(const std::filesystem::path &path) {
  return path.lexically_normal().generic_string();
}

void fnv1aAppend(std::uint64_t &hash, const std::string &text) {
  constexpr std::uint64_t kPrime = 1099511628211ull;
  for (const unsigned char ch : text) {
    hash ^= ch;
    hash *= kPrime;
  }
  hash ^= 0xffu;
  hash *= kPrime;
}

std::string stableHashHex(std::uint64_t value) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string text(16, '0');
  for (int i = 15; i >= 0; --i) {
    text[i] = kHex[value & 0xfu];
    value >>= 4u;
  }
  return text;
}

struct ChartScanCheckpoint {
  bool found = false;
  std::string scanSignature;
  std::string phase;
  int nextIndex = 0;
  int subIndex = 0;
  std::filesystem::path lastPath;
  std::filesystem::path archivePath;
  sqlite3_int64 archiveSize = 0;
  sqlite3_int64 archiveMtimeNs = 0;
  std::string lastInnerPath;
};

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
  if (normalizeStoredPathColumn(db, "chart_scan_checkpoint", "last_path",
                                false) |
      normalizeStoredPathColumn(db, "chart_scan_checkpoint", "archive_path",
                                false)) {
    bumpLibraryRevision();
  }
  return true;
}

ChartScanCheckpoint selectChartScanCheckpoint(sqlite3 *db) {
  ChartScanCheckpoint checkpoint;
  const char *query =
      "SELECT scan_signature, phase, next_index, sub_index, last_path, "
      "archive_path, archive_size, archive_mtime_ns, last_inner_path "
      "FROM chart_scan_checkpoint WHERE id = 1";
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt,
                                    "selecting chart scan checkpoint",
                                    logSqlErrorText)) {
    return checkpoint;
  }
  if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    checkpoint.found = true;
    checkpoint.scanSignature = columnString(stmt.get(), 0);
    checkpoint.phase = columnString(stmt.get(), 1);
    checkpoint.nextIndex = std::max(0, sqlite3_column_int(stmt.get(), 2));
    checkpoint.subIndex = std::max(0, sqlite3_column_int(stmt.get(), 3));
    checkpoint.lastPath = checkpointPathFromDbText(columnString(stmt.get(), 4));
    checkpoint.archivePath =
        checkpointPathFromDbText(columnString(stmt.get(), 5));
    checkpoint.archiveSize = sqlite3_column_int64(stmt.get(), 6);
    checkpoint.archiveMtimeNs = sqlite3_column_int64(stmt.get(), 7);
    checkpoint.lastInnerPath = columnString(stmt.get(), 8);
  }
  return checkpoint;
}

bool upsertChartScanCheckpoint(sqlite3 *db,
                               const ChartScanCheckpoint &checkpoint) {
  const char *query =
      "INSERT INTO chart_scan_checkpoint "
      "(id, scan_signature, phase, next_index, sub_index, last_path, "
      "archive_path, archive_size, archive_mtime_ns, last_inner_path, "
      "updated_at) VALUES (1, ?, ?, ?, ?, ?, ?, ?, ?, ?, "
      "CURRENT_TIMESTAMP) "
      "ON CONFLICT(id) DO UPDATE SET "
      "scan_signature = excluded.scan_signature,"
      "phase = excluded.phase,"
      "next_index = excluded.next_index,"
      "sub_index = excluded.sub_index,"
      "last_path = excluded.last_path,"
      "archive_path = excluded.archive_path,"
      "archive_size = excluded.archive_size,"
      "archive_mtime_ns = excluded.archive_mtime_ns,"
      "last_inner_path = excluded.last_inner_path,"
      "updated_at = CURRENT_TIMESTAMP";
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(
          db, query, stmt, "preparing chart scan checkpoint upsert",
          logSqlErrorText)) {
    return false;
  }
  bindSqliteText(stmt.get(), 1, checkpoint.scanSignature);
  bindSqliteText(stmt.get(), 2, checkpoint.phase);
  sqlite3_bind_int(stmt.get(), 3, std::max(0, checkpoint.nextIndex));
  sqlite3_bind_int(stmt.get(), 4, std::max(0, checkpoint.subIndex));
  bindSqliteText(stmt.get(), 5, checkpointPathTextForDb(checkpoint.lastPath));
  bindSqliteText(stmt.get(), 6, checkpointPathTextForDb(checkpoint.archivePath));
  sqlite3_bind_int64(stmt.get(), 7, checkpoint.archiveSize);
  sqlite3_bind_int64(stmt.get(), 8, checkpoint.archiveMtimeNs);
  bindSqliteText(stmt.get(), 9, checkpoint.lastInnerPath);
  int rc = sqlite3_step(stmt.get());
  if (rc != SQLITE_DONE) {
    logSqlError("upserting chart scan checkpoint", db);
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
  if (normalizeStoredPathColumn(db, "archive_scan_cache", "path", true)) {
    bumpLibraryRevision();
  }
  return true;
}

ArchiveScanCacheRecord selectArchiveScanCache(
    sqlite3 *db, const std::filesystem::path &archivePath) {
  ArchiveScanCacheRecord record;
  const std::string pathText = archivePathTextForDb(archivePath);
  const char *query =
      "SELECT archive_size, mtime_ns, solid, uncompressed_size, file_count, "
      "chart_count FROM archive_scan_cache WHERE path = ?";
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt,
                                    "selecting archive scan cache",
                                    logSqlErrorText)) {
    return record;
  }
  bindSqliteText(stmt.get(), 1, pathText);
  if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    record.found = true;
    record.archiveSize = sqlite3_column_int64(stmt.get(), 0);
    record.mtimeNs = sqlite3_column_int64(stmt.get(), 1);
    record.solid = sqlite3_column_int(stmt.get(), 2) != 0;
    record.uncompressedSize = static_cast<std::uint64_t>(
        std::max<sqlite3_int64>(0, sqlite3_column_int64(stmt.get(), 3)));
    record.fileCount = std::max(0, sqlite3_column_int(stmt.get(), 4));
    record.chartCount = sqlite3_column_int(stmt.get(), 5);
  }
  return record;
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
    ChartDBHelper::ToAbsolutePath(path);
    paths.push_back(path);
  }
  return paths;
}

bool archiveScanCacheMatches(const ArchiveScanCacheRecord &record,
                             sqlite3_int64 archiveSize,
                             sqlite3_int64 mtimeNs) {
  return record.found && record.archiveSize == archiveSize &&
         record.mtimeNs == mtimeNs && record.chartCount >= 0;
}

bool upsertArchiveScanCache(sqlite3 *db,
                            const std::filesystem::path &archivePath,
                            bool solid, std::uint64_t uncompressedSize,
                            int fileCount, int chartCount) {
  sqlite3_int64 archiveSize = 0;
  sqlite3_int64 mtimeNs = 0;
  if (!archiveFileStateForDb(archivePath, archiveSize, mtimeNs)) {
    return false;
  }

  const std::string pathText = archivePathTextForDb(archivePath);
  const char *query =
      "INSERT INTO archive_scan_cache "
      "(path, archive_size, mtime_ns, solid, uncompressed_size, file_count, "
      "chart_count, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, "
      "CURRENT_TIMESTAMP) "
      "ON CONFLICT(path) DO UPDATE SET "
      "archive_size = excluded.archive_size,"
      "mtime_ns = excluded.mtime_ns,"
      "solid = excluded.solid,"
      "uncompressed_size = excluded.uncompressed_size,"
      "file_count = excluded.file_count,"
      "chart_count = excluded.chart_count,"
      "updated_at = CURRENT_TIMESTAMP "
      "WHERE archive_scan_cache.archive_size != excluded.archive_size "
      "OR archive_scan_cache.mtime_ns != excluded.mtime_ns "
      "OR archive_scan_cache.solid != excluded.solid "
      "OR archive_scan_cache.uncompressed_size != excluded.uncompressed_size "
      "OR archive_scan_cache.file_count != excluded.file_count "
      "OR archive_scan_cache.chart_count != excluded.chart_count";
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt,
                                    "preparing archive scan cache upsert",
                                    logSqlErrorText)) {
    return false;
  }
  bindSqliteText(stmt.get(), 1, pathText);
  sqlite3_bind_int64(stmt.get(), 2, archiveSize);
  sqlite3_bind_int64(stmt.get(), 3, mtimeNs);
  sqlite3_bind_int(stmt.get(), 4, solid ? 1 : 0);
  sqlite3_bind_int64(stmt.get(), 5, clampSqlInteger(uncompressedSize));
  sqlite3_bind_int(stmt.get(), 6, std::max(0, fileCount));
  sqlite3_bind_int(stmt.get(), 7, std::max(0, chartCount));
  int rc = sqlite3_step(stmt.get());
  if (rc != SQLITE_DONE) {
    logSqlError("upserting archive scan cache", db);
    return false;
  }
  const bool changed = sqlite3_changes(db) > 0;
  return changed;
}

bool deleteArchiveScanCache(sqlite3 *db,
                            const std::filesystem::path &archivePath) {
  const std::string pathText = archivePathTextForDb(archivePath);
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

std::string solidArchiveNameForPath(const std::filesystem::path &path) {
  const std::string name = path.filename().generic_string();
  return name.empty() ? path.generic_string() : name;
}

bool upsertSolidArchive(sqlite3 *db, const std::filesystem::path &archivePath,
                        std::uint64_t uncompressedSize, int fileCount) {
  sqlite3_int64 archiveSize = 0;
  sqlite3_int64 mtimeNs = 0;
  if (!archiveFileStateForDb(archivePath, archiveSize, mtimeNs)) {
    return false;
  }

  const std::string pathText = storedChartPathText(archivePath);
  const std::string name = solidArchiveNameForPath(archivePath);

  const char *query =
      "INSERT INTO solid_archives "
      "(path, name, archive_size, uncompressed_size, file_count, mtime_ns, "
      "updated_at) VALUES (?, ?, ?, ?, ?, ?, CURRENT_TIMESTAMP) "
      "ON CONFLICT(path) DO UPDATE SET "
      "name = excluded.name,"
      "archive_size = excluded.archive_size,"
      "uncompressed_size = excluded.uncompressed_size,"
      "file_count = excluded.file_count,"
      "mtime_ns = excluded.mtime_ns,"
      "updated_at = CURRENT_TIMESTAMP "
      "WHERE solid_archives.name != excluded.name "
      "OR solid_archives.archive_size != excluded.archive_size "
      "OR solid_archives.uncompressed_size != excluded.uncompressed_size "
      "OR solid_archives.file_count != excluded.file_count "
      "OR solid_archives.mtime_ns != excluded.mtime_ns";
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt,
                                    "preparing solid archive insert",
                                    logSqlErrorText)) {
    return false;
  }
  bindSqliteText(stmt.get(), 1, pathText);
  bindSqliteText(stmt.get(), 2, name);
  sqlite3_bind_int64(stmt.get(), 3, archiveSize);
  sqlite3_bind_int64(stmt.get(), 4, clampSqlInteger(uncompressedSize));
  sqlite3_bind_int(stmt.get(), 5, std::max(0, fileCount));
  sqlite3_bind_int64(stmt.get(), 6, mtimeNs);
  int rc = sqlite3_step(stmt.get());
  if (rc != SQLITE_DONE) {
    logSqlError("inserting solid archive", db);
    return false;
  }
  const bool changed = sqlite3_changes(db) > 0;
  return changed;
}

bool deleteSolidArchive(sqlite3 *db, const std::filesystem::path &archivePath) {
  const std::string pathText = storedChartPathText(archivePath);
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
  std::vector<bms_parser::ChartMeta> chartMetas;
  ChartDBHelper::GetInstance().SelectAllChartMeta(db, chartMetas);

  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(
          db, "DELETE FROM chart_meta WHERE path = ?", stmt,
          "preparing archive chart delete", logSqlErrorText)) {
    return false;
  }

  bool changed = false;
  const std::filesystem::path targetArchive = archivePath.lexically_normal();
  for (const auto &meta : chartMetas) {
    if (!pathIsInsideDirectory(meta.BmsPath, targetArchive)) {
      continue;
    }

    const std::string pathText = storedChartPathText(meta.BmsPath);
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
    ChartDBHelper::ToAbsolutePath(path);
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

int upsertDifficultyTable(sqlite3 *db, const std::string &name,
                          const std::string &symbol, const std::string &dataUrl,
                          const std::string &sourceUrl) {
  int tableId = findDifficultyTableBySourceUrl(db, sourceUrl);
  if (tableId <= 0) {
    tableId = findDifficultyTable(db, name, symbol, sourceUrl);
  }
  if (tableId > 0) {
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
                           const std::string &constraintJson, int sortOrder) {
  auto query =
      "INSERT INTO difficulty_courses "
      "(table_id, name, group_name, level, constraint_json, sort_order) "
      "VALUES (@table_id, @name, @group_name, @level, @constraint_json, "
      "@sort_order)";
  SqliteStatementHandle stmt;
  int rc = prepareSqliteStatement(db, query, stmt);
  if (rc != SQLITE_OK) {
    return 0;
  }
  sqlite3_bind_int(stmt.get(), 1, tableId);
  bindSqliteText(stmt.get(), 2, name);
  bindSqliteText(stmt.get(), 3, groupName);
  bindSqliteText(stmt.get(), 4, level);
  bindSqliteText(stmt.get(), 5, constraintJson);
  sqlite3_bind_int(stmt.get(), 6, sortOrder);
  rc = sqlite3_step(stmt.get());
  if (rc != SQLITE_DONE) {
    return 0;
  }
  return static_cast<int>(sqlite3_last_insert_rowid(db));
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

void appendUniqueLabel(
    std::unordered_map<std::string, std::vector<std::string>> &labelsByHash,
    const std::string &hash, const std::string &label) {
  if (hash.empty() || label.empty()) {
    return;
  }
  auto &labels = labelsByHash[hash];
  if (std::find(labels.begin(), labels.end(), label) == labels.end()) {
    labels.push_back(label);
  }
}

void appendChartLabels(const std::vector<std::string> &source,
                       std::vector<std::string> &destination,
                       std::unordered_set<std::string> &seen) {
  for (const auto &label : source) {
    if (seen.insert(label).second) {
      destination.push_back(label);
    }
  }
}

std::string joinLabels(const std::vector<std::string> &labels) {
  std::string joined;
  for (const auto &label : labels) {
    if (!joined.empty()) {
      joined += " / ";
    }
    joined += label;
  }
  return joined;
}

void loadDifficultyLabelCache(sqlite3 *db, DifficultyLabelCache &cache) {
  cache.loaded = false;
  cache.labelsBySha256.clear();
  cache.labelsByMd5.clear();
  auto query = "SELECT dte.sha256, dte.md5, dt.symbol || dte.level AS label "
               "FROM difficulty_table_entries dte "
               "JOIN difficulty_tables dt ON dt.id = dte.table_id "
               "WHERE dte.sha256 != '' OR dte.md5 != '' "
               "ORDER BY dt.id, dte.sort_order, label";
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt,
                                    "loading difficulty labels",
                                    logSqlErrorText)) {
    return;
  }

  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    const std::string sha256 = normalizedHash(columnString(stmt.get(), 0));
    const std::string md5 = normalizedHash(columnString(stmt.get(), 1));
    const std::string label = columnString(stmt.get(), 2);
    appendUniqueLabel(cache.labelsBySha256, sha256, label);
    appendUniqueLabel(cache.labelsByMd5, md5, label);
  }
  cache.loaded = true;
}

void invalidateDifficultyLabelCache() {
  std::lock_guard<std::mutex> lock(gDifficultyLabelCacheMutex);
  gDifficultyLabelCache.loaded = false;
  gDifficultyLabelCache.labelsBySha256.clear();
  gDifficultyLabelCache.labelsByMd5.clear();
}

void populateDifficultyTableLabels(
    sqlite3 *db, std::vector<ChartMetaRecord> &chartRecords) {
  if (chartRecords.empty()) {
    return;
  }

  std::lock_guard<std::mutex> lock(gDifficultyLabelCacheMutex);
  if (!gDifficultyLabelCache.loaded) {
    loadDifficultyLabelCache(db, gDifficultyLabelCache);
  }
  if (!gDifficultyLabelCache.loaded) {
    return;
  }

  for (auto &chartRecord : chartRecords) {
    std::vector<std::string> labels;
    std::unordered_set<std::string> seen;
    const auto shaIt = gDifficultyLabelCache.labelsBySha256.find(
        normalizedHash(chartRecord.meta.SHA256));
    if (shaIt != gDifficultyLabelCache.labelsBySha256.end()) {
      appendChartLabels(shaIt->second, labels, seen);
    }
    const auto md5It =
        gDifficultyLabelCache.labelsByMd5.find(
            normalizedHash(chartRecord.meta.MD5));
    if (md5It != gDifficultyLabelCache.labelsByMd5.end()) {
      appendChartLabels(md5It->second, labels, seen);
    }
    chartRecord.difficultyTableLabels = joinLabels(labels);
  }
}
} // namespace

sqlite3 *ChartDBHelper::Connect() {
  const std::filesystem::path directory = Utils::GetDocumentsPath("db");
  std::cout << "DB Directory: " << fspath_to_utf8(directory) << "\n";
  std::error_code directoryError;
  if (!Utils::EnsureDirectoryExists(directory, directoryError)) {
    std::cerr << "Can't create chart database directory "
              << fspath_to_utf8(directory) << ": "
              << directoryError.message() << "\n";
    return nullptr;
  }
  const std::filesystem::path path = directory / "chart.db";
  std::cout << "DB Path: " << fspath_to_utf8(path) << "\n";
  std::string openError;
  sqlite3 *db = openSqliteDatabase(path, openError);
  if (db == nullptr) {
    std::cerr << "Can't open chart database: " << openError << "\n";
    return nullptr;
  }
  if (const auto pragmaError = applySqlitePragmas(
          db, {"PRAGMA journal_mode=WAL", "PRAGMA synchronous=NORMAL"})) {
    std::cerr << "Could not configure chart database: " << *pragmaError
              << "\n";
  }
  return db;
}

void ChartDBHelper::Close(sqlite3 *db) {
  closeSqliteDatabase(db);
}

bool ChartDBHelper::CreateChartMetaTable(sqlite3 *db) {
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
      "CREATE INDEX IF NOT EXISTS idx_chart_meta_title ON chart_meta(title)",
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
  const bool normalizedPaths =
      normalizeStoredPathColumn(db, "chart_meta", "path", true) |
      normalizeStoredPathColumn(db, "chart_meta", "folder", false);
  if (normalizedPaths) {
    bumpLibraryRevision();
  }
  return CreateFavoritesTable(db);
}

bool ChartDBHelper::CreateFavoritesTable(sqlite3 *db) {
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

  if (normalizeStoredPathColumn(db, "chart_favorites", "chart_path", true)) {
    bumpLibraryRevision();
  }
  return true;
}

bool ChartDBHelper::CreateSolidArchiveTable(sqlite3 *db) {
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
      "CREATE INDEX IF NOT EXISTS idx_solid_archives_size "
      "ON solid_archives(uncompressed_size)",
  };
  for (const auto *indexQuery : indexes) {
    if (!execSql(db, indexQuery, "creating solid archive index")) {
      return false;
    }
  }
  if (normalizeStoredPathColumn(db, "solid_archives", "path", true)) {
    bumpLibraryRevision();
  }
  return true;
}

bool ChartDBHelper::InsertChartMeta(sqlite3 *db,
                                    bms_parser::ChartMeta &chartMeta) {
  auto query = "REPLACE INTO chart_meta ("
               "path,"
               "md5,"
               "sha256,"
               "title,"
               "subtitle,"
               "genre,"
               "artist,"
               "sub_artist,"
               "folder,"
               "stage_file,"
               "banner,"
               "back_bmp,"
               "preview,"
               "level,"
               "difficulty,"
               "total,"
               "bpm,"
               "max_bpm,"
               "min_bpm,"
               "length,"
               "rank,"
               "player,"
               "keys,"
               "total_notes,"
               "total_long_notes,"
               "total_scratch_notes,"
               "total_backspin_notes,"
               "ln_mode,"
               "source_priority,"
               "source_archive_size"
               ") VALUES("
               "@path,"
               "@md5,"
               "@sha256,"
               "@title,"
               "@subtitle,"
               "@genre,"
               "@artist,"
               "@sub_artist,"
               "@folder,"
               "@stage_file,"
               "@banner,"
               "@back_bmp,"
               "@preview,"
               "@level,"
               "@difficulty,"
               "@total,"
               "@bpm,"
               "@max_bpm,"
               "@min_bpm,"
               "@length,"
               "@rank,"
               "@player,"
               "@keys,"
               "@total_notes,"
               "@total_long_notes,"
               "@total_scratch_notes,"
               "@total_backspin_notes,"
               "@ln_mode,"
               "@source_priority,"
               "@source_archive_size"
               ")";
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt,
                                    "preparing statement to insert a chart",
                                    logSdlSqlErrorText)) {
    return false;
  }
  const archive_file::SourcePreference sourcePreference =
      archive_file::sourcePreferenceForPath(chartMeta.BmsPath);
  std::filesystem::path path = chartMeta.BmsPath;
  ToRelativePath(path);
  const std::string md5 = normalizedHash(chartMeta.MD5);
  const std::string sha256 = normalizedHash(chartMeta.SHA256);

  bindSqliteText(stmt, 1, fspath_to_utf8(path));
  bindSqliteText(stmt, 2, md5);
  bindSqliteText(stmt, 3, sha256);
  bindSqliteText(stmt, 4, chartMeta.Title);
  bindSqliteText(stmt, 5, chartMeta.SubTitle);
  bindSqliteText(stmt, 6, chartMeta.Genre);
  bindSqliteText(stmt, 7, chartMeta.Artist);
  bindSqliteText(stmt, 8, chartMeta.SubArtist);

  std::filesystem::path folder = chartMeta.Folder;
  ToRelativePath(folder);
  bindSqliteText(stmt, 9, fspath_to_utf8(folder));
  bindSqliteText(stmt, 10, fspath_to_utf8(chartMeta.StageFile));
  bindSqliteText(stmt, 11, fspath_to_utf8(chartMeta.Banner));
  bindSqliteText(stmt, 12, fspath_to_utf8(chartMeta.BackBmp));
  bindSqliteText(stmt, 13, fspath_to_utf8(chartMeta.Preview));
  sqlite3_bind_double(stmt, 14, chartMeta.PlayLevel);
  sqlite3_bind_int(stmt, 15, chartMeta.Difficulty);
  sqlite3_bind_double(stmt, 16, chartMeta.Total);
  sqlite3_bind_double(stmt, 17, chartMeta.Bpm);
  sqlite3_bind_double(stmt, 18, chartMeta.MaxBpm);
  sqlite3_bind_double(stmt, 19, chartMeta.MinBpm);
  sqlite3_bind_int64(stmt, 20, chartMeta.PlayLength);
  sqlite3_bind_int(stmt, 21, chartMeta.Rank);
  sqlite3_bind_int(stmt, 22, chartMeta.Player);
  sqlite3_bind_int(stmt, 23, chartMeta.KeyMode);
  sqlite3_bind_int(stmt, 24, chartMeta.TotalNotes);
  sqlite3_bind_int(stmt, 25, chartMeta.TotalLongNotes);
  sqlite3_bind_int(stmt, 26, chartMeta.TotalScratchNotes);
  sqlite3_bind_int(stmt, 27, chartMeta.TotalBackSpinNotes);
  sqlite3_bind_int(stmt, 28, chartMeta.LnMode);
  sqlite3_bind_int(stmt, 29, sourcePreference.priority);
  sqlite3_bind_int64(stmt, 30, clampSqlInteger(sourcePreference.archiveSize));
  const int rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE) {
    logSdlSqlError("inserting a chart", db);
    return false;
  }
  bumpLibraryRevision();
  return true;
}

void ChartDBHelper::SelectAllChartMeta(
    sqlite3 *db, std::vector<bms_parser::ChartMeta> &chartMetas) {
  std::string query = "SELECT ";
  query += kChartMetaSelectColumns;
  query += " FROM chart_meta cm ORDER BY cm.title";
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt, "getting all charts",
                                    logSqlErrorText)) {
    return;
  }

  // reserve space for the result
  chartMetas.reserve(sqlite3_column_count(stmt));

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    chartMetas.push_back(std::move(ReadChartMeta(stmt)));
  }
}

void ChartDBHelper::SelectMusicTracks(sqlite3 *db,
                                      std::vector<MusicTrackRecord> &tracks) {
  const char *musicKey = "COALESCE(NULLIF(cm.folder, ''), cm.path)";
  std::string query = "SELECT ";
  query += kChartMetaSelectColumns;
  query += ", cm.music_chart_count FROM (SELECT cm.*, "
           "COUNT(*) OVER (PARTITION BY ";
  query += musicKey;
  query += ") AS music_chart_count, "
           "ROW_NUMBER() OVER (PARTITION BY ";
  query += musicKey;
  query += " ORDER BY total_notes DESC, length DESC, ";
  query += chartSourceOrderBy("cm");
  query += ", title COLLATE NOCASE, path) AS music_rank FROM chart_meta cm "
           "WHERE ";
  query += preferredChartPredicate("cm");
  query += ") cm WHERE cm.music_rank = 1 "
           "ORDER BY cm.title COLLATE NOCASE, cm.path";

  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt,
                                    "selecting music tracks",
                                    logSqlErrorText)) {
    return;
  }

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    MusicTrackRecord record;
    record.representativeChart = ReadChartMeta(stmt);
    record.chartCount =
        std::max(1, sqlite3_column_int(stmt, kChartMetaColumnCount));
    tracks.push_back(std::move(record));
  }
}

void ChartDBHelper::SelectFavoriteMusicTracks(
    sqlite3 *db, std::vector<MusicTrackRecord> &tracks) {
  if (db == nullptr || !CreateFavoritesTable(db)) {
    return;
  }

  std::string query = "SELECT ";
  query += kChartMetaSelectColumns;
  query += ", cm.music_chart_count FROM (SELECT cm.*, "
           "cf.added_at AS favorite_added_at, "
           "COUNT(*) OVER (PARTITION BY cf.chart_path) AS music_chart_count, "
           "ROW_NUMBER() OVER (PARTITION BY cf.chart_path ORDER BY ";
  query += chartArtworkOrderBy("cm");
  query += ", CASE WHEN cf.chart_sha256 != '' AND "
           "cm.sha256 = cf.chart_sha256 THEN 0 WHEN cf.chart_md5 != '' AND "
           "cm.md5 = cf.chart_md5 THEN 1 WHEN cf.chart_path != '' AND "
           "cm.path = cf.chart_path THEN 2 ELSE 3 END, ";
  query += "total_notes DESC, length DESC, ";
  query += chartSourceOrderBy("cm");
  query += ", title COLLATE NOCASE, path) AS music_rank "
           "FROM chart_favorites cf JOIN chart_meta cm ON "
           "cm.path = cf.chart_path WHERE ";
  query += preferredChartPredicate("cm");
  query += ") cm WHERE cm.music_rank = 1 "
           "ORDER BY cm.favorite_added_at DESC, cm.title COLLATE NOCASE, "
           "cm.path";

  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt,
                                    "selecting favorite music tracks",
                                    logSqlErrorText)) {
    return;
  }

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    MusicTrackRecord record;
    record.representativeChart = ReadChartMeta(stmt);
    record.chartCount =
        std::max(1, sqlite3_column_int(stmt, kChartMetaColumnCount));
    record.useChartPathIdentity = true;
    tracks.push_back(std::move(record));
  }
}

int ChartDBHelper::CountFavoriteCharts(sqlite3 *db) {
  if (db == nullptr || !CreateFavoritesTable(db)) {
    return 0;
  }
  ChartMetaQuery query;
  query.favoritesOnly = true;
  return CountChartMeta(db, query);
}

bool ChartDBHelper::SetFavorite(sqlite3 *db,
                                const bms_parser::ChartMeta &chartMeta,
                                bool favorite) {
  if (db == nullptr || !CreateFavoritesTable(db)) {
    return false;
  }

  const std::string chartPath = storedChartPathText(chartMeta.BmsPath);
  if (chartPath.empty()) {
    return false;
  }

  if (!favorite) {
    const char *query = "DELETE FROM chart_favorites WHERE chart_path = ?1";
    SqliteStatementHandle stmt;
    if (!prepareSqliteStatementLogged(db, query, stmt,
                                      "preparing chart favorite delete",
                                      logSqlErrorText)) {
      return false;
    }
    bindSqliteText(stmt, 1, chartPath);
    const int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
      logSqlError("deleting chart favorite", db);
      return false;
    }
    bumpLibraryRevision();
    return true;
  }

  const char *query =
      "INSERT INTO chart_favorites "
      "(chart_path, chart_md5, chart_sha256, added_at) "
      "VALUES (?1, ?2, ?3, CURRENT_TIMESTAMP) "
      "ON CONFLICT(chart_path) DO UPDATE SET "
      "chart_md5 = excluded.chart_md5,"
      "chart_sha256 = excluded.chart_sha256,"
      "added_at = CURRENT_TIMESTAMP";
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt,
                                    "preparing chart favorite insert",
                                    logSqlErrorText)) {
    return false;
  }
  bindSqliteText(stmt, 1, chartPath);
  bindSqliteText(stmt, 2, normalizedHash(chartMeta.MD5));
  bindSqliteText(stmt, 3, normalizedHash(chartMeta.SHA256));
  const int rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE) {
    logSqlError("saving chart favorite", db);
    return false;
  }
  bumpLibraryRevision();
  return true;
}

int ChartDBHelper::CountAllChartMeta(sqlite3 *db) {
  auto query = "SELECT COUNT(*) FROM chart_meta";
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt, "counting charts",
                                    logSqlErrorText)) {
    return 0;
  }
  int count = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    count = sqlite3_column_int(stmt, 0);
  }
  return count;
}

int ChartDBHelper::CountSolidArchives(sqlite3 *db) {
  auto query = "SELECT COUNT(*) FROM solid_archives";
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt,
                                    "counting solid archives",
                                    logSqlErrorText)) {
    return 0;
  }
  int count = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    count = sqlite3_column_int(stmt, 0);
  }
  return count;
}

void ChartDBHelper::SearchChartMeta(
    sqlite3 *db, const std::string &text,
    std::vector<ChartMetaRecord> &chartMetas) {
  if (!CreateFavoritesTable(db)) {
    return;
  }
  std::string query = "SELECT ";
  query += kChartMetaSelectColumns;
  query += ", '', 0, ";
  query += kChartFavoriteColumn;
  query += " FROM chart_meta cm WHERE rtrim(cm.title || ' ' || cm.subtitle || "
           "' ' || cm.artist || ' ' || cm.sub_artist || ' ' || cm.genre) LIKE "
           "@text AND ";
  query += preferredChartPredicate("cm");
  query += " ORDER BY cm.title";
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt, "searching for charts",
                                    logSqlErrorText)) {
    return;
  }
  // %text%
  bindSqliteText(stmt, 1, "%" + text + "%");

  // reserve space for the result
  chartMetas.reserve(sqlite3_column_count(stmt));

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    chartMetas.push_back(std::move(ReadChartMetaRecord(stmt)));
  }
  populateDifficultyTableLabels(db, chartMetas);
}

void ChartDBHelper::QueryChartMeta(
    sqlite3 *db, const ChartMetaQuery &chartQuery,
    std::vector<ChartMetaRecord> &chartMetas) {
  if (!CreateFavoritesTable(db)) {
    return;
  }
  if (queryNeedsDifficultyTableSchema(chartQuery) &&
      !CreateDifficultyTableTables(db)) {
    return;
  }
  if (chartQuery.solidArchivesOnly) {
    std::string query = "SELECT ";
    query += kSolidArchiveSelectColumns;
    query += " FROM solid_archives sa WHERE 1 = 1";
    if (!chartQuery.keyword.empty()) {
      query += " AND (sa.name LIKE @text OR sa.path LIKE @text)";
    }
    query += " ORDER BY sa.name COLLATE NOCASE, sa.path";
    if (chartQuery.limit > 0) {
      query += " LIMIT @limit OFFSET @offset";
    }

    SqliteStatementHandle stmt;
    if (!prepareSqliteStatementLogged(db, query, stmt,
                                      "querying solid archives",
                                      logSqlErrorText)) {
      return;
    }

    int bindIndex = 1;
    if (!chartQuery.keyword.empty()) {
      bindSqliteText(stmt, bindIndex++, "%" + chartQuery.keyword + "%");
    }
    if (chartQuery.limit > 0) {
      sqlite3_bind_int(stmt, bindIndex++, chartQuery.limit);
      sqlite3_bind_int(stmt, bindIndex++, std::max(0, chartQuery.offset));
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
      int idx = 0;
      ChartMetaRecord record;
      std::filesystem::path path(ReadPath(stmt, idx++));
      if (!path.empty()) {
        ToAbsolutePath(path);
      }
      record.meta.BmsPath = path;
      record.meta.Folder = path.parent_path();
      record.meta.Title = columnString(stmt, idx++);
      record.meta.Artist = "Solid archive skipped";
      record.archiveSize = static_cast<std::uint64_t>(
          std::max<sqlite3_int64>(0, sqlite3_column_int64(stmt, idx++)));
      record.archiveUncompressedSize = static_cast<std::uint64_t>(
          std::max<sqlite3_int64>(0, sqlite3_column_int64(stmt, idx++)));
      record.archiveFileCount = sqlite3_column_int(stmt, idx++);
      record.difficultyTableLabels = "Unzip required";
      record.solidArchive = true;
      chartMetas.push_back(std::move(record));
    }
    return;
  }

  const bool queryDifficultyEntries =
      chartQuery.tableId > 0 && !chartQuery.coursesOnly &&
      chartQuery.courseId <= 0 && chartQuery.courseTableId <= 0 &&
      chartQuery.courseGroupName.empty();
  if (queryDifficultyEntries) {
    std::string query = "SELECT ";
    query += kDifficultyEntrySelectColumns;
    query += ", ";
    query += kChartFavoriteColumn;
    query += " FROM difficulty_table_entries dte "
             "JOIN difficulty_tables dt ON dt.id = dte.table_id "
             "LEFT JOIN chart_meta cm ON cm.path = ("
             "SELECT cm_match.path FROM chart_meta cm_match "
             "WHERE ((dte.sha256 != '' AND cm_match.sha256 = dte.sha256) "
             "OR (dte.md5 != '' AND cm_match.md5 = dte.md5)) ";
    query += "ORDER BY ";
    query += chartSourceOrderBy("cm_match");
    query += ", cm_match.title COLLATE NOCASE LIMIT 1) "
             "WHERE dte.table_id = @table_id";

    if (!chartQuery.tableLevel.empty()) {
      query += " AND dte.level = @table_level";
    }
    if (!chartQuery.keyword.empty()) {
      query += " AND ";
      query += kDifficultyEntrySearchText;
      query += " LIKE @text";
    }
    if (!chartQuery.difficultyText.empty()) {
      query +=
          " AND (lower(dt.symbol || dte.level) = @difficulty "
          "OR lower(dte.level) = @difficulty "
          "OR lower(dt.name || ' ' || dt.symbol || dte.level) LIKE "
          "@difficulty_like "
          "OR lower(dt.name || ' ' || dte.level) LIKE @difficulty_like)";
    }
    if (chartQuery.clearMarkFilter) {
      query += " AND ";
      query += difficultyEntryClearMarkPredicate(
          "dte", "cm", chartQuery.selectedLongNoteMode);
    }

    query += " ORDER BY CASE WHEN cm.path IS NULL THEN 1 ELSE 0 END, "
             "dte.sort_order, COALESCE(NULLIF(cm.title, ''), dte.title, '') "
             "COLLATE NOCASE, dte.id";
    if (chartQuery.limit > 0) {
      query += " LIMIT @limit OFFSET @offset";
    }

    SqliteStatementHandle stmt;
    if (!prepareSqliteStatementLogged(db, query, stmt,
                                      "querying difficulty entries",
                                      logSqlErrorText)) {
      return;
    }

    int bindIndex = 1;
    sqlite3_bind_int(stmt, bindIndex++, chartQuery.tableId);
    if (!chartQuery.tableLevel.empty()) {
      bindSqliteText(stmt, bindIndex++, chartQuery.tableLevel);
    }
    if (!chartQuery.keyword.empty()) {
      bindSqliteText(stmt, bindIndex++, "%" + chartQuery.keyword + "%");
    }
    if (!chartQuery.difficultyText.empty()) {
      bindDifficultySearchText(stmt, bindIndex, chartQuery.difficultyText);
    }
    if (chartQuery.clearMarkFilter) {
      sqlite3_bind_int(stmt, bindIndex++, chartQuery.clearMarkRank);
    }
    if (chartQuery.limit > 0) {
      sqlite3_bind_int(stmt, bindIndex++, chartQuery.limit);
      sqlite3_bind_int(stmt, bindIndex++, std::max(0, chartQuery.offset));
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
      chartMetas.push_back(std::move(ReadChartMetaRecord(stmt)));
    }
    return;
  }

  const bool queryCourseEntries =
      chartQuery.coursesOnly || chartQuery.courseId > 0 ||
      chartQuery.courseTableId > 0 || !chartQuery.courseGroupName.empty();
  if (queryCourseEntries) {
    std::string query = "SELECT ";
    query += kDifficultyCourseEntrySelectColumns;
    query += ", ";
    query += kChartFavoriteColumn;
    query += " FROM difficulty_course_entries dce "
             "JOIN difficulty_courses dc ON dc.id = dce.course_id "
             "JOIN difficulty_tables dt ON dt.id = dc.table_id "
             "LEFT JOIN difficulty_table_entries dte ON dte.id = ("
             "SELECT dte_match.id FROM difficulty_table_entries dte_match "
             "WHERE dte_match.table_id = dc.table_id "
             "AND ((dce.sha256 != '' AND dte_match.sha256 = dce.sha256) "
             "OR (dce.md5 != '' AND dte_match.md5 = dce.md5)) "
             "ORDER BY dte_match.sort_order, dte_match.title COLLATE NOCASE "
             "LIMIT 1) "
             "LEFT JOIN chart_meta cm ON cm.path = ("
             "SELECT cm_match.path FROM chart_meta cm_match "
             "WHERE ((dce.sha256 != '' AND cm_match.sha256 = dce.sha256) "
             "OR (dce.md5 != '' AND cm_match.md5 = dce.md5)) ";
    query += "ORDER BY ";
    query += chartSourceOrderBy("cm_match");
    query += ", cm_match.title COLLATE NOCASE LIMIT 1) WHERE 1 = 1";

    if (chartQuery.courseId > 0) {
      query += " AND dce.course_id = @course_id";
    }
    if (chartQuery.courseTableId > 0) {
      query += " AND dc.table_id = @course_table_id";
    }
    if (!chartQuery.courseGroupName.empty()) {
      query += " AND dc.group_name = @course_group_name";
    }
    if (!chartQuery.keyword.empty()) {
      query += " AND ";
      query += kDifficultyCourseEntrySearchText;
      query += " LIKE @text";
    }
    if (!chartQuery.difficultyText.empty()) {
      query +=
          " AND (lower(dt.symbol || NULLIF(NULLIF(dce.level, ''), '0')) = "
          "@difficulty "
          "OR lower(dt.symbol || dte.level) = @difficulty "
          "OR lower(dce.level) = @difficulty "
          "OR lower(dte.level) = @difficulty "
          "OR lower(dt.symbol || dc.level) = @difficulty "
          "OR lower(dc.level) = @difficulty "
          "OR lower(dc.name) LIKE @difficulty_like "
          "OR lower(dc.group_name || ' ' || dc.level) LIKE "
          "@difficulty_like)";
    }

    query += " ORDER BY dc.sort_order, dce.sort_order, "
             "COALESCE(NULLIF(cm.title, ''), dce.title, '') COLLATE NOCASE, "
             "dce.id";
    if (chartQuery.limit > 0) {
      query += " LIMIT @limit OFFSET @offset";
    }

    SqliteStatementHandle stmt;
    if (!prepareSqliteStatementLogged(db, query, stmt,
                                      "querying course entries",
                                      logSqlErrorText)) {
      return;
    }

    int bindIndex = 1;
    if (chartQuery.courseId > 0) {
      sqlite3_bind_int(stmt, bindIndex++, chartQuery.courseId);
    }
    if (chartQuery.courseTableId > 0) {
      sqlite3_bind_int(stmt, bindIndex++, chartQuery.courseTableId);
    }
    if (!chartQuery.courseGroupName.empty()) {
      bindSqliteText(stmt, bindIndex++, chartQuery.courseGroupName);
    }
    if (!chartQuery.keyword.empty()) {
      bindSqliteText(stmt, bindIndex++, "%" + chartQuery.keyword + "%");
    }
    if (!chartQuery.difficultyText.empty()) {
      bindDifficultySearchText(stmt, bindIndex, chartQuery.difficultyText);
    }
    if (chartQuery.limit > 0) {
      sqlite3_bind_int(stmt, bindIndex++, chartQuery.limit);
      sqlite3_bind_int(stmt, bindIndex++, std::max(0, chartQuery.offset));
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
      chartMetas.push_back(std::move(ReadChartMetaRecord(stmt)));
    }
    return;
  }

  std::string query = "SELECT ";
  query += kChartMetaSelectColumns;
  query += ", '', 0, ";
  query += kChartFavoriteColumn;
  query += " FROM chart_meta cm WHERE 1 = 1";
  appendChartMetaFilters(query, chartQuery);
  query += " ORDER BY cm.title, cm.path";
  if (chartQuery.limit > 0) {
    query += " LIMIT @limit OFFSET @offset";
  }

  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt, "querying charts",
                                    logSqlErrorText)) {
    return;
  }

  int bindIndex = 1;
  bindChartMetaFilterParameters(stmt, bindIndex, chartQuery);
  if (chartQuery.limit > 0) {
    sqlite3_bind_int(stmt, bindIndex++, chartQuery.limit);
    sqlite3_bind_int(stmt, bindIndex++, std::max(0, chartQuery.offset));
  }

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    chartMetas.push_back(std::move(ReadChartMetaRecord(stmt)));
  }
  populateDifficultyTableLabels(db, chartMetas);
}

int ChartDBHelper::CountChartMeta(sqlite3 *db,
                                  const ChartMetaQuery &chartQuery) {
  if (!CreateFavoritesTable(db)) {
    return 0;
  }
  if (queryNeedsDifficultyTableSchema(chartQuery) &&
      !CreateDifficultyTableTables(db)) {
    return 0;
  }
  if (chartQuery.solidArchivesOnly) {
    std::string query = "SELECT COUNT(*) FROM solid_archives sa WHERE 1 = 1";
    if (!chartQuery.keyword.empty()) {
      query += " AND (sa.name LIKE @text OR sa.path LIKE @text)";
    }
    SqliteStatementHandle stmt;
    if (!prepareSqliteStatementLogged(db, query, stmt,
                                      "counting solid archives",
                                      logSqlErrorText)) {
      return 0;
    }
    if (!chartQuery.keyword.empty()) {
      bindSqliteText(stmt, 1, "%" + chartQuery.keyword + "%");
    }
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      count = sqlite3_column_int(stmt, 0);
    }
    return count;
  }

  const bool queryDifficultyEntries =
      chartQuery.tableId > 0 && !chartQuery.coursesOnly &&
      chartQuery.courseId <= 0 && chartQuery.courseTableId <= 0 &&
      chartQuery.courseGroupName.empty();
  if (queryDifficultyEntries) {
    std::string query = "SELECT COUNT(*) FROM difficulty_table_entries dte "
                        "JOIN difficulty_tables dt ON dt.id = dte.table_id ";
    if (!chartQuery.keyword.empty() || chartQuery.clearMarkFilter) {
      query +=
          "LEFT JOIN chart_meta cm ON cm.path = ("
          "SELECT cm_match.path FROM chart_meta cm_match "
          "WHERE ((dte.sha256 != '' AND cm_match.sha256 = dte.sha256) "
          "OR (dte.md5 != '' AND cm_match.md5 = dte.md5)) ";
      query += "ORDER BY ";
      query += chartSourceOrderBy("cm_match");
      query += ", cm_match.title COLLATE NOCASE LIMIT 1) ";
    }
    query += "WHERE dte.table_id = @table_id";

    if (!chartQuery.tableLevel.empty()) {
      query += " AND dte.level = @table_level";
    }
    if (!chartQuery.keyword.empty()) {
      query += " AND ";
      query += kDifficultyEntrySearchText;
      query += " LIKE @text";
    }
    if (!chartQuery.difficultyText.empty()) {
      query +=
          " AND (lower(dt.symbol || dte.level) = @difficulty "
          "OR lower(dte.level) = @difficulty "
          "OR lower(dt.name || ' ' || dt.symbol || dte.level) LIKE "
          "@difficulty_like "
          "OR lower(dt.name || ' ' || dte.level) LIKE @difficulty_like)";
    }
    if (chartQuery.clearMarkFilter) {
      query += " AND ";
      query += difficultyEntryClearMarkPredicate(
          "dte", "cm", chartQuery.selectedLongNoteMode);
    }

    SqliteStatementHandle stmt;
    if (!prepareSqliteStatementLogged(db, query, stmt,
                                      "counting difficulty entries",
                                      logSqlErrorText)) {
      return 0;
    }

    int bindIndex = 1;
    sqlite3_bind_int(stmt, bindIndex++, chartQuery.tableId);
    if (!chartQuery.tableLevel.empty()) {
      bindSqliteText(stmt, bindIndex++, chartQuery.tableLevel);
    }
    if (!chartQuery.keyword.empty()) {
      bindSqliteText(stmt, bindIndex++, "%" + chartQuery.keyword + "%");
    }
    if (!chartQuery.difficultyText.empty()) {
      bindDifficultySearchText(stmt, bindIndex, chartQuery.difficultyText);
    }
    if (chartQuery.clearMarkFilter) {
      sqlite3_bind_int(stmt, bindIndex++, chartQuery.clearMarkRank);
    }

    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      count = sqlite3_column_int(stmt, 0);
    }
    return count;
  }

  const bool countCourseEntries =
      chartQuery.coursesOnly || chartQuery.courseId > 0 ||
      chartQuery.courseTableId > 0 || !chartQuery.courseGroupName.empty();
  if (countCourseEntries) {
    std::string query =
        "SELECT COUNT(*) FROM difficulty_course_entries dce "
        "JOIN difficulty_courses dc ON dc.id = dce.course_id "
        "JOIN difficulty_tables dt ON dt.id = dc.table_id "
        "LEFT JOIN difficulty_table_entries dte ON dte.id = ("
        "SELECT dte_match.id FROM difficulty_table_entries dte_match "
        "WHERE dte_match.table_id = dc.table_id "
        "AND ((dce.sha256 != '' AND dte_match.sha256 = dce.sha256) "
        "OR (dce.md5 != '' AND dte_match.md5 = dce.md5)) "
        "ORDER BY dte_match.sort_order, dte_match.title COLLATE NOCASE "
        "LIMIT 1) ";
    if (!chartQuery.keyword.empty()) {
      query +=
          "LEFT JOIN chart_meta cm ON cm.path = ("
          "SELECT cm_match.path FROM chart_meta cm_match "
          "WHERE ((dce.sha256 != '' AND cm_match.sha256 = dce.sha256) "
          "OR (dce.md5 != '' AND cm_match.md5 = dce.md5)) ";
      query += "ORDER BY ";
      query += chartSourceOrderBy("cm_match");
      query += ", cm_match.title COLLATE NOCASE LIMIT 1) ";
    }
    query += "WHERE 1 = 1";

    if (chartQuery.courseId > 0) {
      query += " AND dce.course_id = @course_id";
    }
    if (chartQuery.courseTableId > 0) {
      query += " AND dc.table_id = @course_table_id";
    }
    if (!chartQuery.courseGroupName.empty()) {
      query += " AND dc.group_name = @course_group_name";
    }
    if (!chartQuery.keyword.empty()) {
      query += " AND ";
      query += kDifficultyCourseEntrySearchText;
      query += " LIKE @text";
    }
    if (!chartQuery.difficultyText.empty()) {
      query +=
          " AND (lower(dt.symbol || NULLIF(NULLIF(dce.level, ''), '0')) = "
          "@difficulty "
          "OR lower(dt.symbol || dte.level) = @difficulty "
          "OR lower(dce.level) = @difficulty "
          "OR lower(dte.level) = @difficulty "
          "OR lower(dt.symbol || dc.level) = @difficulty "
          "OR lower(dc.level) = @difficulty "
          "OR lower(dc.name) LIKE @difficulty_like "
          "OR lower(dc.group_name || ' ' || dc.level) LIKE "
          "@difficulty_like)";
    }

    SqliteStatementHandle stmt;
    if (!prepareSqliteStatementLogged(db, query, stmt,
                                      "counting course entries",
                                      logSqlErrorText)) {
      return 0;
    }

    int bindIndex = 1;
    if (chartQuery.courseId > 0) {
      sqlite3_bind_int(stmt, bindIndex++, chartQuery.courseId);
    }
    if (chartQuery.courseTableId > 0) {
      sqlite3_bind_int(stmt, bindIndex++, chartQuery.courseTableId);
    }
    if (!chartQuery.courseGroupName.empty()) {
      bindSqliteText(stmt, bindIndex++, chartQuery.courseGroupName);
    }
    if (!chartQuery.keyword.empty()) {
      bindSqliteText(stmt, bindIndex++, "%" + chartQuery.keyword + "%");
    }
    if (!chartQuery.difficultyText.empty()) {
      bindDifficultySearchText(stmt, bindIndex, chartQuery.difficultyText);
    }

    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      count = sqlite3_column_int(stmt, 0);
    }
    return count;
  }

  std::string query = "SELECT COUNT(*) FROM chart_meta cm WHERE 1 = 1";
  appendChartMetaFilters(query, chartQuery);

  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt, "counting charts",
                                    logSqlErrorText)) {
    return 0;
  }

  int bindIndex = 1;
  bindChartMetaFilterParameters(stmt, bindIndex, chartQuery);

  int count = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    count = sqlite3_column_int(stmt, 0);
  }
  return count;
}

bool ChartDBHelper::DeleteChartMeta(sqlite3 *db, std::filesystem::path path) {
  // std::cout << "Deleting chart: " << path.string() << std::endl;
  ToRelativePath(path);
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
    bumpLibraryRevision();
  }
  return true;
}

int ChartDBHelper::DeleteChartMetaInDirectory(
    sqlite3 *db, const std::filesystem::path &directory) {
  if (directory.empty()) {
    return -1;
  }

  CreateSolidArchiveTable(db);
  createArchiveScanCacheTable(db);
  createChartScanCheckpointTable(db);

  std::filesystem::path targetDirectory = directory;
  ToAbsolutePath(targetDirectory);
  const std::filesystem::path normalizedTarget =
      targetDirectory.lexically_normal();
  auto matchesTarget = [&](const std::filesystem::path &path) {
    return path.lexically_normal() == normalizedTarget ||
           pathIsInsideDirectory(path, targetDirectory);
  };

  std::vector<bms_parser::ChartMeta> chartMetas;
  SelectAllChartMeta(db, chartMetas);

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

    const std::string target = storedChartPathText(chartMeta.BmsPath);
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
    bumpLibraryRevision();
  }
  return deletedCount;
}

bool ChartDBHelper::DeleteArchiveRecords(
    sqlite3 *db, const std::filesystem::path &archivePath) {
  if (db == nullptr || archivePath.empty()) {
    return false;
  }

  CreateChartMetaTable(db);
  CreateSolidArchiveTable(db);
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
    bumpLibraryRevision();
  }
  return changedCount > 0;
}

bool ChartDBHelper::ClearChartMeta(sqlite3 *db) {
  CreateSolidArchiveTable(db);
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
    bumpLibraryRevision();
  }
  return true;
}

bms_parser::ChartMeta ChartDBHelper::ReadChartMeta(sqlite3_stmt *stmt) {
  const auto absolutePathFromColumn = [this](sqlite3_stmt *row, int column) {
    std::filesystem::path path(ReadPath(row, column));
    if (!path.empty()) {
      ToAbsolutePath(path);
    }
    return path;
  };
  const auto relativePathFromColumn = [this](sqlite3_stmt *row, int column) {
    return std::filesystem::path(ReadPath(row, column));
  };
  return asobmshow::chart_sql::readChartMeta(stmt, absolutePathFromColumn,
                                             relativePathFromColumn);
}

ChartMetaRecord ChartDBHelper::ReadChartMetaRecord(sqlite3_stmt *stmt) {
  ChartMetaRecord record;
  record.meta = ReadChartMeta(stmt);
  int idx = kChartMetaColumnCount;
  if (sqlite3_column_count(stmt) > idx) {
    record.difficultyTableLabels = columnString(stmt, idx++);
  }
  if (sqlite3_column_count(stmt) > idx) {
    record.unavailable = sqlite3_column_int(stmt, idx++) != 0;
  }
  if (sqlite3_column_count(stmt) > idx) {
    record.favorite = sqlite3_column_int(stmt, idx++) != 0;
  }
  return record;
}

std::string ChartDBHelper::DifficultyTableLabelsForChart(
    const bms_parser::ChartMeta &meta) {
  SqliteConnectionHandle connection(Connect());
  if (connection.get() == nullptr) {
    return {};
  }
  return DifficultyTableLabelsForChart(connection.get(), meta);
}

std::string ChartDBHelper::DifficultyTableLabelsForChart(
    sqlite3 *db, const bms_parser::ChartMeta &meta) {
  if (db == nullptr || !CreateDifficultyTableTables(db)) {
    return {};
  }

  std::vector<ChartMetaRecord> records(1);
  records.front().meta = meta;
  populateDifficultyTableLabels(db, records);
  return records.front().difficultyTableLabels;
}

bool ChartDBHelper::CreateEntriesTable(sqlite3 *db) {
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
  if (normalizeStoredPathColumn(db, "entries", "path", true)) {
    bumpLibraryRevision();
  }
  return true;
}

bool ChartDBHelper::InsertEntry(sqlite3 *db,
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
  const std::string pathText = storedChartPathText(path);
  bindSqliteText(stmt, 1, pathText);
  bindSqliteText(stmt, 2, iosBookmark);
  int rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE) {
    logSqlError("inserting an entry", db);
    return false;
  }
  clearChartScanCheckpoint(db);
  bumpLibraryRevision();
  return true;
}

std::vector<ChartEntry> ChartDBHelper::SelectAllEntries(sqlite3 *db) {
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
    std::filesystem::path path(ReadPath(stmt, 0));
    if (!path.empty()) {
      ToAbsolutePath(path);
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

std::filesystem::path ChartDBHelper::DefaultBmsFolderPath() {
  return Utils::GetDocumentsPath("BMS");
}

bool ChartDBHelper::IsDefaultBmsFolderPath(
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

std::vector<ChartEntry> ChartDBHelper::SelectEffectiveEntries(sqlite3 *db) {
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

bool ChartDBHelper::DeleteEntry(sqlite3 *db,
                                const std::filesystem::path &path) {
  createChartScanCheckpointTable(db);
  auto query = "DELETE FROM entries WHERE path = @path";
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt,
                                    "preparing statement to delete an entry",
                                    logSqlErrorText)) {
    return false;
  }
  const std::string pathText = storedChartPathText(path);
  bindSqliteText(stmt, 1, pathText);
  int rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE) {
    logSqlError("deleting an entry", db);
    return false;
  }
  if (sqlite3_changes(db) > 0) {
    clearChartScanCheckpoint(db);
    bumpLibraryRevision();
  }
  return true;
}

bool ChartDBHelper::ClearEntries(sqlite3 *db) {
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
    bumpLibraryRevision();
  }
  return true;
}

int ChartDBHelper::ScanChartRoots(
    sqlite3 *db, const std::vector<std::filesystem::path> &roots,
    const std::stop_token *stopToken,
    ChartScanProgressCallback progressCallback,
    ChartScanPauseCallback pauseCallback,
    ChartScanFlushRequestCallback flushRequestCallback,
    ChartScanFlushCompleteCallback flushCompleteCallback) {
  if (db == nullptr || stopRequested(stopToken)) {
    return 0;
  }
  auto pauseIfNeeded = [&]() {
    return pauseCallback == nullptr || pauseCallback();
  };
  auto shouldStop = [&]() {
    return stopRequested(stopToken) || !pauseIfNeeded();
  };

  auto reportProgress = [&](int current, int total,
                            ChartScanProgressStage stage) {
    if (progressCallback != nullptr) {
      progressCallback(ChartScanProgress{
          .current = current,
          .total = total,
          .stage = stage,
      });
    }
  };

  reportProgress(0, static_cast<int>(std::max<std::size_t>(roots.size(), 1)),
                 ChartScanProgressStage::Preparing);
  if (shouldStop()) {
    return 0;
  }

  CreateChartMetaTable(db);
  CreateSolidArchiveTable(db);
  createArchiveScanCacheTable(db);
  createChartScanCheckpointTable(db);

  std::vector<bms_parser::ChartMeta> chartMetas;
  SelectAllChartMeta(db, chartMetas);

  struct ScanDiff {
    std::filesystem::path path;
    bool deleted = false;
  };
  std::vector<ScanDiff> diffs;
  std::vector<std::filesystem::path> sourcePreferenceRefreshPaths;
  struct CachedSourcePreferenceUpdate {
    std::filesystem::path path;
    int priority = 0;
    sqlite3_int64 archiveSize = 0;
  };
  std::vector<CachedSourcePreferenceUpdate> cachedSourcePreferenceUpdates;
  struct SolidArchiveDiff {
    std::filesystem::path path;
    bool solid = false;
    std::uint64_t uncompressedSize = 0;
    int fileCount = 0;
  };
  struct ArchiveCacheDiff {
    std::filesystem::path path;
    bool solid = false;
    std::uint64_t uncompressedSize = 0;
    int fileCount = 0;
    int chartCount = 0;
  };
  std::vector<SolidArchiveDiff> solidArchiveDiffs;
  std::vector<ArchiveCacheDiff> archiveCacheDiffs;
  std::unordered_map<path_t, ArchiveCacheDiff> pendingArchiveCacheDiffs;
  std::vector<std::filesystem::path> staleSolidArchives;
  std::vector<std::filesystem::path> reindexedArchives;
  std::unordered_set<path_t> knownChartPaths;
  std::unordered_map<path_t, int> knownArchiveChartCounts;
  std::unordered_set<path_t> scannedArchivePaths;
  diffs.reserve(chartMetas.size());
  sourcePreferenceRefreshPaths.reserve(chartMetas.size());
  cachedSourcePreferenceUpdates.reserve(chartMetas.size());

  struct ArchiveKnownState {
    bool checked = false;
    bool fileAvailable = false;
    sqlite3_int64 archiveSize = 0;
    sqlite3_int64 mtimeNs = 0;
    ArchiveScanCacheRecord cache;
  };
  std::unordered_map<path_t, ArchiveKnownState> archiveStates;
  auto archiveStateForPath = [&](const std::filesystem::path &archivePath)
      -> ArchiveKnownState & {
    const path_t archiveKey = fspath_to_path_t(archivePath);
    auto [it, inserted] = archiveStates.emplace(archiveKey, ArchiveKnownState{});
    ArchiveKnownState &state = it->second;
    if (!state.checked) {
      state.checked = true;
      state.fileAvailable =
          archiveFileStateForDb(archivePath, state.archiveSize, state.mtimeNs);
      if (state.fileAvailable) {
        state.cache = selectArchiveScanCache(db, archivePath);
      }
    }
    return state;
  };

  for (const auto &chartMeta : chartMetas) {
    if (shouldStop()) {
      return 0;
    }
    if (!parsedChartMetaHasStableIdentity(chartMeta)) {
      diffs.push_back({.path = chartMeta.BmsPath, .deleted = true});
      continue;
    }

    std::filesystem::path archivePath;
    std::filesystem::path innerPath;
    if (archive_file::splitVirtualPath(chartMeta.BmsPath, archivePath,
                                       innerPath)) {
      const path_t archiveKey = fspath_to_path_t(archivePath);
      ++knownArchiveChartCounts[archiveKey];
      const ArchiveKnownState &archiveState = archiveStateForPath(archivePath);
      if (archiveState.fileAvailable &&
          archiveScanCacheMatches(archiveState.cache, archiveState.archiveSize,
                                  archiveState.mtimeNs)) {
        knownChartPaths.insert(fspath_to_path_t(chartMeta.BmsPath));
        cachedSourcePreferenceUpdates.push_back({
            .path = chartMeta.BmsPath,
            .priority = archiveState.cache.solid ? 2 : 1,
            .archiveSize = archiveState.archiveSize,
        });
      }
      continue;
    }

    if (archive_file::exists(chartMeta.BmsPath)) {
      knownChartPaths.insert(fspath_to_path_t(chartMeta.BmsPath));
      sourcePreferenceRefreshPaths.push_back(chartMeta.BmsPath);
    } else {
      diffs.push_back({.path = chartMeta.BmsPath, .deleted = true});
    }
  }

  for (const auto &solidArchivePath : selectSolidArchivePaths(db)) {
    if (shouldStop()) {
      return 0;
    }
    sqlite3_int64 archiveSize = 0;
    sqlite3_int64 mtimeNs = 0;
    if (!archiveFileStateForDb(solidArchivePath, archiveSize, mtimeNs)) {
      staleSolidArchives.push_back(solidArchivePath);
    }
  }

  auto scanArchivePath = [&](const std::filesystem::path &archivePath) {
    if (shouldStop()) {
      return;
    }
    const path_t archiveKey = fspath_to_path_t(archivePath);
    if (scannedArchivePaths.find(archiveKey) != scannedArchivePaths.end()) {
      return;
    }
    scannedArchivePaths.insert(archiveKey);

    sqlite3_int64 archiveSize = 0;
    sqlite3_int64 mtimeNs = 0;
    if (!archiveFileStateForDb(archivePath, archiveSize, mtimeNs)) {
      return;
    }

    const std::string archiveText = fspath_to_utf8(archivePath);
    const ArchiveScanCacheRecord cache =
        selectArchiveScanCache(db, archivePath);
    if (archiveScanCacheMatches(cache, archiveSize, mtimeNs)) {
      const int knownChartCount =
          knownArchiveChartCounts.contains(archiveKey)
              ? knownArchiveChartCounts.at(archiveKey)
              : 0;
      if (!cache.solid && knownChartCount < cache.chartCount) {
        archive_file::appendDebugLogLine(
            "Archive scan cache incomplete; rescanning: " +
            archiveText + " cachedCharts=" + std::to_string(cache.chartCount) +
            " dbCharts=" + std::to_string(knownChartCount));
      } else {
        archive_file::appendDebugLogLine(
            "Using cached archive scan: " + archiveText +
            " files=" + std::to_string(cache.fileCount) +
            " charts=" + std::to_string(cache.chartCount) +
            " solid=" + std::string(cache.solid ? "yes" : "no") +
            " estimatedUnpacked=" +
            std::to_string(cache.uncompressedSize));
        solidArchiveDiffs.push_back({
            .path = archivePath,
            .solid = cache.solid,
            .uncompressedSize = cache.uncompressedSize,
            .fileCount = cache.fileCount,
        });
        return;
      }
    }
    if (cache.found) {
      archive_file::appendDebugLogLine(
          "Archive scan cache invalidated: " + archiveText);
    }

    const ArchiveScanResult archiveScan =
        scanArchiveForChartsOrSolid(archivePath, knownChartPaths,
                                    pauseCallback);
    if (shouldStop()) {
      return;
    }
    if (!archiveScan.readable) {
      return;
    }
    reindexedArchives.push_back(archivePath);
    ArchiveCacheDiff cacheDiff{
        .path = archivePath,
        .solid = archiveScan.solid,
        .uncompressedSize = archiveScan.uncompressedSize,
        .fileCount = archiveScan.fileCount,
        .chartCount = static_cast<int>(archiveScan.chartPaths.size()),
    };
    if (archiveScan.solid) {
      archiveCacheDiffs.push_back(cacheDiff);
      solidArchiveDiffs.push_back({
          .path = archivePath,
          .solid = true,
          .uncompressedSize = archiveScan.uncompressedSize,
          .fileCount = archiveScan.fileCount,
      });
      return;
    }

    solidArchiveDiffs.push_back({
        .path = archivePath,
        .solid = false,
    });
    if (archiveScan.chartPaths.empty()) {
      archiveCacheDiffs.push_back(cacheDiff);
    } else {
      pendingArchiveCacheDiffs[archiveKey] = cacheDiff;
    }
    for (const auto &chartPath : archiveScan.chartPaths) {
      diffs.push_back({.path = chartPath, .deleted = false});
    }
  };

  int scannedRootCount = 0;
  const int rootCount = static_cast<int>(std::max<std::size_t>(roots.size(), 1));
  for (const auto &root : roots) {
    if (shouldStop() || root.empty()) {
      continue;
    }
    reportProgress(scannedRootCount, rootCount,
                   ChartScanProgressStage::ScanningRoots);
#if TARGET_OS_ANDROID
    if (IsAndroidTreePath(root)) {
      std::vector<std::filesystem::path> androidChartPaths;
      std::string androidError;
      if (!ListAndroidTreeChartFiles(root, androidChartPaths, androidError,
                                     stopToken)) {
        if (!androidError.empty()) {
          SDL_Log("Failed while scanning Android chart folder %s: %s",
                  fspath_to_utf8(root).c_str(), androidError.c_str());
        }
        ++scannedRootCount;
        continue;
      }
      for (const auto &path : androidChartPaths) {
        if (shouldStop()) {
          return 0;
        }
        if (asobmshow::bms_chart_file::isBmsChartPath(path)) {
          const path_t key = fspath_to_path_t(path);
          if (knownChartPaths.find(key) == knownChartPaths.end()) {
            diffs.push_back({.path = path, .deleted = false});
            knownChartPaths.insert(key);
          }
          continue;
        }
        if (archive_file::hasSupportedArchiveExtension(path)) {
          archive_file::appendDebugLogLine(
              "Skipping Android SAF archive during library scan: " +
              fspath_to_utf8(path));
        }
      }
      ++scannedRootCount;
      continue;
    }
#endif
    std::error_code error;
    const bool rootExists = std::filesystem::exists(root, error);
    if (error) {
      SDL_Log("Failed to check chart folder %s: %s",
              fspath_to_utf8(root).c_str(), error.message().c_str());
      ++scannedRootCount;
      continue;
    }
    if (!rootExists) {
      ++scannedRootCount;
      continue;
    }

    std::error_code rootTypeError;
    if (std::filesystem::is_regular_file(root, rootTypeError) &&
        !rootTypeError) {
      if (asobmshow::bms_chart_file::isBmsChartPath(root)) {
        const path_t key = fspath_to_path_t(root);
        if (knownChartPaths.find(key) == knownChartPaths.end()) {
          diffs.push_back({.path = root, .deleted = false});
          knownChartPaths.insert(key);
        }
      } else if (archive_file::hasSupportedArchiveExtension(root)) {
        scanArchivePath(root);
      }
      ++scannedRootCount;
      continue;
    }

    std::filesystem::recursive_directory_iterator iterator(
        root, std::filesystem::directory_options::skip_permission_denied,
        error);
    for (const auto end = std::filesystem::recursive_directory_iterator();
         !error && iterator != end; iterator.increment(error)) {
      if (shouldStop()) {
        return 0;
      }
      std::error_code typeError;
      if (!iterator->is_regular_file(typeError) || typeError) {
        continue;
      }
      const std::filesystem::path path = iterator->path();
      if (asobmshow::bms_chart_file::isBmsChartPath(path)) {
        const path_t key = fspath_to_path_t(path);
        if (knownChartPaths.find(key) == knownChartPaths.end()) {
          diffs.push_back({.path = path, .deleted = false});
          knownChartPaths.insert(key);
        }
        continue;
      }
      if (archive_file::hasSupportedArchiveExtension(path)) {
        scanArchivePath(path);
      }
    }
    if (error) {
      SDL_Log("Failed while scanning chart folder %s: %s",
              fspath_to_utf8(root).c_str(), error.message().c_str());
    }
    ++scannedRootCount;
  }
  reportProgress(rootCount, rootCount, ChartScanProgressStage::PreparingUpdates);

  const bool noScanWork =
      diffs.empty() && sourcePreferenceRefreshPaths.empty() &&
      cachedSourcePreferenceUpdates.empty() && solidArchiveDiffs.empty() &&
      archiveCacheDiffs.empty() && pendingArchiveCacheDiffs.empty() &&
      staleSolidArchives.empty() && reindexedArchives.empty();
  if (noScanWork) {
    clearChartScanCheckpoint(db);
    clearChartMetadataRebuildRequiredIfPresent(db);
    return 0;
  }
  if (shouldStop()) {
    return 0;
  }

  struct ArchiveParseBatch {
    std::filesystem::path archivePath;
    std::vector<std::filesystem::path> innerPaths;
  };
  std::vector<ScanDiff> individualDiffs;
  std::vector<path_t> archiveBatchOrder;
  std::unordered_map<path_t, ArchiveParseBatch> archiveBatches;
  individualDiffs.reserve(diffs.size());
  for (const auto &diff : diffs) {
    if (diff.deleted) {
      individualDiffs.push_back(diff);
      continue;
    }
    std::filesystem::path archivePath;
    std::filesystem::path innerPath;
    if (!archive_file::splitVirtualPath(diff.path, archivePath, innerPath)) {
      individualDiffs.push_back(diff);
      continue;
    }

    const path_t archiveKey = fspath_to_path_t(archivePath);
    auto batchIt = archiveBatches.find(archiveKey);
    if (batchIt == archiveBatches.end()) {
      archiveBatchOrder.push_back(archiveKey);
      batchIt = archiveBatches
                    .emplace(archiveKey, ArchiveParseBatch{
                                             .archivePath = archivePath,
                                             .innerPaths = {},
                                         })
                    .first;
    }
    batchIt->second.innerPaths.push_back(innerPath);
  }

  int parseTotal = static_cast<int>(individualDiffs.size());
  for (const auto &archiveKey : archiveBatchOrder) {
    const auto batchIt = archiveBatches.find(archiveKey);
    if (batchIt != archiveBatches.end()) {
      parseTotal += static_cast<int>(batchIt->second.innerPaths.size());
    }
  }
  parseTotal = std::max(parseTotal, 1);
  int parseCurrent = 0;

  auto computeScanSignature = [&](std::size_t individualStart,
                                  std::size_t archiveStart) {
    constexpr std::uint64_t kOffset = 14695981039346656037ull;
    std::uint64_t hash = kOffset;
    fnv1aAppend(hash, "chart-scan-v1");

    std::vector<std::string> rootKeys;
    rootKeys.reserve(roots.size());
    for (const auto &root : roots) {
      std::string rootKey = checkpointPathTextForDb(root);
      sqlite3_int64 size = 0;
      sqlite3_int64 mtimeNs = 0;
      if (archiveFileStateForDb(root, size, mtimeNs)) {
        rootKey += "|file|";
        rootKey += std::to_string(size);
        rootKey += "|";
        rootKey += std::to_string(mtimeNs);
      } else {
        std::error_code error;
        const bool directory = std::filesystem::is_directory(root, error);
        if (error) {
          rootKey += "|unknown";
        } else {
          rootKey += directory ? "|dir" : "|missing";
        }
      }
      rootKeys.push_back(std::move(rootKey));
    }
    std::sort(rootKeys.begin(), rootKeys.end());
    fnv1aAppend(hash, "roots");
    fnv1aAppend(hash, std::to_string(rootKeys.size()));
    for (const auto &rootKey : rootKeys) {
      fnv1aAppend(hash, rootKey);
    }

    fnv1aAppend(hash, "individual");
    individualStart = std::min(individualStart, individualDiffs.size());
    fnv1aAppend(hash,
                std::to_string(individualDiffs.size() - individualStart));
    for (std::size_t i = individualStart; i < individualDiffs.size(); ++i) {
      const auto &diff = individualDiffs[i];
      fnv1aAppend(hash, diff.deleted ? "d" : "u");
      fnv1aAppend(hash, checkpointPathTextForDb(diff.path));
    }

    fnv1aAppend(hash, "archives");
    archiveStart = std::min(archiveStart, archiveBatchOrder.size());
    fnv1aAppend(hash, std::to_string(archiveBatchOrder.size() - archiveStart));
    for (std::size_t i = archiveStart; i < archiveBatchOrder.size(); ++i) {
      const auto &archiveKey = archiveBatchOrder[i];
      const auto batchIt = archiveBatches.find(archiveKey);
      if (batchIt == archiveBatches.end()) {
        continue;
      }
      const ArchiveParseBatch &batch = batchIt->second;
      fnv1aAppend(hash, checkpointPathTextForDb(batch.archivePath));
      sqlite3_int64 archiveSize = 0;
      sqlite3_int64 mtimeNs = 0;
      if (archiveFileStateForDb(batch.archivePath, archiveSize, mtimeNs)) {
        fnv1aAppend(hash, std::to_string(archiveSize));
        fnv1aAppend(hash, std::to_string(mtimeNs));
      } else {
        fnv1aAppend(hash, "missing");
      }
      fnv1aAppend(hash, std::to_string(batch.innerPaths.size()));
      for (const auto &innerPath : batch.innerPaths) {
        fnv1aAppend(hash, checkpointInnerPathText(innerPath));
      }
    }
    return stableHashHex(hash);
  };

  const std::string scanSignature = computeScanSignature(0, 0);
  struct ResumePlan {
    bool valid = false;
    bool archivePhase = false;
    std::size_t individualStart = 0;
    std::size_t archiveStart = 0;
    std::size_t archiveSubStart = 0;
    std::unordered_set<path_t> protectedArchiveKeys;
  };
  ResumePlan resumePlan;
  const ChartScanCheckpoint checkpoint = selectChartScanCheckpoint(db);

  auto archiveBatchInnerCountBefore = [&](std::size_t archiveIndex) {
    std::size_t count = 0;
    const std::size_t limit = std::min(archiveIndex, archiveBatchOrder.size());
    for (std::size_t i = 0; i < limit; ++i) {
      const auto batchIt = archiveBatches.find(archiveBatchOrder[i]);
      if (batchIt != archiveBatches.end()) {
        count += batchIt->second.innerPaths.size();
      }
    }
    return count;
  };

  auto validateIndividualCheckpoint = [&]() {
    const std::size_t nextIndex =
        static_cast<std::size_t>(checkpoint.nextIndex);
    if (nextIndex > individualDiffs.size() || checkpoint.subIndex != 0) {
      return false;
    }
    if (nextIndex > 0 &&
        checkpointPathTextForDb(individualDiffs[nextIndex - 1].path) !=
            checkpointPathTextForDb(checkpoint.lastPath)) {
      return false;
    }
    resumePlan.valid = true;
    resumePlan.archivePhase = false;
    resumePlan.individualStart = nextIndex;
    parseCurrent = static_cast<int>(
        std::min<std::size_t>(nextIndex, static_cast<std::size_t>(parseTotal)));
    return true;
  };

  auto validateArchiveCheckpoint = [&]() {
    const std::size_t nextIndex =
        static_cast<std::size_t>(checkpoint.nextIndex);
    const std::size_t subIndex = static_cast<std::size_t>(checkpoint.subIndex);
    if (nextIndex > archiveBatchOrder.size()) {
      return false;
    }
    if (nextIndex == 0 && subIndex == 0) {
      resumePlan.valid = true;
      resumePlan.archivePhase = true;
      resumePlan.individualStart = individualDiffs.size();
      resumePlan.archiveStart = 0;
      resumePlan.archiveSubStart = 0;
      parseCurrent = static_cast<int>(std::min<std::size_t>(
          individualDiffs.size(), static_cast<std::size_t>(parseTotal)));
      return true;
    }

    if (subIndex == 0) {
      if (nextIndex == 0) {
        return false;
      }
      const auto previousBatchIt =
          archiveBatches.find(archiveBatchOrder[nextIndex - 1]);
      if (previousBatchIt == archiveBatches.end()) {
        return false;
      }
      const ArchiveParseBatch &previousBatch = previousBatchIt->second;
      if (checkpointPathTextForDb(previousBatch.archivePath) !=
          checkpointPathTextForDb(checkpoint.archivePath)) {
        return false;
      }
      sqlite3_int64 archiveSize = 0;
      sqlite3_int64 mtimeNs = 0;
      if (!archiveFileStateForDb(previousBatch.archivePath, archiveSize,
                                 mtimeNs) ||
          archiveSize != checkpoint.archiveSize ||
          mtimeNs != checkpoint.archiveMtimeNs) {
        return false;
      }
      if (!previousBatch.innerPaths.empty() &&
          checkpointInnerPathText(previousBatch.innerPaths.back()) !=
              checkpoint.lastInnerPath) {
        return false;
      }
    } else {
      if (nextIndex >= archiveBatchOrder.size()) {
        return false;
      }
      const auto batchIt = archiveBatches.find(archiveBatchOrder[nextIndex]);
      if (batchIt == archiveBatches.end()) {
        return false;
      }
      const ArchiveParseBatch &batch = batchIt->second;
      if (subIndex > batch.innerPaths.size()) {
        return false;
      }
      if (checkpointPathTextForDb(batch.archivePath) !=
          checkpointPathTextForDb(checkpoint.archivePath)) {
        return false;
      }
      sqlite3_int64 archiveSize = 0;
      sqlite3_int64 mtimeNs = 0;
      if (!archiveFileStateForDb(batch.archivePath, archiveSize, mtimeNs) ||
          archiveSize != checkpoint.archiveSize ||
          mtimeNs != checkpoint.archiveMtimeNs) {
        return false;
      }
      if (checkpointInnerPathText(batch.innerPaths[subIndex - 1]) !=
          checkpoint.lastInnerPath) {
        return false;
      }
    }

    resumePlan.valid = true;
    resumePlan.archivePhase = true;
    resumePlan.individualStart = individualDiffs.size();
    resumePlan.archiveStart = nextIndex;
    resumePlan.archiveSubStart = subIndex;
    for (std::size_t i = 0; i < nextIndex && i < archiveBatchOrder.size();
         ++i) {
      resumePlan.protectedArchiveKeys.insert(archiveBatchOrder[i]);
    }
    if (subIndex > 0 && nextIndex < archiveBatchOrder.size()) {
      resumePlan.protectedArchiveKeys.insert(archiveBatchOrder[nextIndex]);
    }
    const std::size_t resumedCount =
        individualDiffs.size() + archiveBatchInnerCountBefore(nextIndex) +
        subIndex;
    parseCurrent = static_cast<int>(std::min<std::size_t>(
        resumedCount, static_cast<std::size_t>(parseTotal)));
    return true;
  };

  if (checkpoint.found && checkpoint.scanSignature == scanSignature &&
      ((checkpoint.phase == kScanCheckpointPhaseIndividual &&
        validateIndividualCheckpoint()) ||
       (checkpoint.phase == kScanCheckpointPhaseArchive &&
        validateArchiveCheckpoint()))) {
    archive_file::appendDebugLogLine(
        "Continuing chart scan from checkpoint: phase=" + checkpoint.phase +
        " nextIndex=" + std::to_string(checkpoint.nextIndex) +
        " subIndex=" + std::to_string(checkpoint.subIndex));
  } else if (checkpoint.found) {
    clearChartScanCheckpoint(db);
    archive_file::appendDebugLogLine(
        "Discarded stale chart scan checkpoint before parsing.");
  }

  int changedCount = 0;
  bool transactionOpen = false;
  beginSqliteTransaction(db, "chart scan");
  transactionOpen = true;
  std::uint64_t completedFlushRequest = 0;

  auto makeCheckpoint = [&](const std::string &signature,
                            const std::string &phase, std::size_t nextIndex,
                            std::size_t subIndex,
                            const std::filesystem::path &lastPath,
                            const std::filesystem::path &archivePath,
                            const std::string &lastInnerPath) {
    ChartScanCheckpoint nextCheckpoint;
    nextCheckpoint.found = true;
    nextCheckpoint.scanSignature = signature;
    nextCheckpoint.phase = phase;
    nextCheckpoint.nextIndex =
        static_cast<int>(std::min<std::size_t>(
            nextIndex, static_cast<std::size_t>(std::numeric_limits<int>::max())));
    nextCheckpoint.subIndex =
        static_cast<int>(std::min<std::size_t>(
            subIndex, static_cast<std::size_t>(std::numeric_limits<int>::max())));
    nextCheckpoint.lastPath = lastPath;
    nextCheckpoint.archivePath = archivePath;
    nextCheckpoint.lastInnerPath = lastInnerPath;
    if (!archivePath.empty()) {
      archiveFileStateForDb(archivePath, nextCheckpoint.archiveSize,
                            nextCheckpoint.archiveMtimeNs);
    }
    return nextCheckpoint;
  };

  auto saveCheckpoint = [&](const ChartScanCheckpoint &nextCheckpoint) {
    if (transactionOpen) {
      commitSqliteTransaction(db, "chart scan");
      transactionOpen = false;
    }
    if (!upsertChartScanCheckpoint(db, nextCheckpoint)) {
      archive_file::appendDebugLogLine(
          "Failed to save chart scan checkpoint: phase=" +
          nextCheckpoint.phase +
          " nextIndex=" + std::to_string(nextCheckpoint.nextIndex) +
          " subIndex=" + std::to_string(nextCheckpoint.subIndex));
    }
    beginSqliteTransaction(db, "chart scan");
    transactionOpen = true;
  };

  auto pendingFlushRequest = [&]() -> std::uint64_t {
    if (flushRequestCallback == nullptr) {
      return 0;
    }
    return flushRequestCallback();
  };

  auto acknowledgeFlushRequest = [&](std::uint64_t request) {
    if (request == 0 || request <= completedFlushRequest) {
      return;
    }
    completedFlushRequest = request;
    if (flushCompleteCallback != nullptr) {
      flushCompleteCallback(request);
    }
  };

  auto checkpointSaveRequest = [&](bool force)
      -> std::optional<std::uint64_t> {
    const std::uint64_t request = pendingFlushRequest();
    if (!force && request <= completedFlushRequest) {
      return std::nullopt;
    }
    return request;
  };

  auto saveCheckpointForFlush = [&](const ChartScanCheckpoint &nextCheckpoint,
                                    std::uint64_t request) {
    saveCheckpoint(nextCheckpoint);
    acknowledgeFlushRequest(request);
  };

  auto archiveDeleteProtectedByCheckpoint =
      [&](const std::filesystem::path &archivePath) {
        if (!resumePlan.valid || !resumePlan.archivePhase) {
          return false;
        }
        const path_t archiveKey = fspath_to_path_t(archivePath);
        return resumePlan.protectedArchiveKeys.find(archiveKey) !=
               resumePlan.protectedArchiveKeys.end();
      };

  for (const auto &path : sourcePreferenceRefreshPaths) {
    if (shouldStop()) {
      break;
    }
    if (updateChartSourcePreference(db, path)) {
      ++changedCount;
    }
  }

  for (const auto &update : cachedSourcePreferenceUpdates) {
    if (shouldStop()) {
      break;
    }
    if (updateChartSourcePreferenceValues(db, update.path, update.priority,
                                          update.archiveSize)) {
      ++changedCount;
    }
  }

  for (const auto &path : staleSolidArchives) {
    if (shouldStop()) {
      break;
    }
    if (deleteSolidArchive(db, path)) {
      ++changedCount;
    }
    if (deleteArchiveScanCache(db, path)) {
      ++changedCount;
    }
  }

  for (const auto &path : reindexedArchives) {
    if (shouldStop()) {
      break;
    }
    if (archiveDeleteProtectedByCheckpoint(path)) {
      archive_file::appendDebugLogLine(
          "Skipping archive chart delete for checkpoint-protected archive: " +
          checkpointPathTextForDb(path));
      continue;
    }
    if (deleteChartMetaInArchive(db, path)) {
      ++changedCount;
    }
  }

  for (const auto &diff : archiveCacheDiffs) {
    if (shouldStop()) {
      break;
    }
    if (upsertArchiveScanCache(db, diff.path, diff.solid,
                               diff.uncompressedSize, diff.fileCount,
                               diff.chartCount)) {
      ++changedCount;
    }
  }

  for (const auto &diff : solidArchiveDiffs) {
    if (shouldStop()) {
      break;
    }
    if (diff.solid) {
      if (upsertSolidArchive(db, diff.path, diff.uncompressedSize,
                             diff.fileCount)) {
        ++changedCount;
      }
      if (deleteChartMetaInArchive(db, diff.path)) {
        ++changedCount;
      }
    } else if (deleteSolidArchive(db, diff.path)) {
      ++changedCount;
    }
  }

  auto parseAndInsertChart =
      [&](const std::filesystem::path &path,
          const std::vector<unsigned char> *bytes) -> bool {
    const std::string chartText = fspath_to_utf8(path);
    bms_parser::Parser parser;
    bms_parser::Chart *rawChart = nullptr;
    std::unique_ptr<bms_parser::Chart> chart;
    std::atomic_bool cancelled(false);
    try {
      if (bytes != nullptr) {
        parser.Parse(*bytes, &rawChart, false, false, cancelled);
        chart.reset(rawChart);
        rawChart = nullptr;
        if (chart != nullptr) {
          chart->Meta.BmsPath = path;
          std::filesystem::path archivePath;
          std::filesystem::path innerPath;
          if (archive_file::splitVirtualPath(path, archivePath, innerPath)) {
            chart->Meta.Folder =
                archive_file::makeVirtualPath(archivePath,
                                              innerPath.parent_path());
          }
        }
      } else {
        archive_file::parseChart(parser, path, &rawChart, false, false,
                                 cancelled);
        chart.reset(rawChart);
        rawChart = nullptr;
      }
    } catch (const std::exception &e) {
      if (chart == nullptr && rawChart != nullptr) {
        chart.reset(rawChart);
        rawChart = nullptr;
      }
      SDL_Log("Error parsing %s: %s", chartText.c_str(), e.what());
      archive_file::appendDebugLogLine(
          "DB parse failed: " + chartText + ": " + e.what());
      return false;
    }

    if (chart == nullptr) {
      archive_file::appendDebugLogLine(
          "DB parse returned null: " + chartText);
      return false;
    }
    if (!parsedChartLooksInsertable(*chart)) {
      SDL_Log("Skipping chart without measures or stable identity: %s",
              chartText.c_str());
      archive_file::appendDebugLogLine(
          "DB skipped chart: " + chartText +
          " measures=" + std::to_string(chart->Measures.size()) +
          " md5=" + chart->Meta.MD5 +
          " sha256=" + chart->Meta.SHA256);
      return false;
    }
    const bool inserted = InsertChartMeta(db, chart->Meta);
    return inserted;
  };

  const std::size_t individualStartIndex =
      resumePlan.valid ? resumePlan.individualStart : 0;
  for (std::size_t diffIndex = individualStartIndex;
       diffIndex < individualDiffs.size(); ++diffIndex) {
    const auto &diff = individualDiffs[diffIndex];
    if (shouldStop()) {
      break;
    }
    reportProgress(parseCurrent, parseTotal,
                   diff.deleted ? ChartScanProgressStage::RemovingDeleted
                                : ChartScanProgressStage::ParsingCharts);
    if (diff.deleted) {
      if (DeleteChartMeta(db, diff.path)) {
        ++changedCount;
      }
    } else if (parseAndInsertChart(diff.path, nullptr)) {
      ++changedCount;
    }
    ++parseCurrent;
    const std::size_t nextIndex = diffIndex + 1;
    const auto checkpointRequest = checkpointSaveRequest(
        nextIndex % kIndividualParseCheckpointInterval == 0);
    if (checkpointRequest.has_value()) {
      saveCheckpointForFlush(
          makeCheckpoint(computeScanSignature(nextIndex, 0),
                         kScanCheckpointPhaseIndividual, 0, 0, {}, {}, ""),
          *checkpointRequest);
    }
  }

  if (!shouldStop() && !archiveBatchOrder.empty() &&
      (!resumePlan.valid || !resumePlan.archivePhase)) {
    const std::filesystem::path lastPath =
        individualDiffs.empty() ? std::filesystem::path()
                                : individualDiffs.back().path;
    const auto checkpointRequest = checkpointSaveRequest(true);
    if (checkpointRequest.has_value()) {
      saveCheckpointForFlush(makeCheckpoint(
          computeScanSignature(individualDiffs.size(), 0),
          kScanCheckpointPhaseArchive, 0, 0, lastPath, {}, ""),
          *checkpointRequest);
    }
  }

  const std::size_t archiveStartIndex =
      resumePlan.valid && resumePlan.archivePhase ? resumePlan.archiveStart : 0;
  const std::size_t archiveSubStartIndex =
      resumePlan.valid && resumePlan.archivePhase ? resumePlan.archiveSubStart
                                                  : 0;
  auto writePendingArchiveCache = [&](const ArchiveParseBatch &batch) {
    const auto cacheIt =
        pendingArchiveCacheDiffs.find(fspath_to_path_t(batch.archivePath));
    if (cacheIt == pendingArchiveCacheDiffs.end()) {
      return;
    }
    const ArchiveCacheDiff &diff = cacheIt->second;
    if (upsertArchiveScanCache(db, diff.path, diff.solid,
                               diff.uncompressedSize, diff.fileCount,
                               diff.chartCount)) {
      ++changedCount;
    }
  };

  for (std::size_t archiveIndex = archiveStartIndex;
       archiveIndex < archiveBatchOrder.size(); ++archiveIndex) {
    if (shouldStop()) {
      break;
    }
    const auto &archiveKey = archiveBatchOrder[archiveIndex];
    const auto batchIt = archiveBatches.find(archiveKey);
    if (batchIt == archiveBatches.end()) {
      continue;
    }
    const ArchiveParseBatch &batch = batchIt->second;
    const std::size_t innerStart =
        archiveIndex == archiveStartIndex ? archiveSubStartIndex : 0;
    if (innerStart >= batch.innerPaths.size()) {
      writePendingArchiveCache(batch);
      const std::filesystem::path lastPath =
          batch.innerPaths.empty()
              ? std::filesystem::path()
              : archive_file::makeVirtualPath(batch.archivePath,
                                              batch.innerPaths.back());
      const std::string lastInnerPath =
          batch.innerPaths.empty()
              ? ""
              : checkpointInnerPathText(batch.innerPaths.back());
      const auto checkpointRequest = checkpointSaveRequest(true);
      if (checkpointRequest.has_value()) {
        saveCheckpointForFlush(makeCheckpoint(
            computeScanSignature(individualDiffs.size(), archiveIndex + 1),
            kScanCheckpointPhaseArchive, 0, 0, lastPath, batch.archivePath,
            lastInnerPath), *checkpointRequest);
      }
      continue;
    }
    std::vector<std::filesystem::path> pendingInnerPaths(
        batch.innerPaths.begin() +
            static_cast<std::vector<std::filesystem::path>::difference_type>(
                innerStart),
        batch.innerPaths.end());
    std::vector<archive_file::FileData> files;
    std::string errorMessage;
    const std::string archiveText = fspath_to_utf8(batch.archivePath);
    reportProgress(parseCurrent, parseTotal,
                   ChartScanProgressStage::ReadingArchive);
    if (!archive_file::readArchiveEntries(batch.archivePath, pendingInnerPaths,
                                          files, &errorMessage,
                                          pauseCallback)) {
      if (!errorMessage.empty()) {
        SDL_Log("Failed to read charts from archive %s: %s",
                archiveText.c_str(), errorMessage.c_str());
        archive_file::appendDebugLogLine(
            "Failed to read DB chart batch: " + archiveText + ": " +
            errorMessage);
      }
      parseCurrent += static_cast<int>(pendingInnerPaths.size());
      continue;
    }
    archive_file::appendDebugLogLine(
        "Parsing DB chart batch: " + archiveText +
        " requested=" + std::to_string(pendingInnerPaths.size()) +
        " files=" + std::to_string(files.size()));
    bool parsedFullBatch = files.size() == pendingInnerPaths.size();
    bool checkpointOrderReliable = true;
    std::size_t parsedInBatch = innerStart;
    for (const auto &file : files) {
      if (shouldStop()) {
        parsedFullBatch = false;
        break;
      }
      const std::filesystem::path chartPath =
          archive_file::makeVirtualPath(batch.archivePath, file.path);
      reportProgress(parseCurrent, parseTotal,
                     ChartScanProgressStage::ParsingCharts);
      if (parseAndInsertChart(chartPath, &file.bytes)) {
        ++changedCount;
      }
      ++parseCurrent;
      if (parsedInBatch >= batch.innerPaths.size() ||
          checkpointInnerPathText(batch.innerPaths[parsedInBatch]) !=
              checkpointInnerPathText(file.path)) {
        checkpointOrderReliable = false;
      }
      ++parsedInBatch;
      if (checkpointOrderReliable) {
        const auto checkpointRequest = checkpointSaveRequest(
            parsedInBatch % kArchiveParseCheckpointInterval == 0);
        if (checkpointRequest.has_value()) {
          saveCheckpointForFlush(
              makeCheckpoint(
                  computeScanSignature(individualDiffs.size(), archiveIndex),
                  kScanCheckpointPhaseArchive, 0, parsedInBatch, chartPath,
                  batch.archivePath, checkpointInnerPathText(file.path)),
              *checkpointRequest);
        }
      }
    }
    if (parsedFullBatch && !stopRequested(stopToken)) {
      writePendingArchiveCache(batch);
      const std::filesystem::path lastPath =
          batch.innerPaths.empty()
              ? std::filesystem::path()
              : archive_file::makeVirtualPath(batch.archivePath,
                                              batch.innerPaths.back());
      const std::string lastInnerPath =
          batch.innerPaths.empty()
              ? ""
              : checkpointInnerPathText(batch.innerPaths.back());
      const auto checkpointRequest = checkpointSaveRequest(true);
      if (checkpointRequest.has_value()) {
        saveCheckpointForFlush(makeCheckpoint(
            computeScanSignature(individualDiffs.size(), archiveIndex + 1),
            kScanCheckpointPhaseArchive, 0, 0, lastPath, batch.archivePath,
            lastInnerPath), *checkpointRequest);
      }
    } else {
      archive_file::appendDebugLogLine(
          "Skipped archive scan cache write because chart batch did not "
          "complete: " + archiveText +
          " requested=" + std::to_string(pendingInnerPaths.size()) +
          " files=" + std::to_string(files.size()));
    }
  }
  if (transactionOpen) {
    commitSqliteTransaction(db, "chart scan");
    transactionOpen = false;
  }
  acknowledgeFlushRequest(pendingFlushRequest());
  if (!stopRequested(stopToken)) {
    clearChartScanCheckpoint(db);
    clearChartMetadataRebuildRequiredIfPresent(db);
  }

  if (changedCount > 0) {
    bumpLibraryRevision();
  }
  return changedCount;
}

bool ChartDBHelper::CreateDifficultyTableTables(sqlite3 *db) {
  const char *createTables[] = {
      "CREATE TABLE IF NOT EXISTS difficulty_tables ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "name TEXT NOT NULL,"
      "symbol TEXT NOT NULL,"
      "data_url TEXT NOT NULL DEFAULT '',"
      "source_url TEXT NOT NULL DEFAULT '',"
      "updated_at TEXT,"
      "UNIQUE(name, symbol, source_url)"
      ")",
      "CREATE TABLE IF NOT EXISTS difficulty_table_entries ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "table_id INTEGER NOT NULL,"
      "level TEXT NOT NULL DEFAULT '',"
      "md5 TEXT NOT NULL DEFAULT '',"
      "sha256 TEXT NOT NULL DEFAULT '',"
      "title TEXT,"
      "subtitle TEXT,"
      "artist TEXT,"
      "subartist TEXT,"
      "url TEXT,"
      "url_diff TEXT,"
      "sort_order INTEGER NOT NULL DEFAULT 0"
      ")",
      "CREATE TABLE IF NOT EXISTS difficulty_courses ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "table_id INTEGER NOT NULL,"
      "name TEXT NOT NULL,"
      "group_name TEXT NOT NULL DEFAULT '',"
      "level TEXT NOT NULL DEFAULT '',"
      "constraint_json TEXT NOT NULL DEFAULT '[]',"
      "sort_order INTEGER NOT NULL DEFAULT 0"
      ")",
      "CREATE TABLE IF NOT EXISTS difficulty_course_entries ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "course_id INTEGER NOT NULL,"
      "level TEXT NOT NULL DEFAULT '',"
      "md5 TEXT NOT NULL DEFAULT '',"
      "sha256 TEXT NOT NULL DEFAULT '',"
      "title TEXT,"
      "subtitle TEXT,"
      "artist TEXT,"
      "subartist TEXT,"
      "url TEXT,"
      "url_diff TEXT,"
      "sort_order INTEGER NOT NULL DEFAULT 0"
      ")",
      "CREATE INDEX IF NOT EXISTS idx_difficulty_entries_table_level "
      "ON difficulty_table_entries(table_id, level)",
      "CREATE INDEX IF NOT EXISTS idx_difficulty_entries_table_sort_order "
      "ON difficulty_table_entries(table_id, sort_order)",
      "CREATE INDEX IF NOT EXISTS idx_difficulty_entries_table_level_sort_order "
      "ON difficulty_table_entries(table_id, level, sort_order)",
      "CREATE INDEX IF NOT EXISTS idx_difficulty_entries_table_sha256 "
      "ON difficulty_table_entries(table_id, sha256)",
      "CREATE INDEX IF NOT EXISTS idx_difficulty_entries_table_md5 "
      "ON difficulty_table_entries(table_id, md5)",
      "CREATE INDEX IF NOT EXISTS idx_difficulty_entries_md5 "
      "ON difficulty_table_entries(md5)",
      "CREATE INDEX IF NOT EXISTS idx_difficulty_entries_sha256 "
      "ON difficulty_table_entries(sha256)",
      "CREATE INDEX IF NOT EXISTS idx_difficulty_courses_group "
      "ON difficulty_courses(table_id, group_name)",
      "CREATE INDEX IF NOT EXISTS idx_difficulty_course_entries_course "
      "ON difficulty_course_entries(course_id)",
      "CREATE INDEX IF NOT EXISTS idx_difficulty_course_entries_course_sha256 "
      "ON difficulty_course_entries(course_id, sha256)",
      "CREATE INDEX IF NOT EXISTS idx_difficulty_course_entries_course_md5 "
      "ON difficulty_course_entries(course_id, md5)",
      "CREATE INDEX IF NOT EXISTS idx_difficulty_course_entries_md5 "
      "ON difficulty_course_entries(md5)",
      "CREATE INDEX IF NOT EXISTS idx_difficulty_course_entries_sha256 "
      "ON difficulty_course_entries(sha256)",
  };

  for (const auto *query : createTables) {
    if (!execSql(db, query, "creating difficulty table schema")) {
      return false;
    }
  }
  const char *courseEntryMigrations[] = {
      "ALTER TABLE difficulty_course_entries ADD COLUMN title TEXT",
      "ALTER TABLE difficulty_course_entries ADD COLUMN subtitle TEXT",
      "ALTER TABLE difficulty_course_entries ADD COLUMN artist TEXT",
      "ALTER TABLE difficulty_course_entries ADD COLUMN subartist TEXT",
      "ALTER TABLE difficulty_course_entries ADD COLUMN url TEXT",
      "ALTER TABLE difficulty_course_entries ADD COLUMN url_diff TEXT",
  };
  for (const auto *query : courseEntryMigrations) {
    if (!execSqlAllowDuplicateColumn(
            db, query, "migrating difficulty course entry schema")) {
      return false;
    }
  }
  return true;
}

bool ChartDBHelper::ImportDifficultyTable(sqlite3 *db,
                                          const std::string &headerJson,
                                          const std::string &dataJson,
                                          const std::string &sourceUrl) {
  if (!CreateDifficultyTableTables(db)) {
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

  invalidateDifficultyLabelCache();
  std::string transactionError;
  SqliteTransactionHandle transaction(db, "BEGIN", transactionError);
  if (!transaction.active()) {
    SDL_Log("Failed to start difficulty table import transaction: %s",
            transactionError.c_str());
    return false;
  }
  const int tableId =
      upsertDifficultyTable(db, name, symbol, dataUrl, sourceUrl);
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
    insertDifficultyTableEntry(db, tableId, chart, sortOrder++);
  }
  TableChartItemLookup chartLookup;
  chartLookup.reserve(chartItems.size() * 2);
  for (const auto &chart : chartItems) {
    addToChartItemLookup(chartLookup, chart);
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

    const int courseId =
        insertDifficultyCourse(db, tableId, courseName, groupName, level,
                               constraintJson, courseSortOrder++);
    if (courseId <= 0) {
      continue;
    }

    auto courseCharts = readCourseCharts(*course);
    int chartSortOrder = 0;
    for (auto &chart : courseCharts) {
      if (chart.md5.empty() && chart.sha256.empty()) {
        continue;
      }
      if (const auto *tableChart = findChartItemInLookup(chartLookup, chart)) {
        fillMissingCourseChartMetadata(chart, *tableChart);
      }
      insertDifficultyCourseEntry(db, courseId, chart, chartSortOrder++);
    }
  }

  if (!transaction.commit(transactionError)) {
    SDL_Log("Failed to commit difficulty table import transaction: %s",
            transactionError.c_str());
    return false;
  }
  bumpLibraryRevision();
  SDL_Log("Imported difficulty table %s (%s) from %s", name.c_str(),
          symbol.c_str(), sourceUrl.c_str());
  return true;
}

bool ChartDBHelper::ImportDifficultyTableFromUrl(sqlite3 *db,
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

bool ChartDBHelper::UpdateDifficultyTableFromSourceUrl(
    sqlite3 *db, int tableId, std::string *errorMessage) {
  if (!CreateDifficultyTableTables(db)) {
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

bool ChartDBHelper::DeleteDifficultyTable(sqlite3 *db, int tableId) {
  if (tableId <= 0 || !CreateDifficultyTableTables(db)) {
    return false;
  }

  invalidateDifficultyLabelCache();
  std::string transactionError;
  SqliteTransactionHandle transaction(db, "BEGIN", transactionError);
  if (!transaction.active()) {
    SDL_Log("Failed to start difficulty table delete transaction: %s",
            transactionError.c_str());
    return false;
  }
  if (!clearDifficultyTableContent(db, tableId)) {
    return false;
  }

  auto query = "DELETE FROM difficulty_tables WHERE id = @id";
  SqliteStatementHandle stmt;
  int rc = prepareSqliteStatement(db, query, stmt);
  if (rc != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_int(stmt.get(), 1, tableId);
  rc = sqlite3_step(stmt.get());
  const bool deleted = rc == SQLITE_DONE && sqlite3_changes(db) > 0;
  if (!deleted) {
    return false;
  }

  if (!transaction.commit(transactionError)) {
    SDL_Log("Failed to commit difficulty table delete transaction: %s",
            transactionError.c_str());
    return false;
  }
  bumpLibraryRevision();
  return true;
}

int ChartDBHelper::ImportDifficultyTablesFromDirectory(
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

std::vector<DifficultyTableInfo>
ChartDBHelper::SelectDifficultyTables(sqlite3 *db) {
  if (!CreateDifficultyTableTables(db)) {
    return {};
  }
  auto query = "SELECT dt.id, dt.name, dt.symbol, dt.source_url, "
               "COUNT(dte.id) "
               "FROM difficulty_tables dt "
               "LEFT JOIN difficulty_table_entries dte ON dte.table_id = dt.id "
               "GROUP BY dt.id "
               "ORDER BY dt.name COLLATE NOCASE";

  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt,
                                    "selecting difficulty tables",
                                    logSqlErrorText)) {
    return {};
  }

  std::vector<DifficultyTableInfo> tables;
  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    DifficultyTableInfo table;
    table.id = columnInt(stmt.get(), 0);
    table.name = columnString(stmt.get(), 1);
    table.symbol = columnString(stmt.get(), 2);
    table.sourceUrl = columnString(stmt.get(), 3);
    table.chartCount = columnInt(stmt.get(), 4);
    tables.push_back(std::move(table));
  }
  return tables;
}

std::vector<DifficultyLevelInfo>
ChartDBHelper::SelectDifficultyLevels(sqlite3 *db, int tableId) {
  if (!CreateDifficultyTableTables(db)) {
    return {};
  }
  auto query = "SELECT dte.table_id, dt.name, dt.symbol, dte.level, "
               "COUNT(dte.id), MIN(dte.sort_order) "
               "FROM difficulty_table_entries dte "
               "JOIN difficulty_tables dt ON dt.id = dte.table_id "
               "WHERE dte.table_id = @table_id "
               "GROUP BY dte.table_id, dte.level "
               "ORDER BY MIN(dte.sort_order), dte.level";
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt,
                                    "selecting difficulty levels",
                                    logSqlErrorText)) {
    return {};
  }
  sqlite3_bind_int(stmt.get(), 1, tableId);

  std::vector<DifficultyLevelInfo> levels;
  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    DifficultyLevelInfo level;
    level.tableId = columnInt(stmt.get(), 0);
    level.tableName = columnString(stmt.get(), 1);
    level.tableSymbol = columnString(stmt.get(), 2);
    level.level = columnString(stmt.get(), 3);
    level.chartCount = columnInt(stmt.get(), 4);
    levels.push_back(std::move(level));
  }
  return levels;
}

std::vector<DifficultyCourseGroupInfo>
ChartDBHelper::SelectDifficultyCourseGroups(sqlite3 *db) {
  if (!CreateDifficultyTableTables(db)) {
    return {};
  }
  std::string query =
      "SELECT dc.table_id, dt.name, dc.group_name, "
      "COUNT(dce.id), SUM(CASE WHEN cm.path IS NULL THEN 0 ELSE 1 END), "
      "MIN(dc.sort_order) "
      "FROM difficulty_courses dc "
      "JOIN difficulty_tables dt ON dt.id = dc.table_id "
      "LEFT JOIN difficulty_course_entries dce ON dce.course_id = dc.id "
      "LEFT JOIN chart_meta cm ON cm.path = ("
      "SELECT cm_match.path FROM chart_meta cm_match "
      "WHERE ((dce.sha256 != '' AND cm_match.sha256 = dce.sha256) "
      "OR (dce.md5 != '' AND cm_match.md5 = dce.md5)) ";
  query += "ORDER BY ";
  query += chartSourceOrderBy("cm_match");
  query += ", cm_match.title COLLATE NOCASE LIMIT 1) "
      "GROUP BY dc.table_id, dc.group_name "
      "ORDER BY dt.name COLLATE NOCASE, MIN(dc.sort_order)";

  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt,
                                    "selecting difficulty course groups",
                                    logSqlErrorText)) {
    return {};
  }

  std::vector<DifficultyCourseGroupInfo> groups;
  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    DifficultyCourseGroupInfo group;
    group.tableId = columnInt(stmt.get(), 0);
    group.tableName = columnString(stmt.get(), 1);
    group.groupName = columnString(stmt.get(), 2);
    group.chartCount = columnInt(stmt.get(), 3);
    group.matchedChartCount = columnInt(stmt.get(), 4);
    groups.push_back(std::move(group));
  }
  return groups;
}

std::vector<DifficultyCourseInfo>
ChartDBHelper::SelectDifficultyCourses(sqlite3 *db, int tableId,
                                       const std::string &groupName) {
  if (!CreateDifficultyTableTables(db)) {
    return {};
  }
  std::string query =
      "SELECT dc.id, dc.table_id, dt.name, dc.group_name, dc.level, dc.name, "
      "dc.constraint_json, COUNT(dce.id), "
      "SUM(CASE WHEN cm.path IS NULL THEN 0 ELSE 1 END) "
      "FROM difficulty_courses dc "
      "JOIN difficulty_tables dt ON dt.id = dc.table_id "
      "LEFT JOIN difficulty_course_entries dce ON dce.course_id = dc.id "
      "LEFT JOIN chart_meta cm ON cm.path = ("
      "SELECT cm_match.path FROM chart_meta cm_match "
      "WHERE ((dce.sha256 != '' AND cm_match.sha256 = dce.sha256) "
      "OR (dce.md5 != '' AND cm_match.md5 = dce.md5)) ";
  query += "ORDER BY ";
  query += chartSourceOrderBy("cm_match");
  query += ", cm_match.title COLLATE NOCASE LIMIT 1) "
      "WHERE dc.table_id = @table_id AND dc.group_name = @group_name "
      "GROUP BY dc.id "
      "ORDER BY dc.sort_order";

  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt,
                                    "selecting difficulty courses",
                                    logSqlErrorText)) {
    return {};
  }
  sqlite3_bind_int(stmt.get(), 1, tableId);
  bindSqliteText(stmt.get(), 2, groupName);

  std::vector<DifficultyCourseInfo> courses;
  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    DifficultyCourseInfo course;
    course.id = columnInt(stmt.get(), 0);
    course.tableId = columnInt(stmt.get(), 1);
    course.tableName = columnString(stmt.get(), 2);
    course.groupName = columnString(stmt.get(), 3);
    course.level = columnString(stmt.get(), 4);
    course.name = columnString(stmt.get(), 5);
    course.constraintJson = columnString(stmt.get(), 6);
    course.chartCount = columnInt(stmt.get(), 7);
    course.matchedChartCount = columnInt(stmt.get(), 8);
    courses.push_back(std::move(course));
  }
  return courses;
}

ChartDBHelper::ChartDBHelper() {
  archive_file::setCachePathNormalizer([](std::filesystem::path &path) {
    ChartDBHelper::ToRelativePath(path);
  });
}

std::uint64_t ChartDBHelper::GetLibraryRevision() const {
  return gLibraryRevision.load(std::memory_order_relaxed);
}

void ChartDBHelper::ToRelativePath(
    [[maybe_unused]] std::filesystem::path &path) {
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  if (path.empty() || path.is_relative()) {
    return;
  }

  if (auto relative = relativeToCurrentDocumentsPath(path)) {
    path = storedDocumentsPath(*relative);
  }
#endif
}

void ChartDBHelper::ToAbsolutePath(
    [[maybe_unused]] std::filesystem::path &path) {
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  if (path.empty()) {
    return;
  }

  if (path.is_absolute()) {
    if (auto relative = relativeToCurrentDocumentsPath(path)) {
      path = Utils::GetDocumentsPath() / *relative;
    }
    return;
  }

  if (auto relative = relativeFromStoredDocumentsPath(path)) {
    path = Utils::GetDocumentsPath() / *relative;
  } else {
    path = Utils::GetDocumentsPath("BMS") / path;
  }
#endif
}
