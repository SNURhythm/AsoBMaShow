#pragma once

#include "../LibraryFolderClearData.h"
#include "../targets.h"

#include <string>

namespace main_menu_library {

enum class EmptyLibraryBootstrapMode {
  DefaultFolder,
  FolderPicker,
};

constexpr EmptyLibraryBootstrapMode
emptyLibraryBootstrapMode(TargetPlatform platform) noexcept {
  switch (platform) {
  case TargetPlatform::iOS:
  case TargetPlatform::Android:
    return EmptyLibraryBootstrapMode::DefaultFolder;
  case TargetPlatform::Windows:
  case TargetPlatform::MacOS:
  case TargetPlatform::Linux:
    return EmptyLibraryBootstrapMode::FolderPicker;
  }
  return EmptyLibraryBootstrapMode::FolderPicker;
}

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
