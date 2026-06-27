#include "MainMenuLibrary.h"

#include "../SqliteRAII.h"
#include "../view/ClearLampColors.h"

#include <SDL2/SDL.h>
#include <algorithm>
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

void addFolderChart(
    std::unordered_map<std::string, FolderClearAggregate> &aggregates,
    const ScoreClearRankCache &scoreRanks, const std::string &folderKey,
    std::string_view sha256, std::string_view md5, std::string_view path,
    int longNoteMode) {
  auto &aggregate = aggregates[folderKey];
  if (aggregate.hasUnclearedChart) {
    return;
  }
  aggregate.addChart(
      scoreRanks.bestRankForStoredKeys(sha256, md5, path, longNoteMode));
}

void addClearMarkCount(FolderClearMarkCounts &counts,
                       const std::string &folderKey, int clearRank) {
  if (clearRank < kNoClearTypeRank) {
    return;
  }
  counts[folderKey][clearRank]++;
}
} // namespace

std::unordered_map<std::string, int>
LoadFolderClearRanks(sqlite3 *db, const ScoreClearRankCache &scoreRanks,
                     int selectedLongNoteMode) {
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

  runQuery("SELECT cm.sha256, cm.md5, cm.path, cm.ln_mode, "
           "cm.total_long_notes, cm.total_backspin_notes "
           "FROM chart_meta cm WHERE " +
               preferredChartPredicate("cm"),
           [&](sqlite3_stmt *row) {
             const int longNoteMode = scoreLongNoteModeForClearLamp(
                 sqlite3_column_int(row, 3), sqlite3_column_int(row, 4),
                 sqlite3_column_int(row, 5), selectedLongNoteMode);
             addFolderChart(aggregates, scoreRanks, "all", columnText(row, 0),
                            columnText(row, 1), columnText(row, 2),
                            longNoteMode);
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

        const std::string_view sha256 = columnText(row, 2);
        const std::string_view md5 = columnText(row, 3);
        const std::string_view path = columnText(row, 4);
        const int longNoteMode = scoreLongNoteModeForClearLamp(
            sqlite3_column_int(row, 5), sqlite3_column_int(row, 6),
            sqlite3_column_int(row, 7), selectedLongNoteMode);
        addFolderChart(aggregates, scoreRanks, currentTableKey, sha256, md5,
                       path, longNoteMode);
        addFolderChart(aggregates, scoreRanks, currentLevelKey, sha256, md5,
                       path, longNoteMode);
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

        const std::string_view sha256 = columnText(row, 3);
        const std::string_view md5 = columnText(row, 4);
        const std::string_view path = columnText(row, 5);
        const int longNoteMode = scoreLongNoteModeForClearLamp(
            sqlite3_column_int(row, 6), sqlite3_column_int(row, 7),
            sqlite3_column_int(row, 8), selectedLongNoteMode);
        addFolderChart(aggregates, scoreRanks, "courses", sha256, md5, path,
                       longNoteMode);
        addFolderChart(aggregates, scoreRanks, currentCourseGroupKey, sha256,
                       md5, path, longNoteMode);
        addFolderChart(aggregates, scoreRanks, currentCourseKey, sha256, md5,
                       path, longNoteMode);
      });

  std::unordered_map<std::string, int> folderClearRanks;
  for (const auto &[key, aggregate] : aggregates) {
    const int clearRank = aggregate.clearRank();
    if (clearRank >= kClearTypeAssistedEasyClearRank) {
      folderClearRanks[key] = clearRank;
    }
  }
  for (const auto &[courseIdText, clearRank] : scoreRanks.rankByCourseId) {
    if (clearRank < kClearTypeAssistedEasyClearRank) {
      continue;
    }
    try {
      folderClearRanks[folderKeyForCourse(std::stoi(courseIdText))] =
          clearRank;
    } catch (const std::exception &) {
    }
  }
  return folderClearRanks;
}

FolderClearMarkCounts
LoadFolderClearMarkCounts(sqlite3 *db, const ScoreClearRankCache &scoreRanks,
                          int selectedLongNoteMode) {
  FolderClearMarkCounts counts;

  {
    const std::string query =
        "SELECT cm.sha256, cm.md5, cm.path, cm.ln_mode, "
        "cm.total_long_notes, cm.total_backspin_notes "
        "FROM chart_meta cm WHERE " +
        preferredChartPredicate("cm");
    SqliteStatementHandle stmt;
    const int rc = prepareSqliteStatement(db, query, stmt);
    if (rc != SQLITE_OK) {
      SDL_Log("SQL error while loading all-song clear mark counts: %s",
              sqlite3_errmsg(db));
    } else {
      while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        const int longNoteMode = scoreLongNoteModeForClearLamp(
            sqlite3_column_int(stmt.get(), 3), sqlite3_column_int(stmt.get(), 4),
            sqlite3_column_int(stmt.get(), 5), selectedLongNoteMode);
        const int clearRank = scoreRanks.bestRankForStoredKeys(
            columnText(stmt.get(), 0), columnText(stmt.get(), 1),
            columnText(stmt.get(), 2), longNoteMode);
        addClearMarkCount(counts, "all", clearRank);
      }
    }
  }

  const std::string query =
      "SELECT dte.table_id, dte.level, dte.sha256, dte.md5, "
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
      "ORDER BY dte.table_id, dte.level";
  SqliteStatementHandle stmt;
  const int rc = prepareSqliteStatement(db, query, stmt);
  if (rc != SQLITE_OK) {
    SDL_Log("SQL error while loading folder clear mark counts: %s",
            sqlite3_errmsg(db));
    return counts;
  }

  int currentTableId = 0;
  std::string currentTableKey;
  std::string currentLevel;
  std::string currentLevelKey;
  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    const int tableId = sqlite3_column_int(stmt.get(), 0);
    const std::string_view level = columnText(stmt.get(), 1);
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

    const std::string sha256(columnText(stmt.get(), 2));
    const std::string md5(columnText(stmt.get(), 3));
    const int longNoteMode = scoreLongNoteModeForClearLamp(
        sqlite3_column_int(stmt.get(), 4), sqlite3_column_int(stmt.get(), 5),
        sqlite3_column_int(stmt.get(), 6), selectedLongNoteMode);
    const int clearRank =
        scoreRanks.bestRankForHashes(sha256, md5, "", longNoteMode);
    addClearMarkCount(counts, currentTableKey, clearRank);
    addClearMarkCount(counts, currentLevelKey, clearRank);
  }

  return counts;
}

} // namespace main_menu_library
