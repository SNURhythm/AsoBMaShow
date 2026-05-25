// Fill out your copyright notice in the Description page of Project Settings.

#include "ChartDBHelper.h"
#include "Utils.h"
#include <SDL2/SDL.h>
#include "path.h"

#include <algorithm>
#include <cctype>
#include <codecvt>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include "../yoga/lib/nlohmann/json.hpp"
#include <optional>
#include <regex>
#include <sstream>
#include <unordered_set>
#include "targets.h"
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
#include "iOSNatives.hpp"
#endif

namespace {
using json = nlohmann::json;

constexpr const char *kChartMetaSelectColumns = "cm.path,"
                                                "cm.md5,"
                                                "cm.sha256,"
                                                "cm.title,"
                                                "cm.subtitle,"
                                                "cm.genre,"
                                                "cm.artist,"
                                                "cm.sub_artist,"
                                                "cm.folder,"
                                                "cm.stage_file,"
                                                "cm.banner,"
                                                "cm.back_bmp,"
                                                "cm.preview,"
                                                "cm.level,"
                                                "cm.difficulty,"
                                                "cm.total,"
                                                "cm.bpm,"
                                                "cm.max_bpm,"
                                                "cm.min_bpm,"
                                                "cm.length,"
                                                "cm.rank,"
                                                "cm.player,"
                                                "cm.keys,"
                                                "cm.total_notes,"
                                                "cm.total_long_notes,"
                                                "cm.total_scratch_notes,"
                                                "cm.total_backspin_notes";

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

std::string normalizedHash(const std::string &value) {
  return lowerCopy(trimCopy(value));
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

std::string shellQuote(const std::string &value) {
#ifdef _WIN32
  std::string quoted = "\"";
  for (char c : value) {
    if (c == '"' || c == '\\') {
      quoted.push_back('\\');
    }
    quoted.push_back(c);
  }
  quoted.push_back('"');
  return quoted;
#else
  std::string quoted = "'";
  for (char c : value) {
    if (c == '\'') {
      quoted += "'\\''";
    } else {
      quoted.push_back(c);
    }
  }
  quoted.push_back('\'');
  return quoted;
#endif
}

std::filesystem::path makeTempDownloadPath(const std::string &extension) {
  const auto ticks =
      std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("asobmashow_table_" + std::to_string(ticks) + extension);
}

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
#else
  const auto outputPath = makeTempDownloadPath(".txt");
  std::string command;
#ifdef _WIN32
  command = "powershell -NoProfile -ExecutionPolicy Bypass -Command "
            "\"$ProgressPreference='SilentlyContinue'; "
            "Invoke-WebRequest -UseBasicParsing -MaximumRedirection 5 "
            "-TimeoutSec 25 -Uri " +
            shellQuote(url) + " -OutFile " + shellQuote(outputPath.string()) +
            "\"";
#else
  command = "curl -L --fail --silent --show-error --max-time 25 "
            "-A 'AsoBMaShow' -o " +
            shellQuote(outputPath.string()) + " " + shellQuote(url);
#endif

  const int rc = std::system(command.c_str());
  if (rc != 0) {
    if (errorMessage != nullptr) {
      *errorMessage = "Failed to download " + url;
    }
    std::filesystem::remove(outputPath);
    return std::nullopt;
  }

  auto body = readTextFile(outputPath);
  std::filesystem::remove(outputPath);
  if (!body && errorMessage != nullptr) {
    *errorMessage = "Downloaded file could not be read: " + url;
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

std::vector<TableChartItem> readCourseCharts(const json &course) {
  std::vector<TableChartItem> charts;
  if (!course.is_object()) {
    return charts;
  }

  const auto chartIt = course.find("charts");
  if (chartIt != course.end() && chartIt->is_array()) {
    for (const auto &chartValue : *chartIt) {
      if (chartValue.is_object()) {
        charts.push_back(readChartItem(chartValue, "0"));
      }
    }
  }

  const auto md5It = course.find("md5");
  if (md5It != course.end() && md5It->is_array()) {
    for (const auto &md5Value : *md5It) {
      const std::string md5 = normalizedHash(jsonValueToString(md5Value));
      if (!md5.empty()) {
        charts.push_back({.level = "0", .md5 = md5});
      }
    }
  }

  const auto sha256It = course.find("sha256");
  if (sha256It != course.end() && sha256It->is_array()) {
    for (const auto &sha256Value : *sha256It) {
      const std::string sha256 = normalizedHash(jsonValueToString(sha256Value));
      if (!sha256.empty()) {
        charts.push_back({.level = "0", .sha256 = sha256});
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
  return {courseName, ""};
}

bool execSql(sqlite3 *db, const char *query, const char *context) {
  char *errMsg = nullptr;
  const int rc = sqlite3_exec(db, query, nullptr, nullptr, &errMsg);
  if (rc != SQLITE_OK) {
    std::cerr << "SQL error while " << context << ": "
              << (errMsg != nullptr ? errMsg : sqlite3_errmsg(db)) << "\n";
    sqlite3_free(errMsg);
    return false;
  }
  return true;
}

bool bindText(sqlite3_stmt *stmt, int idx, const std::string &value) {
  return sqlite3_bind_text(stmt, idx, value.c_str(), -1, SQLITE_TRANSIENT) ==
         SQLITE_OK;
}

int findDifficultyTable(sqlite3 *db, const std::string &name,
                        const std::string &symbol,
                        const std::string &sourceUrl) {
  auto query =
      "SELECT id FROM difficulty_tables WHERE name = @name AND symbol = "
      "@symbol AND source_url = @source_url";
  sqlite3_stmt *stmt = nullptr;
  int rc = sqlite3_prepare_v2(db, query, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    std::cerr << "SQL error while looking up difficulty table: "
              << sqlite3_errmsg(db) << "\n";
    return 0;
  }
  bindText(stmt, 1, name);
  bindText(stmt, 2, symbol);
  bindText(stmt, 3, sourceUrl);
  int id = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    id = sqlite3_column_int(stmt, 0);
  }
  sqlite3_finalize(stmt);
  return id;
}

bool clearDifficultyTableContent(sqlite3 *db, int tableId) {
  sqlite3_stmt *stmt = nullptr;
  auto deleteCourseEntries =
      "DELETE FROM difficulty_course_entries WHERE course_id IN "
      "(SELECT id FROM difficulty_courses WHERE table_id = @table_id)";
  int rc = sqlite3_prepare_v2(db, deleteCourseEntries, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_int(stmt, 1, tableId);
  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    return false;
  }

  auto deleteCourses =
      "DELETE FROM difficulty_courses WHERE table_id = @table_id";
  rc = sqlite3_prepare_v2(db, deleteCourses, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_int(stmt, 1, tableId);
  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    return false;
  }

  auto deleteEntries =
      "DELETE FROM difficulty_table_entries WHERE table_id = @table_id";
  rc = sqlite3_prepare_v2(db, deleteEntries, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_int(stmt, 1, tableId);
  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE;
}

int upsertDifficultyTable(sqlite3 *db, const std::string &name,
                          const std::string &symbol, const std::string &dataUrl,
                          const std::string &sourceUrl) {
  int tableId = findDifficultyTable(db, name, symbol, sourceUrl);
  if (tableId > 0) {
    auto updateQuery =
        "UPDATE difficulty_tables SET data_url = @data_url, updated_at = "
        "CURRENT_TIMESTAMP WHERE id = @id";
    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, updateQuery, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
      return 0;
    }
    bindText(stmt, 1, dataUrl);
    sqlite3_bind_int(stmt, 2, tableId);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE || !clearDifficultyTableContent(db, tableId)) {
      return 0;
    }
    return tableId;
  }

  auto insertQuery =
      "INSERT INTO difficulty_tables "
      "(name, symbol, data_url, source_url, updated_at) "
      "VALUES (@name, @symbol, @data_url, @source_url, CURRENT_TIMESTAMP)";
  sqlite3_stmt *stmt = nullptr;
  int rc = sqlite3_prepare_v2(db, insertQuery, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    std::cerr << "SQL error while inserting difficulty table: "
              << sqlite3_errmsg(db) << "\n";
    return 0;
  }
  bindText(stmt, 1, name);
  bindText(stmt, 2, symbol);
  bindText(stmt, 3, dataUrl);
  bindText(stmt, 4, sourceUrl);
  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
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
  sqlite3_stmt *stmt = nullptr;
  int rc = sqlite3_prepare_v2(db, query, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_int(stmt, 1, tableId);
  bindText(stmt, 2, chart.level);
  bindText(stmt, 3, chart.md5);
  bindText(stmt, 4, chart.sha256);
  bindText(stmt, 5, chart.title);
  bindText(stmt, 6, chart.subtitle);
  bindText(stmt, 7, chart.artist);
  bindText(stmt, 8, chart.subartist);
  bindText(stmt, 9, chart.url);
  bindText(stmt, 10, chart.urlDiff);
  sqlite3_bind_int(stmt, 11, sortOrder);
  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
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
  sqlite3_stmt *stmt = nullptr;
  int rc = sqlite3_prepare_v2(db, query, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return 0;
  }
  sqlite3_bind_int(stmt, 1, tableId);
  bindText(stmt, 2, name);
  bindText(stmt, 3, groupName);
  bindText(stmt, 4, level);
  bindText(stmt, 5, constraintJson);
  sqlite3_bind_int(stmt, 6, sortOrder);
  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    return 0;
  }
  return static_cast<int>(sqlite3_last_insert_rowid(db));
}

bool insertDifficultyCourseEntry(sqlite3 *db, int courseId,
                                 const TableChartItem &chart, int sortOrder) {
  auto query = "INSERT INTO difficulty_course_entries "
               "(course_id, level, md5, sha256, sort_order) "
               "VALUES (@course_id, @level, @md5, @sha256, @sort_order)";
  sqlite3_stmt *stmt = nullptr;
  int rc = sqlite3_prepare_v2(db, query, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_int(stmt, 1, courseId);
  bindText(stmt, 2, chart.level);
  bindText(stmt, 3, chart.md5);
  bindText(stmt, 4, chart.sha256);
  sqlite3_bind_int(stmt, 5, sortOrder);
  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE;
}

int columnInt(sqlite3_stmt *stmt, int idx) {
  return sqlite3_column_type(stmt, idx) == SQLITE_NULL
             ? 0
             : sqlite3_column_int(stmt, idx);
}

std::string columnString(sqlite3_stmt *stmt, int idx) {
  if (sqlite3_column_type(stmt, idx) == SQLITE_NULL) {
    return "";
  }
  return reinterpret_cast<const char *>(sqlite3_column_text(stmt, idx));
}
} // namespace

sqlite3 *ChartDBHelper::Connect() {
  std::filesystem::path Directory = Utils::GetDocumentsPath("db");
  std::cout << "DB Directory: " << Directory.string() << "\n";
  std::filesystem::create_directories(Directory);
  std::filesystem::path path = Directory / "chart.db";
  std::cout << "DB Path: " << path.string() << "\n";
  sqlite3 *db;
  int rc;
  rc = sqlite3_open(path.string().c_str(), &db);
  sqlite3_busy_timeout(db, 1000);
  if (rc) {
    std::cerr << "Can't open database: " << sqlite3_errmsg(db) << "\n";
    sqlite3_close(db);
    return nullptr;
  }
  // wal
  sqlite3_exec(db, "PRAGMA journal_mode=WAL", nullptr, nullptr, nullptr);
  sqlite3_exec(db, "PRAGMA synchronous=NORMAL", nullptr, nullptr, nullptr);
  return db;
}

void ChartDBHelper::Close(sqlite3 *db) { sqlite3_close(db); }

void ChartDBHelper::BeginTransaction(sqlite3 *db) {
  sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr);
}

void ChartDBHelper::CommitTransaction(sqlite3 *db) {
  sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
}

bool ChartDBHelper::CreateChartMetaTable(sqlite3 *db) {
  auto query = "CREATE TABLE IF NOT EXISTS chart_meta ("
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
               "total_backspin_notes INTEGER"
               ")";
  char *errMsg;
  int rc = sqlite3_exec(db, query, nullptr, nullptr, &errMsg);
  if (rc != SQLITE_OK) {
    std::cerr << "SQL error while creating chart meta table: " << errMsg
              << "\n";
    sqlite3_free(errMsg);
    return false;
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
               "total_backspin_notes"
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
               "@total_backspin_notes"
               ")";
  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(db, query, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    std::string err = std::string(sqlite3_errmsg(db));
    // UE_LOG(LogTemp, Error, TEXT("SQL error while preparing statement to
    // insert a chart: %s"), *err);
    sqlite3_close(db);
    return false;
  }
  std::filesystem::path path = chartMeta.BmsPath;
  ToRelativePath(path);

  sqlite3_bind_text(stmt, 1, path_t_to_utf8(fspath_to_path_t(path)).c_str(), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, (chartMeta.MD5).c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, (chartMeta.SHA256).c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, (chartMeta.Title).c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 5, (chartMeta.SubTitle).c_str(), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 6, (chartMeta.Genre).c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 7, (chartMeta.Artist).c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 8, (chartMeta.SubArtist).c_str(), -1,
                    SQLITE_TRANSIENT);

  std::filesystem::path folder = chartMeta.Folder;
  ToRelativePath(folder);
  sqlite3_bind_text(stmt, 9, path_t_to_utf8(fspath_to_path_t(folder)).c_str(),
                    -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(
      stmt, 10, path_t_to_utf8(fspath_to_path_t(chartMeta.StageFile)).c_str(),
      -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 11,
                    path_t_to_utf8(fspath_to_path_t(chartMeta.Banner)).c_str(),
                    -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 12,
                    path_t_to_utf8(fspath_to_path_t(chartMeta.BackBmp)).c_str(),
                    -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 13,
                    path_t_to_utf8(fspath_to_path_t(chartMeta.Preview)).c_str(),
                    -1, SQLITE_TRANSIENT);
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
  rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE) {
    SDL_Log("SQL error while inserting a chart: %s", sqlite3_errmsg(db));
    sqlite3_free(stmt);
    return false;
  }
  sqlite3_finalize(stmt);
  return true;
}

void ChartDBHelper::SelectAllChartMeta(
    sqlite3 *db, std::vector<bms_parser::ChartMeta> &chartMetas) {
  auto query = "SELECT "
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
               "total_backspin_notes"
               " FROM chart_meta ORDER BY title";
  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(db, query, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    std::cerr << "SQL error while getting all charts: " << sqlite3_errmsg(db)
              << "\n";
    sqlite3_free(stmt);
    return;
  }

  // reserve space for the result
  chartMetas.reserve(sqlite3_column_count(stmt));

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    chartMetas.push_back(std::move(ReadChartMeta(stmt)));
  }
  sqlite3_finalize(stmt);
}

void ChartDBHelper::SearchChartMeta(
    sqlite3 *db, const std::string &text,
    std::vector<bms_parser::ChartMeta> &chartMetas) {
  auto query =
      "SELECT "
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
      "total_backspin_notes"
      " FROM chart_meta WHERE rtrim(title || ' ' || subtitle || ' ' || artist "
      "|| ' ' || sub_artist || ' ' || genre) LIKE @text GROUP BY sha256 ORDER "
      "BY title";
  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(db, query, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    std::cerr << "SQL error while searching for charts: " << sqlite3_errmsg(db)
              << "\n";
    sqlite3_free(stmt);
    return;
  }
  // %text%
  sqlite3_bind_text(stmt, 1, ("%" + text + "%").c_str(), -1, SQLITE_TRANSIENT);

  // reserve space for the result
  chartMetas.reserve(sqlite3_column_count(stmt));

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    chartMetas.push_back(std::move(ReadChartMeta(stmt)));
  }
  sqlite3_finalize(stmt);
}

void ChartDBHelper::QueryChartMeta(
    sqlite3 *db, const ChartMetaQuery &chartQuery,
    std::vector<bms_parser::ChartMeta> &chartMetas) {
  std::string query = "SELECT ";
  query += kChartMetaSelectColumns;
  query += " FROM chart_meta cm WHERE 1 = 1";

  if (!chartQuery.keyword.empty()) {
    query += " AND rtrim(cm.title || ' ' || cm.subtitle || ' ' || cm.artist || "
             "' ' || cm.sub_artist || ' ' || cm.genre) LIKE @text";
  }

  if (chartQuery.tableId > 0) {
    query += " AND EXISTS (SELECT 1 FROM difficulty_table_entries dte "
             "WHERE dte.table_id = @table_id "
             "AND ((dte.sha256 != '' AND lower(cm.sha256) = dte.sha256) "
             "OR (dte.md5 != '' AND lower(cm.md5) = dte.md5))";
    if (!chartQuery.tableLevel.empty()) {
      query += " AND dte.level = @table_level";
    }
    query += ")";
  }

  if (chartQuery.coursesOnly || chartQuery.courseId > 0 ||
      !chartQuery.courseGroupName.empty()) {
    query += " AND EXISTS (SELECT 1 FROM difficulty_course_entries dce "
             "JOIN difficulty_courses dc ON dc.id = dce.course_id "
             "WHERE ((dce.sha256 != '' AND lower(cm.sha256) = dce.sha256) "
             "OR (dce.md5 != '' AND lower(cm.md5) = dce.md5))";
    if (chartQuery.courseId > 0) {
      query += " AND dce.course_id = @course_id";
    }
    if (chartQuery.courseTableId > 0) {
      query += " AND dc.table_id = @course_table_id";
    }
    if (!chartQuery.courseGroupName.empty()) {
      query += " AND dc.group_name = @course_group_name";
    }
    query += ")";
  }

  if (!chartQuery.difficultyText.empty()) {
    query += " AND EXISTS (SELECT 1 FROM difficulty_table_entries dte_filter "
             "JOIN difficulty_tables dt_filter ON dt_filter.id = "
             "dte_filter.table_id "
             "WHERE ((dte_filter.sha256 != '' AND lower(cm.sha256) = "
             "dte_filter.sha256) "
             "OR (dte_filter.md5 != '' AND lower(cm.md5) = dte_filter.md5)) "
             "AND (lower(dt_filter.symbol || dte_filter.level) = @difficulty "
             "OR lower(dte_filter.level) = @difficulty "
             "OR lower(dt_filter.name || ' ' || dt_filter.symbol || "
             "dte_filter.level) LIKE @difficulty_like "
             "OR lower(dt_filter.name || ' ' || dte_filter.level) LIKE "
             "@difficulty_like))";
  }

  query += " GROUP BY cm.sha256 ORDER BY cm.title";

  sqlite3_stmt *stmt = nullptr;
  int rc = sqlite3_prepare_v2(db, query.c_str(), -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    std::cerr << "SQL error while querying charts: " << sqlite3_errmsg(db)
              << "\n";
    sqlite3_free(stmt);
    return;
  }

  int bindIndex = 1;
  if (!chartQuery.keyword.empty()) {
    bindText(stmt, bindIndex++, "%" + chartQuery.keyword + "%");
  }
  if (chartQuery.tableId > 0) {
    sqlite3_bind_int(stmt, bindIndex++, chartQuery.tableId);
    if (!chartQuery.tableLevel.empty()) {
      bindText(stmt, bindIndex++, chartQuery.tableLevel);
    }
  }
  if (chartQuery.coursesOnly || chartQuery.courseId > 0 ||
      !chartQuery.courseGroupName.empty()) {
    if (chartQuery.courseId > 0) {
      sqlite3_bind_int(stmt, bindIndex++, chartQuery.courseId);
    }
    if (chartQuery.courseTableId > 0) {
      sqlite3_bind_int(stmt, bindIndex++, chartQuery.courseTableId);
    }
    if (!chartQuery.courseGroupName.empty()) {
      bindText(stmt, bindIndex++, chartQuery.courseGroupName);
    }
  }
  if (!chartQuery.difficultyText.empty()) {
    const std::string difficulty =
        lowerCopy(trimCopy(chartQuery.difficultyText));
    bindText(stmt, bindIndex++, difficulty);
    bindText(stmt, bindIndex++, "%" + difficulty + "%");
  }

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    chartMetas.push_back(std::move(ReadChartMeta(stmt)));
  }
  sqlite3_finalize(stmt);
}

bool ChartDBHelper::DeleteChartMeta(sqlite3 *db, std::filesystem::path path) {
  // std::cout << "Deleting chart: " << path.string() << std::endl;
  ToRelativePath(path);
  auto query = "DELETE FROM chart_meta WHERE path = @path";
  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(db, query, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    std::cout << "SQL error while preparing statement to delete a chart: "
              << sqlite3_errmsg(db) << "\n";
    sqlite3_free(stmt);
    return false;
  }
  const auto target = path_t_to_utf8(fspath_to_path_t(path));
  SDL_Log("Deleting chart: %s", target.c_str());
  sqlite3_bind_text(stmt, 1, target.c_str(), -1, SQLITE_TRANSIENT);
  rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE) {
    std::cout << "SQL error while deleting a chart: " << sqlite3_errmsg(db)
              << "\n";
    sqlite3_close(db);
    return false;
  }
  sqlite3_finalize(stmt);
  return true;
}

bool ChartDBHelper::ClearChartMeta(sqlite3 *db) {
  auto query = "DELETE FROM chart_meta";
  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(db, query, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    std::cerr << "SQL error while clearing: " << sqlite3_errmsg(db) << "\n";
    sqlite3_free(stmt);
    return false;
  }
  rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE) {

    std::cerr << "SQL error while clearing: " << sqlite3_errmsg(db) << "\n";
    sqlite3_free(stmt);
    return false;
  }
  sqlite3_finalize(stmt);
  return true;
}

bms_parser::ChartMeta ChartDBHelper::ReadChartMeta(sqlite3_stmt *stmt) {
  int idx = 0;
  bms_parser::ChartMeta chartMeta;
  auto t = ReadPath(stmt, idx++);

  std::filesystem::path path = std::filesystem::path(t);
  ToAbsolutePath(path);
  chartMeta.BmsPath = path;
  chartMeta.MD5 = std::string(
      reinterpret_cast<const char *>(sqlite3_column_text(stmt, idx++)));
  chartMeta.SHA256 = std::string(
      reinterpret_cast<const char *>(sqlite3_column_text(stmt, idx++)));
  chartMeta.Title = std::string(
      reinterpret_cast<const char *>(sqlite3_column_text(stmt, idx++)));
  chartMeta.SubTitle = std::string(
      reinterpret_cast<const char *>(sqlite3_column_text(stmt, idx++)));
  chartMeta.Genre = std::string(
      reinterpret_cast<const char *>(sqlite3_column_text(stmt, idx++)));
  chartMeta.Artist = std::string(
      reinterpret_cast<const char *>(sqlite3_column_text(stmt, idx++)));
  chartMeta.SubArtist = std::string(
      reinterpret_cast<const char *>(sqlite3_column_text(stmt, idx++)));
  auto folder_char = ReadPath(stmt, idx++);
  std::filesystem::path folder = std::filesystem::path(folder_char);
  ToAbsolutePath(folder);
  chartMeta.Folder = folder;
  auto stage_file_char = ReadPath(stmt, idx++);
  chartMeta.StageFile = std::filesystem::path(stage_file_char);
  auto banner_char = ReadPath(stmt, idx++);
  chartMeta.Banner = std::filesystem::path(banner_char);
  auto back_bmp_char = ReadPath(stmt, idx++);
  chartMeta.BackBmp = std::filesystem::path(back_bmp_char);
  auto preview_char = ReadPath(stmt, idx++);
  chartMeta.Preview = std::filesystem::path(preview_char);

  chartMeta.PlayLevel = sqlite3_column_double(stmt, idx++);
  chartMeta.Difficulty = sqlite3_column_int(stmt, idx++);
  chartMeta.Total = sqlite3_column_double(stmt, idx++);
  chartMeta.Bpm = sqlite3_column_double(stmt, idx++);
  chartMeta.MaxBpm = sqlite3_column_double(stmt, idx++);
  chartMeta.MinBpm = sqlite3_column_double(stmt, idx++);
  chartMeta.PlayLength = sqlite3_column_int64(stmt, idx++);
  chartMeta.Rank = sqlite3_column_int(stmt, idx++);
  chartMeta.Player = sqlite3_column_int(stmt, idx++);
  chartMeta.KeyMode = sqlite3_column_int(stmt, idx++);
  chartMeta.TotalNotes = sqlite3_column_int(stmt, idx++);
  chartMeta.TotalLongNotes = sqlite3_column_int(stmt, idx++);
  chartMeta.TotalScratchNotes = sqlite3_column_int(stmt, idx++);
  chartMeta.TotalBackSpinNotes = sqlite3_column_int(stmt, idx++);

  return chartMeta;
}

bool ChartDBHelper::CreateEntriesTable(sqlite3 *db) {
  // save paths to search for charts
  auto query = "CREATE TABLE IF NOT EXISTS entries ("
               "path       TEXT primary key"
               ")";

  char *errMsg;
  int rc = sqlite3_exec(db, query, nullptr, nullptr, &errMsg);
  if (rc != SQLITE_OK) {
    std::cerr << "SQL error while creating entries table: "
              << sqlite3_errmsg(db) << "\n";
    sqlite3_free(errMsg);
    return false;
  }
  return true;
}

bool ChartDBHelper::InsertEntry(sqlite3 *db,
                                const std::filesystem::path &path) {
  auto query = "REPLACE INTO entries ("
               "path"
               ") VALUES("
               "@path"
               ")";
  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(db, query, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    std::cerr << "SQL error while preparing statement to insert an entry: "
              << sqlite3_errmsg(db) << "\n";
    sqlite3_close(db);
    return false;
  }
  sqlite3_bind_text(stmt, 1, path.string().c_str(), -1, SQLITE_TRANSIENT);
  rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE) {
    std::cerr << "SQL error while inserting an entry: " << sqlite3_errmsg(db)
              << "\n";
    sqlite3_free(stmt);
    return false;
  }
  sqlite3_finalize(stmt);
  return true;
}

std::vector<path_t> ChartDBHelper::SelectAllEntries(sqlite3 *db) {
  auto query = "SELECT "
               "path"
               " FROM entries";
  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(db, query, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    std::cerr << "SQL error while getting all entries: " << sqlite3_errmsg(db)
              << "\n";
    sqlite3_free(stmt);
    return std::vector<path_t>();
  }
  std::vector<path_t> entries;
  entries.reserve(sqlite3_column_count(stmt));
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    std::filesystem::path entry = std::filesystem::path(
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0)));
    entries.push_back(fspath_to_path_t(entry));
  }
  sqlite3_finalize(stmt);
  return entries;
}

bool ChartDBHelper::DeleteEntry(sqlite3 *db,
                                const std::filesystem::path &path) {
  auto query = "DELETE FROM entries WHERE path = @path";
  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(db, query, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    std::cerr << "SQL error while preparing statement to delete an entry: "
              << sqlite3_errmsg(db) << "\n";
    sqlite3_free(stmt);
    return false;
  }
  sqlite3_bind_text(stmt, 1, path.string().c_str(), -1, SQLITE_TRANSIENT);
  rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE) {
    std::cerr << "SQL error while deleting an entry: " << sqlite3_errmsg(db)
              << "\n";
    sqlite3_close(db);
    return false;
  }
  sqlite3_finalize(stmt);
  return true;
}

bool ChartDBHelper::ClearEntries(sqlite3 *db) {
  auto query = "DELETE FROM entries";
  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(db, query, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {

    std::cerr << "SQL error while clearing: " << sqlite3_errmsg(db) << "\n";
    sqlite3_free(stmt);
    return false;
  }
  rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE) {
    std::cerr << "SQL error while clearing: " << sqlite3_errmsg(db) << "\n";
    sqlite3_free(stmt);
    return false;
  }
  sqlite3_finalize(stmt);
  return true;
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
      "sort_order INTEGER NOT NULL DEFAULT 0"
      ")",
      "CREATE INDEX IF NOT EXISTS idx_difficulty_entries_table_level "
      "ON difficulty_table_entries(table_id, level)",
      "CREATE INDEX IF NOT EXISTS idx_difficulty_entries_md5 "
      "ON difficulty_table_entries(md5)",
      "CREATE INDEX IF NOT EXISTS idx_difficulty_entries_sha256 "
      "ON difficulty_table_entries(sha256)",
      "CREATE INDEX IF NOT EXISTS idx_difficulty_courses_group "
      "ON difficulty_courses(table_id, group_name)",
      "CREATE INDEX IF NOT EXISTS idx_difficulty_course_entries_course "
      "ON difficulty_course_entries(course_id)",
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

  BeginTransaction(db);
  const int tableId =
      upsertDifficultyTable(db, name, symbol, dataUrl, sourceUrl);
  if (tableId <= 0) {
    CommitTransaction(db);
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

  int sortOrder = 0;
  if (charts != nullptr) {
    for (const auto &chartValue : *charts) {
      if (!chartValue.is_object()) {
        continue;
      }
      const auto chart = readChartItem(chartValue, "");
      if (chart.md5.empty() && chart.sha256.empty()) {
        continue;
      }
      insertDifficultyTableEntry(db, tableId, chart, sortOrder++);
    }
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

    const auto courseCharts = readCourseCharts(*course);
    int chartSortOrder = 0;
    for (const auto &chart : courseCharts) {
      if (chart.md5.empty() && chart.sha256.empty()) {
        continue;
      }
      insertDifficultyCourseEntry(db, courseId, chart, chartSortOrder++);
    }
  }

  CommitTransaction(db);
  SDL_Log("Imported difficulty table %s (%s) from %s", name.c_str(),
          symbol.c_str(), sourceUrl.c_str());
  return true;
}

bool ChartDBHelper::ImportDifficultyTableFromUrl(sqlite3 *db,
                                                 const std::string &pageUrl,
                                                 std::string *errorMessage) {
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

int ChartDBHelper::ImportDifficultyTablesFromDirectory(
    sqlite3 *db, const std::filesystem::path &directory) {
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  if (ec) {
    SDL_Log("Failed to create difficulty table directory %s: %s",
            directory.string().c_str(), ec.message().c_str());
    return 0;
  }

  int imported = 0;
  std::unordered_set<std::string> importedHeaders;
  for (std::filesystem::recursive_directory_iterator it(directory, ec), end;
       !ec && it != end; it.increment(ec)) {
    if (ec || !it->is_regular_file()) {
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
                                path.string())) {
        imported++;
      }
      continue;
    }

    if (!document.is_object() || !document.contains("name") ||
        !document.contains("symbol") || !document.contains("data_url")) {
      continue;
    }

    std::error_code canonicalEc;
    const auto canonical =
        std::filesystem::weakly_canonical(path, canonicalEc).string();
    if (importedHeaders.find(canonical) != importedHeaders.end()) {
      continue;
    }
    importedHeaders.insert(canonical);

    const std::string dataUrl = jsonStringAt(document, "data_url");
    std::filesystem::path dataPath = path.parent_path() / dataUrl;
    if (!std::filesystem::exists(dataPath)) {
      continue;
    }
    const auto dataRaw = readTextFile(dataPath);
    if (!dataRaw) {
      continue;
    }
    if (ImportDifficultyTable(db, *raw, *dataRaw, path.string())) {
      imported++;
    }
  }

  if (ec) {
    SDL_Log("Failed while scanning difficulty table directory %s: %s",
            directory.string().c_str(), ec.message().c_str());
  }
  return imported;
}

std::vector<DifficultyTableInfo>
ChartDBHelper::SelectDifficultyTables(sqlite3 *db) {
  auto query = "SELECT dt.id, dt.name, dt.symbol, dt.source_url, "
               "COUNT(DISTINCT cm.sha256) "
               "FROM difficulty_tables dt "
               "LEFT JOIN difficulty_table_entries dte ON dte.table_id = dt.id "
               "LEFT JOIN chart_meta cm ON "
               "((dte.sha256 != '' AND lower(cm.sha256) = dte.sha256) "
               "OR (dte.md5 != '' AND lower(cm.md5) = dte.md5)) "
               "GROUP BY dt.id "
               "ORDER BY dt.name COLLATE NOCASE";

  sqlite3_stmt *stmt = nullptr;
  int rc = sqlite3_prepare_v2(db, query, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    std::cerr << "SQL error while selecting difficulty tables: "
              << sqlite3_errmsg(db) << "\n";
    return {};
  }

  std::vector<DifficultyTableInfo> tables;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    DifficultyTableInfo table;
    table.id = columnInt(stmt, 0);
    table.name = columnString(stmt, 1);
    table.symbol = columnString(stmt, 2);
    table.sourceUrl = columnString(stmt, 3);
    table.matchedChartCount = columnInt(stmt, 4);
    tables.push_back(std::move(table));
  }
  sqlite3_finalize(stmt);
  return tables;
}

std::vector<DifficultyLevelInfo>
ChartDBHelper::SelectDifficultyLevels(sqlite3 *db, int tableId) {
  auto query = "SELECT dte.table_id, dt.name, dt.symbol, dte.level, "
               "COUNT(DISTINCT cm.sha256), MIN(dte.sort_order) "
               "FROM difficulty_table_entries dte "
               "JOIN difficulty_tables dt ON dt.id = dte.table_id "
               "LEFT JOIN chart_meta cm ON "
               "((dte.sha256 != '' AND lower(cm.sha256) = dte.sha256) "
               "OR (dte.md5 != '' AND lower(cm.md5) = dte.md5)) "
               "WHERE dte.table_id = @table_id "
               "GROUP BY dte.table_id, dte.level "
               "ORDER BY MIN(dte.sort_order), dte.level";
  sqlite3_stmt *stmt = nullptr;
  int rc = sqlite3_prepare_v2(db, query, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    std::cerr << "SQL error while selecting difficulty levels: "
              << sqlite3_errmsg(db) << "\n";
    return {};
  }
  sqlite3_bind_int(stmt, 1, tableId);

  std::vector<DifficultyLevelInfo> levels;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    DifficultyLevelInfo level;
    level.tableId = columnInt(stmt, 0);
    level.tableName = columnString(stmt, 1);
    level.tableSymbol = columnString(stmt, 2);
    level.level = columnString(stmt, 3);
    level.matchedChartCount = columnInt(stmt, 4);
    levels.push_back(std::move(level));
  }
  sqlite3_finalize(stmt);
  return levels;
}

std::vector<DifficultyCourseGroupInfo>
ChartDBHelper::SelectDifficultyCourseGroups(sqlite3 *db) {
  auto query =
      "SELECT dc.table_id, dt.name, dc.group_name, "
      "COUNT(DISTINCT cm.sha256), MIN(dc.sort_order) "
      "FROM difficulty_courses dc "
      "JOIN difficulty_tables dt ON dt.id = dc.table_id "
      "LEFT JOIN difficulty_course_entries dce ON dce.course_id = dc.id "
      "LEFT JOIN chart_meta cm ON "
      "((dce.sha256 != '' AND lower(cm.sha256) = dce.sha256) "
      "OR (dce.md5 != '' AND lower(cm.md5) = dce.md5)) "
      "GROUP BY dc.table_id, dc.group_name "
      "ORDER BY dt.name COLLATE NOCASE, MIN(dc.sort_order)";

  sqlite3_stmt *stmt = nullptr;
  int rc = sqlite3_prepare_v2(db, query, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    std::cerr << "SQL error while selecting difficulty course groups: "
              << sqlite3_errmsg(db) << "\n";
    return {};
  }

  std::vector<DifficultyCourseGroupInfo> groups;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    DifficultyCourseGroupInfo group;
    group.tableId = columnInt(stmt, 0);
    group.tableName = columnString(stmt, 1);
    group.groupName = columnString(stmt, 2);
    group.matchedChartCount = columnInt(stmt, 3);
    groups.push_back(std::move(group));
  }
  sqlite3_finalize(stmt);
  return groups;
}

std::vector<DifficultyCourseInfo>
ChartDBHelper::SelectDifficultyCourses(sqlite3 *db, int tableId,
                                       const std::string &groupName) {
  auto query =
      "SELECT dc.id, dc.table_id, dt.name, dc.group_name, dc.level, dc.name, "
      "COUNT(DISTINCT cm.sha256) "
      "FROM difficulty_courses dc "
      "JOIN difficulty_tables dt ON dt.id = dc.table_id "
      "LEFT JOIN difficulty_course_entries dce ON dce.course_id = dc.id "
      "LEFT JOIN chart_meta cm ON "
      "((dce.sha256 != '' AND lower(cm.sha256) = dce.sha256) "
      "OR (dce.md5 != '' AND lower(cm.md5) = dce.md5)) "
      "WHERE dc.table_id = @table_id AND dc.group_name = @group_name "
      "GROUP BY dc.id "
      "ORDER BY dc.sort_order";

  sqlite3_stmt *stmt = nullptr;
  int rc = sqlite3_prepare_v2(db, query, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    std::cerr << "SQL error while selecting difficulty courses: "
              << sqlite3_errmsg(db) << "\n";
    return {};
  }
  sqlite3_bind_int(stmt, 1, tableId);
  bindText(stmt, 2, groupName);

  std::vector<DifficultyCourseInfo> courses;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    DifficultyCourseInfo course;
    course.id = columnInt(stmt, 0);
    course.tableId = columnInt(stmt, 1);
    course.tableName = columnString(stmt, 2);
    course.groupName = columnString(stmt, 3);
    course.level = columnString(stmt, 4);
    course.name = columnString(stmt, 5);
    course.matchedChartCount = columnInt(stmt, 6);
    courses.push_back(std::move(course));
  }
  sqlite3_finalize(stmt);
  return courses;
}

void ChartDBHelper::ToRelativePath(
    [[maybe_unused]] std::filesystem::path &path) {
  // for iOS, remove Documents
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  static std::filesystem::path Documents = Utils::GetDocumentsPath("BMS/");
  if (path.string().find(Documents.string()) != std::string::npos) {
    path = path.string().substr(Documents.string().length());
  }

#endif
  // otherwise, noop
}

void ChartDBHelper::ToAbsolutePath(
    [[maybe_unused]] std::filesystem::path &path) {
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  static std::filesystem::path Documents = Utils::GetDocumentsPath("BMS/");
  path = Documents / path;
#endif
  // otherwise, noop
}
