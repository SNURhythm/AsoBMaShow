#pragma once

#include "../ChartRecordFilters.h"
#include "../LibraryFolderClearData.h"
#include "../repositories/ChartRepository.h"
#include "../targets.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

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

void appendUniqueScanFolder(std::vector<ChartEntry> &entries,
                            const std::filesystem::path &folder);

std::optional<std::filesystem::path>
sameFolderForChart(const ChartMetaRecord &record);

ChartRecordFilters
filtersForSameFolder(const ChartRecordFilters &currentFilters);

ChartMetaQuery chartQueryForSameFolder(
    const std::filesystem::path &folder, const std::string &keyword,
    const ChartRecordFilters &filters, int selectedLongNoteMode);

std::filesystem::path chartSelectionPathForReload(
    const std::filesystem::path &visibleSelectionPath,
    const std::optional<ChartMetaRecord> &retainedSelection);

float centeredScrollOffsetForItem(int selectedIndex, int itemCount,
                                  int itemHeight,
                                  int viewportHeight) noexcept;

inline bool difficultyRangeEnabledForFolder(bool difficultyTableFolder,
                                            bool clearMarkFolder, int tableId,
                                            const std::string &tableLevel) {
  if (tableId <= 0) {
    return false;
  }
  return difficultyTableFolder || (clearMarkFolder && tableLevel.empty());
}

} // namespace main_menu_library
