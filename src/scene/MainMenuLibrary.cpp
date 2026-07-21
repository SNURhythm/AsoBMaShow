#include "MainMenuLibrary.h"

#include <algorithm>

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

void appendUniqueScanFolder(std::vector<ChartEntry> &entries,
                            const std::filesystem::path &folder) {
  if (folder.empty()) {
    return;
  }
  const auto normalizedFolder = folder.lexically_normal();
  const bool alreadyIncluded =
      std::any_of(entries.begin(), entries.end(), [&](const ChartEntry &entry) {
        return std::filesystem::path(entry.path).lexically_normal() ==
               normalizedFolder;
      });
  if (!alreadyIncluded) {
    entries.push_back({.path = fspath_to_path_t(folder)});
  }
}

} // namespace main_menu_library
