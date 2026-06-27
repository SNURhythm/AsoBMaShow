#pragma once

#include "../ScoreDBHelper.h"
#include "../sqlite3.h"

#include <array>
#include <string>
#include <unordered_map>

namespace main_menu_library {

using ClearMarkCountMap = std::unordered_map<int, int>;
using FolderClearMarkCounts = std::unordered_map<std::string, ClearMarkCountMap>;
using FolderClearRankMap = std::unordered_map<std::string, int>;

struct FolderClearDataByLongNoteMode {
  std::array<FolderClearRankMap, 4> clearRanks;
  std::array<FolderClearMarkCounts, 4> clearMarkCounts;
};

std::string folderKeyForTable(int tableId);
std::string folderKeyForLevel(int tableId, const std::string &level);
std::string folderKeyForCourseGroup(int tableId, const std::string &groupName);
std::string folderKeyForCourse(int courseId);

FolderClearDataByLongNoteMode
LoadFolderClearDataByLongNoteMode(sqlite3 *db,
                                  const ScoreClearRankCache &scoreRanks);

} // namespace main_menu_library
