#include "MainMenuLibrary.h"

#include "../SqliteRAII.h"
#include "../view/ClearLampColors.h"

#include <SDL2/SDL.h>
#include <algorithm>
#include <array>
#include <exception>
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

std::string chartSourceOrderBy(const std::string &alias) {
  return chartSourcePriorityExpr(alias) + ", " +
         chartSourceArchiveSizeExpr(alias) + ", " + alias + ".path";
}

void addClearMarkCount(FolderClearMarkCounts &counts,
                       const std::string &folderKey, int clearRank) {
  if (clearRank < kNoClearTypeRank) {
    return;
  }
  counts[folderKey][clearRank]++;
}

using FolderClearAggregateByLongNoteMode =
    std::array<FolderClearAggregate, 4>;

void addFolderChartForAllLongNoteModes(
    std::unordered_map<std::string, FolderClearAggregateByLongNoteMode>
        &aggregates,
    const ScoreClearRankCache &scoreRanks, const std::string &folderKey,
    std::string_view sha256, std::string_view md5, std::string_view path,
    int chartLongNoteMode, int totalLongNotes, int totalBackSpinNotes) {
  auto &aggregateByMode = aggregates[folderKey];
  for (int selectedLongNoteMode = 1; selectedLongNoteMode <= 3;
       ++selectedLongNoteMode) {
    auto &aggregate =
        aggregateByMode[static_cast<size_t>(selectedLongNoteMode)];
    if (aggregate.hasUnclearedChart) {
      continue;
    }
    const int longNoteMode =
        scoreLongNoteModeForClearLamp(chartLongNoteMode, totalLongNotes,
                                      totalBackSpinNotes,
                                      selectedLongNoteMode);
    aggregate.addChart(
        scoreRanks.bestRankForStoredKeys(sha256, md5, path, longNoteMode));
  }
}

void addClearMarkCountsForAllLongNoteModes(
    std::array<FolderClearMarkCounts, 4> &countsByMode,
    const ScoreClearRankCache &scoreRanks, const std::string &folderKey,
    std::string_view sha256, std::string_view md5, std::string_view path,
    int chartLongNoteMode, int totalLongNotes, int totalBackSpinNotes) {
  for (int selectedLongNoteMode = 1; selectedLongNoteMode <= 3;
       ++selectedLongNoteMode) {
    const int longNoteMode =
        scoreLongNoteModeForClearLamp(chartLongNoteMode, totalLongNotes,
                                      totalBackSpinNotes,
                                      selectedLongNoteMode);
    const int clearRank =
        scoreRanks.bestRankForStoredKeys(sha256, md5, path, longNoteMode);
    addClearMarkCount(
        countsByMode[static_cast<size_t>(selectedLongNoteMode)], folderKey,
        clearRank);
  }
}
} // namespace

FolderClearDataByLongNoteMode
LoadFolderClearDataByLongNoteMode(sqlite3 *db,
                                  const ScoreClearRankCache &scoreRanks) {
  FolderClearDataByLongNoteMode data;
  std::unordered_map<std::string, FolderClearAggregateByLongNoteMode>
      aggregates;

  auto runQuery = [&](const std::string &query, const auto &handleRow) {
    SqliteStatementHandle stmt;
    const int rc = prepareSqliteStatement(db, query, stmt);
    if (rc != SQLITE_OK) {
      SDL_Log("SQL error while loading folder clear data: %s",
              sqlite3_errmsg(db));
      return;
    }
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
      handleRow(stmt.get());
    }
  };

  runQuery("SELECT cm.sha256, cm.md5, cm.path, cm.ln_mode, "
           "cm.total_long_notes, cm.total_backspin_notes "
           "FROM chart_meta cm WHERE " +
               preferredChartPredicate("cm"),
           [&](sqlite3_stmt *row) {
             addFolderChartForAllLongNoteModes(
                 aggregates, scoreRanks, "all", columnText(row, 0),
                 columnText(row, 1), columnText(row, 2),
                 sqlite3_column_int(row, 3), sqlite3_column_int(row, 4),
                 sqlite3_column_int(row, 5));
             addClearMarkCountsForAllLongNoteModes(
                 data.clearMarkCounts, scoreRanks, "all", columnText(row, 0),
                 columnText(row, 1), columnText(row, 2),
                 sqlite3_column_int(row, 3), sqlite3_column_int(row, 4),
                 sqlite3_column_int(row, 5));
           });

  int currentTableId = 0;
  std::string currentTableKey;
  std::string currentLevel;
  std::string currentLevelKey;
  runQuery(
      "SELECT dte.table_id, dte.level, dte.sha256, dte.md5, '', "
      "COALESCE(cm.ln_mode, 0), COALESCE(cm.total_long_notes, 0), "
      "COALESCE(cm.total_backspin_notes, 0) "
      "FROM difficulty_table_entries dte "
      "LEFT JOIN chart_meta cm ON cm.path = ("
      "SELECT cm_match.path FROM chart_meta cm_match "
      "WHERE ((dte.sha256 != '' AND cm_match.sha256 = dte.sha256) "
      "OR (dte.md5 != '' AND cm_match.md5 = dte.md5)) "
      "ORDER BY " +
          chartSourceOrderBy("cm_match") +
          " LIMIT 1) "
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

        addFolderChartForAllLongNoteModes(
            aggregates, scoreRanks, currentTableKey, columnText(row, 2),
            columnText(row, 3), columnText(row, 4),
            sqlite3_column_int(row, 5), sqlite3_column_int(row, 6),
            sqlite3_column_int(row, 7));
        addFolderChartForAllLongNoteModes(
            aggregates, scoreRanks, currentLevelKey, columnText(row, 2),
            columnText(row, 3), columnText(row, 4),
            sqlite3_column_int(row, 5), sqlite3_column_int(row, 6),
            sqlite3_column_int(row, 7));
        addClearMarkCountsForAllLongNoteModes(
            data.clearMarkCounts, scoreRanks, currentTableKey,
            columnText(row, 2), columnText(row, 3), columnText(row, 4),
            sqlite3_column_int(row, 5), sqlite3_column_int(row, 6),
            sqlite3_column_int(row, 7));
        addClearMarkCountsForAllLongNoteModes(
            data.clearMarkCounts, scoreRanks, currentLevelKey,
            columnText(row, 2), columnText(row, 3), columnText(row, 4),
            sqlite3_column_int(row, 5), sqlite3_column_int(row, 6),
            sqlite3_column_int(row, 7));
      });

  int currentCourseId = 0;
  int currentCourseGroupTableId = 0;
  std::string currentCourseGroupName;
  std::string currentCourseGroupKey;
  std::string currentCourseKey;
  runQuery(
      "SELECT dc.id, dc.table_id, dc.group_name, dce.sha256, dce.md5, '', "
      "COALESCE(cm.ln_mode, 0), COALESCE(cm.total_long_notes, 0), "
      "COALESCE(cm.total_backspin_notes, 0) "
      "FROM difficulty_courses dc "
      "JOIN difficulty_course_entries dce ON dce.course_id = dc.id "
      "LEFT JOIN chart_meta cm ON cm.path = ("
      "SELECT cm_match.path FROM chart_meta cm_match "
      "WHERE ((dce.sha256 != '' AND cm_match.sha256 = dce.sha256) "
      "OR (dce.md5 != '' AND cm_match.md5 = dce.md5)) "
      "ORDER BY " +
          chartSourceOrderBy("cm_match") +
          " LIMIT 1) "
      "ORDER BY dc.table_id, dc.group_name, dc.id, dce.sort_order",
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

        addFolderChartForAllLongNoteModes(
            aggregates, scoreRanks, "courses", columnText(row, 3),
            columnText(row, 4), columnText(row, 5),
            sqlite3_column_int(row, 6), sqlite3_column_int(row, 7),
            sqlite3_column_int(row, 8));
        addFolderChartForAllLongNoteModes(
            aggregates, scoreRanks, currentCourseGroupKey, columnText(row, 3),
            columnText(row, 4), columnText(row, 5),
            sqlite3_column_int(row, 6), sqlite3_column_int(row, 7),
            sqlite3_column_int(row, 8));
        addFolderChartForAllLongNoteModes(
            aggregates, scoreRanks, currentCourseKey, columnText(row, 3),
            columnText(row, 4), columnText(row, 5),
            sqlite3_column_int(row, 6), sqlite3_column_int(row, 7),
            sqlite3_column_int(row, 8));
      });

  for (const auto &[key, aggregateByMode] : aggregates) {
    for (int selectedLongNoteMode = 1; selectedLongNoteMode <= 3;
         ++selectedLongNoteMode) {
      const int clearRank =
          aggregateByMode[static_cast<size_t>(selectedLongNoteMode)]
              .clearRank();
      if (clearRank >= kClearTypeAssistedEasyClearRank) {
        data.clearRanks[static_cast<size_t>(selectedLongNoteMode)][key] =
            clearRank;
      }
    }
  }
  for (const auto &[courseIdText, clearRank] : scoreRanks.rankByCourseId) {
    if (clearRank < kClearTypeAssistedEasyClearRank) {
      continue;
    }
    try {
      const std::string folderKey = folderKeyForCourse(std::stoi(courseIdText));
      for (int selectedLongNoteMode = 1; selectedLongNoteMode <= 3;
           ++selectedLongNoteMode) {
        data.clearRanks[static_cast<size_t>(selectedLongNoteMode)]
                       [folderKey] = clearRank;
      }
    } catch (const std::exception &) {
    }
  }

  return data;
}

} // namespace main_menu_library
