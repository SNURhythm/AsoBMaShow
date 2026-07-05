#include "MainMenuLibrary.h"

#include "../ChartSqlExpressions.h"
#include "../LongNoteModeUtils.h"
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

std::string folderKeyForCourseTable(int tableId) {
  return "course-table:" + std::to_string(tableId);
}

std::string folderKeyForCourseGroup(int tableId,
                                    const std::string &groupName) {
  return "course-group:" + std::to_string(tableId) + ":" + groupName;
}

std::string folderKeyForCourse(int courseId) {
  return "course:" + std::to_string(courseId);
}

namespace {
using asobmshow::chart_sql::matchedChartPathSubquery;
using asobmshow::chart_sql::preferredChartPredicate;

std::string_view columnText(sqlite3_stmt *stmt, int column) {
  return sqliteColumnTextView(stmt, column);
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
    std::string_view sha256, int chartLongNoteMode, int totalLongNotes,
    int totalBackSpinNotes) {
  auto &aggregateByMode = aggregates[folderKey];
  for (int selectedLongNoteMode : long_note_mode::kPlayableValues) {
    auto &aggregate =
        aggregateByMode[static_cast<size_t>(selectedLongNoteMode)];
    if (aggregate.hasUnclearedChart) {
      continue;
    }
    const int longNoteMode =
        scoreLongNoteModeForClearLamp(chartLongNoteMode, totalLongNotes,
                                      totalBackSpinNotes,
                                      selectedLongNoteMode);
    aggregate.addChart(scoreRanks.bestRankForStoredKey(sha256, longNoteMode));
  }
}

void addClearMarkCountsForAllLongNoteModes(
    std::array<FolderClearMarkCounts, 4> &countsByMode,
    const ScoreClearRankCache &scoreRanks, const std::string &folderKey,
    std::string_view sha256, int chartLongNoteMode, int totalLongNotes,
    int totalBackSpinNotes) {
  for (int selectedLongNoteMode : long_note_mode::kPlayableValues) {
    const int longNoteMode =
        scoreLongNoteModeForClearLamp(chartLongNoteMode, totalLongNotes,
                                      totalBackSpinNotes,
                                      selectedLongNoteMode);
    const int clearRank = scoreRanks.bestRankForStoredKey(sha256, longNoteMode);
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

  runQuery("SELECT cm.sha256, cm.ln_mode, "
           "cm.total_long_notes, cm.total_backspin_notes "
           "FROM chart_meta cm WHERE " +
               preferredChartPredicate("cm"),
           [&](sqlite3_stmt *row) {
             addFolderChartForAllLongNoteModes(
                 aggregates, scoreRanks, "all", columnText(row, 0),
                 sqlite3_column_int(row, 1), sqlite3_column_int(row, 2),
                 sqlite3_column_int(row, 3));
             addClearMarkCountsForAllLongNoteModes(
                 data.clearMarkCounts, scoreRanks, "all", columnText(row, 0),
                 sqlite3_column_int(row, 1), sqlite3_column_int(row, 2),
                 sqlite3_column_int(row, 3));
           });

  int currentTableId = 0;
  std::string currentTableKey;
  std::string currentLevel;
  std::string currentLevelKey;
  runQuery(
      "SELECT dte.table_id, dte.level, dte.sha256, "
      "COALESCE(cm.ln_mode, 0), COALESCE(cm.total_long_notes, 0), "
      "COALESCE(cm.total_backspin_notes, 0) "
      "FROM difficulty_table_entries dte "
      "LEFT JOIN chart_meta cm ON cm.path = " +
          matchedChartPathSubquery("dte") + " "
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
            sqlite3_column_int(row, 3), sqlite3_column_int(row, 4),
            sqlite3_column_int(row, 5));
        addFolderChartForAllLongNoteModes(
            aggregates, scoreRanks, currentLevelKey, columnText(row, 2),
            sqlite3_column_int(row, 3), sqlite3_column_int(row, 4),
            sqlite3_column_int(row, 5));
        addClearMarkCountsForAllLongNoteModes(
            data.clearMarkCounts, scoreRanks, currentTableKey,
            columnText(row, 2), sqlite3_column_int(row, 3),
            sqlite3_column_int(row, 4), sqlite3_column_int(row, 5));
        addClearMarkCountsForAllLongNoteModes(
            data.clearMarkCounts, scoreRanks, currentLevelKey,
            columnText(row, 2), sqlite3_column_int(row, 3),
            sqlite3_column_int(row, 4), sqlite3_column_int(row, 5));
      });

  int currentCourseId = 0;
  int currentCourseTableId = 0;
  std::string currentCourseTableKey;
  int currentCourseGroupTableId = 0;
  std::string currentCourseGroupName;
  std::string currentCourseGroupKey;
  std::string currentCourseKey;
  runQuery(
      "SELECT dc.id, dc.table_id, dc.group_name, dce.sha256, "
      "COALESCE(cm.ln_mode, 0), COALESCE(cm.total_long_notes, 0), "
      "COALESCE(cm.total_backspin_notes, 0) "
      "FROM difficulty_courses dc "
      "JOIN difficulty_course_entries dce ON dce.course_id = dc.id "
      "LEFT JOIN chart_meta cm ON cm.path = " +
          matchedChartPathSubquery("dce") + " "
      "ORDER BY dc.table_id, dc.group_name, dc.id, dce.sort_order",
      [&](sqlite3_stmt *row) {
        const int courseId = sqlite3_column_int(row, 0);
        const int tableId = sqlite3_column_int(row, 1);
        const std::string_view groupName = columnText(row, 2);
        if (tableId != currentCourseTableId) {
          currentCourseTableId = tableId;
          currentCourseTableKey = folderKeyForCourseTable(tableId);
        }
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
            sqlite3_column_int(row, 4), sqlite3_column_int(row, 5),
            sqlite3_column_int(row, 6));
        addFolderChartForAllLongNoteModes(
            aggregates, scoreRanks, currentCourseTableKey, columnText(row, 3),
            sqlite3_column_int(row, 4), sqlite3_column_int(row, 5),
            sqlite3_column_int(row, 6));
        addFolderChartForAllLongNoteModes(
            aggregates, scoreRanks, currentCourseGroupKey, columnText(row, 3),
            sqlite3_column_int(row, 4), sqlite3_column_int(row, 5),
            sqlite3_column_int(row, 6));
        addFolderChartForAllLongNoteModes(
            aggregates, scoreRanks, currentCourseKey, columnText(row, 3),
            sqlite3_column_int(row, 4), sqlite3_column_int(row, 5),
            sqlite3_column_int(row, 6));
      });

  for (const auto &[key, aggregateByMode] : aggregates) {
    for (int selectedLongNoteMode : long_note_mode::kPlayableValues) {
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
      for (int selectedLongNoteMode : long_note_mode::kPlayableValues) {
        data.clearRanks[static_cast<size_t>(selectedLongNoteMode)]
                       [folderKey] = clearRank;
      }
    } catch (const std::exception &) {
    }
  }

  return data;
}

} // namespace main_menu_library
