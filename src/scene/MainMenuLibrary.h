#pragma once

#include "../LibraryFolderClearData.h"

#include <string>

namespace main_menu_library {

std::string folderKeyForTable(int tableId);
std::string folderKeyForLevel(int tableId, const std::string &level);
std::string folderKeyForCourseTable(int tableId);
std::string folderKeyForCourseGroup(int tableId, const std::string &groupName);
std::string folderKeyForCourse(int courseId);

inline bool difficultyRangeEnabledForFolder(bool difficultyTableFolder,
                                            bool clearMarkFolder, int tableId,
                                            const std::string &tableLevel) {
  if (tableId <= 0) {
    return false;
  }
  return difficultyTableFolder || (clearMarkFolder && tableLevel.empty());
}

} // namespace main_menu_library
