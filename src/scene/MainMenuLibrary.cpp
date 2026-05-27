#include "MainMenuLibrary.h"

#include "../view/ClearLampColors.h"

#include <SDL2/SDL.h>
#include <algorithm>
#include <cctype>
#include <limits>
#include <unordered_set>

namespace main_menu_library {

std::string folderKeyForTable(int tableId) {
  return "table:" + std::to_string(tableId);
}

std::string folderKeyForLevel(int tableId, const std::string &level) {
  return "level:" + std::to_string(tableId) + ":" + level;
}

std::string folderKeyForCourseGroup(int tableId,
                                    const std::string &groupName) {
  return "course-group:" + std::to_string(tableId) + ":" + groupName;
}

std::string folderKeyForCourse(int courseId) {
  return "course:" + std::to_string(courseId);
}

namespace {
std::string columnText(sqlite3_stmt *stmt, int column) {
  const auto *text =
      reinterpret_cast<const char *>(sqlite3_column_text(stmt, column));
  return text != nullptr ? std::string(text) : "";
}

std::string normalizedHashKey(std::string value) {
  value.erase(value.begin(), std::find_if(value.begin(), value.end(),
                                          [](unsigned char c) {
                                            return std::isspace(c) == 0;
                                          }));
  value.erase(std::find_if(value.rbegin(), value.rend(),
                           [](unsigned char c) {
                             return std::isspace(c) == 0;
                           }).base(),
              value.end());
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::tolower(c));
                 });
  return value;
}

std::string chartIdentity(const std::string &sha256, const std::string &md5,
                          const std::string &path,
                          const std::string &fallback = "") {
  const std::string normalizedSha = normalizedHashKey(sha256);
  if (!normalizedSha.empty()) {
    return "sha256:" + normalizedSha;
  }
  const std::string normalizedMd5 = normalizedHashKey(md5);
  if (!normalizedMd5.empty()) {
    return "md5:" + normalizedMd5;
  }
  if (!path.empty()) {
    return "path:" + path;
  }
  return fallback;
}

struct FolderClearAggregate {
  std::unordered_set<std::string> chartIds;
  int minimumClearRank = std::numeric_limits<int>::max();
  bool hasUnclearedChart = false;

  void addChart(const std::string &identity, int clearRank) {
    if (identity.empty() || !chartIds.insert(identity).second) {
      return;
    }
    if (clearRank < kClearTypeAssistedEasyClearRank) {
      hasUnclearedChart = true;
      return;
    }
    minimumClearRank = std::min(minimumClearRank, clearRank);
  }

  [[nodiscard]] int clearRank() const {
    if (chartIds.empty() || hasUnclearedChart ||
        minimumClearRank == std::numeric_limits<int>::max()) {
      return kNoClearTypeRank;
    }
    return minimumClearRank;
  }
};

void addFolderChart(
    std::unordered_map<std::string, FolderClearAggregate> &aggregates,
    const ScoreClearRankCache &scoreRanks, const std::string &folderKey,
    const std::string &sha256, const std::string &md5,
    const std::string &path, const std::string &fallbackIdentity = "") {
  aggregates[folderKey].addChart(
      chartIdentity(sha256, md5, path, fallbackIdentity),
      scoreRanks.bestRankForHashes(sha256, md5, path));
}
} // namespace

std::unordered_map<std::string, int>
LoadFolderClearRanks(sqlite3 *db, const ScoreClearRankCache &scoreRanks) {
  std::unordered_map<std::string, FolderClearAggregate> aggregates;
  sqlite3_stmt *stmt = nullptr;

  auto runQuery = [&](const std::string &query, const auto &handleRow) {
    stmt = nullptr;
    const int rc = sqlite3_prepare_v2(db, query.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
      SDL_Log("SQL error while loading folder clear ranks: %s",
              sqlite3_errmsg(db));
      return;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      handleRow(stmt);
    }
    sqlite3_finalize(stmt);
    stmt = nullptr;
  };

  runQuery("SELECT sha256, md5, path FROM chart_meta", [&](sqlite3_stmt *row) {
    addFolderChart(aggregates, scoreRanks, "all", columnText(row, 0),
                   columnText(row, 1), columnText(row, 2));
  });

  runQuery(
      "SELECT dte.table_id, dte.level, dte.sha256, dte.md5, '', dte.id "
      "FROM difficulty_table_entries dte",
      [&](sqlite3_stmt *row) {
        const int tableId = sqlite3_column_int(row, 0);
        const std::string level = columnText(row, 1);
        const std::string sha256 = columnText(row, 2);
        const std::string md5 = columnText(row, 3);
        const std::string path = columnText(row, 4);
        const std::string fallbackIdentity =
            "difficulty-entry:" + std::to_string(sqlite3_column_int(row, 5));
        addFolderChart(aggregates, scoreRanks, folderKeyForTable(tableId),
                       sha256, md5, path, fallbackIdentity);
        addFolderChart(aggregates, scoreRanks,
                       folderKeyForLevel(tableId, level), sha256, md5, path,
                       fallbackIdentity);
      });

  runQuery(
      "SELECT dc.id, dc.table_id, dc.group_name, cm.sha256, cm.md5, cm.path "
      "FROM difficulty_courses dc "
      "JOIN difficulty_course_entries dce ON dce.course_id = dc.id "
      "JOIN chart_meta cm ON "
      "((dce.sha256 != '' AND cm.sha256 = dce.sha256) "
      "OR (dce.md5 != '' AND cm.md5 = dce.md5))",
      [&](sqlite3_stmt *row) {
        const int courseId = sqlite3_column_int(row, 0);
        const int tableId = sqlite3_column_int(row, 1);
        const std::string groupName = columnText(row, 2);
        const std::string sha256 = columnText(row, 3);
        const std::string md5 = columnText(row, 4);
        const std::string path = columnText(row, 5);
        addFolderChart(aggregates, scoreRanks, "courses", sha256, md5, path);
        addFolderChart(aggregates, scoreRanks,
                       folderKeyForCourseGroup(tableId, groupName), sha256,
                       md5, path);
        addFolderChart(aggregates, scoreRanks, folderKeyForCourse(courseId),
                       sha256, md5, path);
      });

  std::unordered_map<std::string, int> folderClearRanks;
  for (const auto &[key, aggregate] : aggregates) {
    const int clearRank = aggregate.clearRank();
    if (clearRank >= kClearTypeAssistedEasyClearRank) {
      folderClearRanks[key] = clearRank;
    }
  }
  return folderClearRanks;
}

} // namespace main_menu_library
