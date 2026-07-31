#include "MainMenuLibrary.h"

#include "../BmsMetadataText.h"
#include "../CanonicalDigest.h"

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

std::vector<ChartEntry>
downloadedPathScanEntries(const std::filesystem::path &path) {
  if (path.empty()) {
    return {};
  }
  return {{.path = fspath_to_path_t(path)}};
}

namespace {

std::string canonicalHash(const std::string &value, std::size_t size) {
  std::string normalized =
      asobmshow::bms_metadata::normalizedHash(value);
  return canonical_digest::isCanonicalLowerHex(normalized, size)
             ? normalized
             : std::string();
}

bool pathWithin(const std::filesystem::path &path,
                const std::filesystem::path &root) {
  if (path.empty() || root.empty()) {
    return false;
  }
  const std::filesystem::path normalizedPath = path.lexically_normal();
  const std::filesystem::path normalizedRoot = root.lexically_normal();
  if (normalizedPath == normalizedRoot) {
    return true;
  }
  const std::filesystem::path relative =
      normalizedPath.lexically_relative(normalizedRoot);
  if (relative.empty() || relative.is_absolute()) {
    return false;
  }
  const auto first = relative.begin();
  return first != relative.end() && *first != std::filesystem::path("..") &&
         *first != std::filesystem::path(".");
}

bool identityMatches(const FindBmsChartIdentity &target,
                     const bms_parser::ChartMeta &candidate) {
  const FindBmsChartIdentity current = findBmsChartIdentity(candidate);
  if (!target.sha256.empty()) {
    return current.sha256 == target.sha256;
  }
  return !target.md5.empty() && current.md5 == target.md5;
}

} // namespace

FindBmsChartIdentity findBmsChartIdentity(const bms_parser::ChartMeta &meta) {
  return {
      .sha256 = canonicalHash(meta.SHA256, 64),
      .md5 = canonicalHash(meta.MD5, 32),
  };
}

bool sameChartSelection(const ChartMetaRecord &left,
                        const ChartMetaRecord &right) {
  const bool leftHasPath = !left.meta.BmsPath.empty();
  const bool rightHasPath = !right.meta.BmsPath.empty();
  if (leftHasPath || rightHasPath) {
    return leftHasPath && rightHasPath &&
           left.meta.BmsPath.lexically_normal() ==
               right.meta.BmsPath.lexically_normal() &&
           left.courseStart == right.courseStart &&
           left.unavailable == right.unavailable &&
           left.solidArchive == right.solidArchive;
  }

  const FindBmsChartIdentity leftIdentity = findBmsChartIdentity(left.meta);
  const FindBmsChartIdentity rightIdentity = findBmsChartIdentity(right.meta);
  if (!leftIdentity.sha256.empty() || !rightIdentity.sha256.empty()) {
    return !leftIdentity.sha256.empty() &&
           leftIdentity.sha256 == rightIdentity.sha256;
  }
  if (!leftIdentity.md5.empty() || !rightIdentity.md5.empty()) {
    return !leftIdentity.md5.empty() && leftIdentity.md5 == rightIdentity.md5;
  }
  return left.meta.Title == right.meta.Title &&
         left.meta.SubTitle == right.meta.SubTitle &&
         left.meta.Artist == right.meta.Artist &&
         left.courseStart == right.courseStart &&
         left.unavailable == right.unavailable &&
         left.solidArchive == right.solidArchive;
}

std::uint64_t chartSelectionGenerationAfter(
    std::uint64_t currentGeneration,
    const std::optional<ChartMetaRecord> &currentSelection,
    const ChartMetaRecord &nextSelection) {
  return currentSelection.has_value() &&
                 sameChartSelection(*currentSelection, nextSelection)
             ? currentGeneration
             : currentGeneration + 1;
}

bool findBmsSelectionHandoffAllowed(
    std::uint64_t capturedGeneration, std::uint64_t currentGeneration,
    const FindBmsChartIdentity &target,
    const ChartMetaRecord &currentSelection) {
  return capturedGeneration == currentGeneration && target.valid() &&
         currentSelection.unavailable &&
         identityMatches(target, currentSelection.meta);
}

std::optional<std::filesystem::path> downloadedChartPath(
    const std::vector<bms_parser::ChartMeta> &matchingCharts,
    const std::filesystem::path &downloadedPath) {
  std::optional<std::filesystem::path> selected;
  for (const auto &chart : matchingCharts) {
    const std::filesystem::path candidate = chart.BmsPath.lexically_normal();
    if (!pathWithin(candidate, downloadedPath)) {
      continue;
    }
    if (!selected.has_value() || candidate < *selected) {
      selected = candidate;
    }
  }
  return selected;
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

std::filesystem::path chartSelectionPathForReload(
    const std::filesystem::path &visibleSelectionPath,
    const std::optional<ChartMetaRecord> &retainedSelection) {
  if (!visibleSelectionPath.empty()) {
    return visibleSelectionPath;
  }
  if (retainedSelection.has_value()) {
    return retainedSelection->meta.BmsPath;
  }
  return {};
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
