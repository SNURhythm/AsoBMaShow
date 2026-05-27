#pragma once

#include "../ScoreDBHelper.h"
#include "../sqlite3.h"

#include <string>
#include <unordered_map>

namespace main_menu_library {

std::string folderKeyForTable(int tableId);
std::string folderKeyForLevel(int tableId, const std::string &level);
std::string folderKeyForCourseGroup(int tableId, const std::string &groupName);
std::string folderKeyForCourse(int courseId);

std::unordered_map<std::string, int>
LoadFolderClearRanks(sqlite3 *db, const ScoreClearRankCache &scoreRanks);

} // namespace main_menu_library
