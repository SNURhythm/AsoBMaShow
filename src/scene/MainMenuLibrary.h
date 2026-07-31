#pragma once

#include "../ChartRecordFilters.h"
#include "../LibraryFolderClearData.h"
#include "../repositories/ChartRepository.h"
#include "../targets.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace main_menu_library {

struct FindBmsChartIdentity {
  std::string sha256;
  std::string md5;

  [[nodiscard]] bool valid() const noexcept {
    return !sha256.empty() || !md5.empty();
  }
};

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

std::vector<ChartEntry>
downloadedPathScanEntries(const std::filesystem::path &path);

FindBmsChartIdentity
findBmsChartIdentity(const bms_parser::ChartMeta &meta);

bool sameChartSelection(const ChartMetaRecord &left,
                        const ChartMetaRecord &right);

std::uint64_t chartSelectionGenerationAfter(
    std::uint64_t currentGeneration,
    const std::optional<ChartMetaRecord> &currentSelection,
    const ChartMetaRecord &nextSelection);

bool findBmsSelectionHandoffAllowed(
    std::uint64_t capturedGeneration, std::uint64_t currentGeneration,
    const FindBmsChartIdentity &target,
    const ChartMetaRecord &currentSelection);

std::optional<std::filesystem::path> downloadedChartPath(
    const std::vector<bms_parser::ChartMeta> &matchingCharts,
    const std::filesystem::path &downloadedPath);

std::optional<std::filesystem::path> downloadedChartPath(
    const std::vector<bms_parser::ChartMeta> &matchingCharts,
    const std::filesystem::path &downloadedPath,
    const std::vector<std::filesystem::path> &upsertedChartPaths);

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
