#include "MainMenuLibrary.h"

namespace main_menu_library {

std::string folderKeyForTable(int tableId) {
  return chart_library::folderKeyForTable(tableId);
}

std::string folderKeyForLevel(int tableId, const std::string &level) {
  return chart_library::folderKeyForLevel(tableId, level);
}

std::string folderKeyForCourseTable(int tableId) {
  return chart_library::folderKeyForCourseTable(tableId);
}

std::string folderKeyForCourseGroup(int tableId,
                                    const std::string &groupName) {
  return chart_library::folderKeyForCourseGroup(tableId, groupName);
}

std::string folderKeyForCourse(int courseId) {
  return chart_library::folderKeyForCourse(courseId);
}

} // namespace main_menu_library
