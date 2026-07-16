#pragma once

#include <array>
#include <string>
#include <unordered_map>

namespace chart_library {

using ClearMarkCountMap = std::unordered_map<int, int>;
using FolderClearMarkCounts = std::unordered_map<std::string, ClearMarkCountMap>;
using FolderClearRankMap = std::unordered_map<std::string, int>;

struct FolderClearDataByLongNoteMode {
  std::array<FolderClearRankMap, 4> clearRanks;
  std::array<FolderClearMarkCounts, 4> clearMarkCounts;
};

inline std::string folderKeyForTable(int tableId) {
  return "table:" + std::to_string(tableId);
}

inline std::string folderKeyForLevel(int tableId, const std::string &level) {
  return "level:" + std::to_string(tableId) + ":" + level;
}

inline std::string folderKeyForCourseTable(int tableId) {
  return "course-table:" + std::to_string(tableId);
}

inline std::string folderKeyForCourseGroup(int tableId,
                                           const std::string &groupName) {
  return "course-group:" + std::to_string(tableId) + ":" + groupName;
}

inline std::string folderKeyForCourse(int courseId) {
  return "course:" + std::to_string(courseId);
}

} // namespace chart_library
