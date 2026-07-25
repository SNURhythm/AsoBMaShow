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

std::optional<std::filesystem::path>
sameFolderForChart(const ChartMetaRecord &record) {
  std::filesystem::path folder = record.meta.Folder;
  if (folder.empty() && !record.meta.BmsPath.empty()) {
    folder = record.meta.BmsPath.parent_path();
  }
  if (folder.empty()) {
    return std::nullopt;
  }
  return folder.lexically_normal();
}

ChartRecordFilters
filtersForSameFolder(const ChartRecordFilters &currentFilters) {
  ChartRecordFilters filters;
  filters.sort = currentFilters.sort;
  return filters;
}

ChartMetaQuery chartQueryForSameFolder(
    const std::filesystem::path &folder, const std::string &keyword,
    const ChartRecordFilters &filters, int selectedLongNoteMode) {
  ChartMetaQuery query;
  query.keyword = keyword;
  query.exactFolder = folder.lexically_normal();
  query.selectedLongNoteMode = selectedLongNoteMode;
  chart_record_filters::applyToQuery(query, filters, false);
  query.sortCriterion = filters.sort.criterion;
  query.sortDirection = filters.sort.direction;
  return query;
}

float centeredScrollOffsetForItem(int selectedIndex, int itemCount,
                                  int itemHeight,
                                  int viewportHeight) noexcept {
  if (selectedIndex < 0 || selectedIndex >= itemCount || itemCount <= 0 ||
      itemHeight <= 0 || viewportHeight <= 0) {
    return 0.0f;
  }
  const float selectedY = static_cast<float>(selectedIndex * itemHeight);
  const float centeredOffset =
      selectedY - static_cast<float>(std::max(0, viewportHeight - itemHeight)) /
                      2.0f;
  const float maxOffset = static_cast<float>(
      std::max(0, std::max(1, itemCount) * itemHeight - viewportHeight));
  return std::clamp(centeredOffset, 0.0f, maxOffset);
}

} // namespace main_menu_library
