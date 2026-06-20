#include "MainMenuLibrary.h"

#include "../SqliteRAII.h"
#include "../view/ClearLampColors.h"

#include <SDL2/SDL.h>
#include <algorithm>
#include <limits>
#include <string_view>

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
std::string_view columnText(sqlite3_stmt *stmt, int column) {
  const auto *text =
      reinterpret_cast<const char *>(sqlite3_column_text(stmt, column));
  if (text == nullptr) {
    return {};
  }
  return {text, static_cast<size_t>(sqlite3_column_bytes(stmt, column))};
}

struct FolderClearAggregate {
  bool hasChart = false;
  int minimumClearRank = std::numeric_limits<int>::max();
  bool hasUnclearedChart = false;

  void addChart(int clearRank) {
    hasChart = true;
    if (clearRank < kClearTypeAssistedEasyClearRank) {
      hasUnclearedChart = true;
      return;
    }
    minimumClearRank = std::min(minimumClearRank, clearRank);
  }

  [[nodiscard]] int clearRank() const {
    if (!hasChart || hasUnclearedChart ||
        minimumClearRank == std::numeric_limits<int>::max()) {
      return kNoClearTypeRank;
    }
    return minimumClearRank;
  }
};

constexpr const char *kMaxSqlIntegerText = "9223372036854775807";

std::string chartSourcePriorityExpr(const std::string &alias) {
  return "COALESCE(" + alias + ".source_priority, 3)";
}

std::string chartSourceArchiveSizeExpr(const std::string &alias) {
  return "COALESCE(" + alias + ".source_archive_size, " +
         kMaxSqlIntegerText + ")";
}

std::string preferredChartPredicate(const std::string &alias) {
  const std::string betterPriority = chartSourcePriorityExpr("cm_better");
  const std::string currentPriority = chartSourcePriorityExpr(alias);
  const std::string betterArchiveSize =
      chartSourceArchiveSizeExpr("cm_better");
  const std::string currentArchiveSize = chartSourceArchiveSizeExpr(alias);

  return "NOT EXISTS (SELECT 1 FROM chart_meta cm_better WHERE "
         "cm_better.path != " +
         alias + ".path AND ((" + alias +
         ".sha256 != '' AND cm_better.sha256 = " + alias +
         ".sha256) OR (" + alias +
         ".sha256 = '' AND " + alias +
         ".md5 != '' AND cm_better.md5 = " + alias + ".md5)) AND (" +
         betterPriority + " < " + currentPriority + " OR (" +
         betterPriority + " = " + currentPriority + " AND " +
         betterArchiveSize + " < " + currentArchiveSize + ") OR (" +
         betterPriority + " = " + currentPriority + " AND " +
         betterArchiveSize + " = " + currentArchiveSize +
         " AND cm_better.path < " + alias + ".path)))";
}

void addFolderChart(
    std::unordered_map<std::string, FolderClearAggregate> &aggregates,
    const ScoreClearRankCache &scoreRanks, const std::string &folderKey,
    std::string_view sha256, std::string_view md5, std::string_view path) {
  auto &aggregate = aggregates[folderKey];
  if (aggregate.hasUnclearedChart) {
    return;
  }
  aggregate.addChart(scoreRanks.bestRankForStoredKeys(sha256, md5, path));
}
} // namespace

std::unordered_map<std::string, int>
LoadFolderClearRanks(sqlite3 *db, const ScoreClearRankCache &scoreRanks) {
  std::unordered_map<std::string, FolderClearAggregate> aggregates;

  auto runQuery = [&](const std::string &query, const auto &handleRow) {
    SqliteStatementHandle stmt;
    const int rc = prepareSqliteStatement(db, query, stmt);
    if (rc != SQLITE_OK) {
      SDL_Log("SQL error while loading folder clear ranks: %s",
              sqlite3_errmsg(db));
      return;
    }
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
      handleRow(stmt.get());
    }
  };

  runQuery("SELECT cm.sha256, cm.md5, cm.path FROM chart_meta cm WHERE " +
               preferredChartPredicate("cm"),
           [&](sqlite3_stmt *row) {
             addFolderChart(aggregates, scoreRanks, "all", columnText(row, 0),
                            columnText(row, 1), columnText(row, 2));
           });

  int currentTableId = 0;
  std::string currentTableKey;
  std::string currentLevel;
  std::string currentLevelKey;
  runQuery(
      "SELECT dte.table_id, dte.level, dte.sha256, dte.md5, '' "
      "FROM difficulty_table_entries dte "
      "ORDER BY dte.table_id, dte.level",
      [&](sqlite3_stmt *row) {
        const int tableId = sqlite3_column_int(row, 0);
        const std::string_view level = columnText(row, 1);
        if (tableId != currentTableId) {
          currentTableId = tableId;
          currentTableKey = folderKeyForTable(tableId);
          currentLevel.clear();
          currentLevelKey.clear();
        }
        if (level != std::string_view(currentLevel)) {
          currentLevel = std::string(level);
          currentLevelKey = folderKeyForLevel(tableId, currentLevel);
        }

        const std::string_view sha256 = columnText(row, 2);
        const std::string_view md5 = columnText(row, 3);
        const std::string_view path = columnText(row, 4);
        addFolderChart(aggregates, scoreRanks, currentTableKey, sha256, md5,
                       path);
        addFolderChart(aggregates, scoreRanks, currentLevelKey, sha256, md5,
                       path);
      });

  int currentCourseId = 0;
  int currentCourseGroupTableId = 0;
  std::string currentCourseGroupName;
  std::string currentCourseGroupKey;
  std::string currentCourseKey;
  runQuery(
      "SELECT dc.id, dc.table_id, dc.group_name, cm.sha256, cm.md5, cm.path "
      "FROM difficulty_courses dc "
      "JOIN difficulty_course_entries dce ON dce.course_id = dc.id "
      "JOIN chart_meta cm ON "
      "((dce.sha256 != '' AND cm.sha256 = dce.sha256) "
      "OR (dce.md5 != '' AND cm.md5 = dce.md5)) "
      "WHERE " +
          preferredChartPredicate("cm"),
      [&](sqlite3_stmt *row) {
        const int courseId = sqlite3_column_int(row, 0);
        const int tableId = sqlite3_column_int(row, 1);
        const std::string_view groupName = columnText(row, 2);
        if (tableId != currentCourseGroupTableId ||
            groupName != std::string_view(currentCourseGroupName)) {
          currentCourseGroupTableId = tableId;
          currentCourseGroupName = std::string(groupName);
          currentCourseGroupKey =
              folderKeyForCourseGroup(tableId, currentCourseGroupName);
        }
        if (courseId != currentCourseId) {
          currentCourseId = courseId;
          currentCourseKey = folderKeyForCourse(courseId);
        }

        const std::string_view sha256 = columnText(row, 3);
        const std::string_view md5 = columnText(row, 4);
        const std::string_view path = columnText(row, 5);
        addFolderChart(aggregates, scoreRanks, "courses", sha256, md5, path);
        addFolderChart(aggregates, scoreRanks, currentCourseGroupKey, sha256,
                       md5, path);
        addFolderChart(aggregates, scoreRanks, currentCourseKey, sha256, md5,
                       path);
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
