#pragma once

#include "repositories/ChartRepository.h"
#include "scene/play/RhythmState.h"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct ChartRecordFilters {
  std::optional<int> clearMarkRank;
  bool clearMarkOrAbove = false;
  bool clearMarkOrBelow = false;
  std::optional<std::string> scoreRank;
  bool scoreRankOrAbove = false;
  bool scoreRankOrBelow = false;
  std::optional<double> bpmMin;
  std::optional<double> bpmMax;
  std::optional<std::string> difficultyMinLevel;
  std::optional<std::string> difficultyMaxLevel;
  ChartRecordSortState sort;
};

namespace chart_record_filters {

inline ChartRecordSortDirection
defaultDirectionFor(ChartRecordSortCriterion criterion) {
  switch (criterion) {
  case ChartRecordSortCriterion::Title:
  case ChartRecordSortCriterion::MinBpm:
  case ChartRecordSortCriterion::MainBpm:
  case ChartRecordSortCriterion::Difficulty:
    return ChartRecordSortDirection::Ascending;
  case ChartRecordSortCriterion::Default:
  case ChartRecordSortCriterion::ClearMark:
  case ChartRecordSortCriterion::Score:
  case ChartRecordSortCriterion::MaxBpm:
    return ChartRecordSortDirection::Descending;
  }
  return ChartRecordSortDirection::Descending;
}

inline ChartRecordSortDirection opposite(ChartRecordSortDirection direction) {
  return direction == ChartRecordSortDirection::Ascending
             ? ChartRecordSortDirection::Descending
             : ChartRecordSortDirection::Ascending;
}

inline ChartRecordSortState
nextSortState(ChartRecordSortState current,
              ChartRecordSortCriterion criterion) {
  if (criterion == ChartRecordSortCriterion::Default) {
    return {};
  }
  if (current.criterion == criterion) {
    current.direction = opposite(current.direction);
    return current;
  }
  return {.criterion = criterion, .direction = defaultDirectionFor(criterion)};
}

inline bool hasActiveCriteria(const ChartRecordFilters &filters) {
  return filters.clearMarkRank.has_value() || filters.scoreRank.has_value() ||
         filters.bpmMin.has_value() || filters.bpmMax.has_value() ||
         filters.difficultyMinLevel.has_value() ||
         filters.difficultyMaxLevel.has_value() ||
         filters.sort.criterion != ChartRecordSortCriterion::Default;
}

inline bool normalizeBpmRange(ChartRecordFilters &filters) {
  if (filters.bpmMin.has_value() && filters.bpmMax.has_value() &&
      *filters.bpmMin > *filters.bpmMax) {
    std::swap(filters.bpmMin, filters.bpmMax);
    return true;
  }
  return false;
}

inline bool normalizeBpmRange(ChartRecordFilters &filters,
                              std::string &bpmMinText,
                              std::string &bpmMaxText) {
  const bool swapped = normalizeBpmRange(filters);
  if (swapped) {
    std::swap(bpmMinText, bpmMaxText);
  }
  return swapped;
}

inline std::optional<size_t>
difficultyLevelIndex(const std::vector<DifficultyLevelInfo> &levels,
                     const std::optional<std::string> &level) {
  if (!level.has_value()) {
    return std::nullopt;
  }
  for (size_t i = 0; i < levels.size(); ++i) {
    if (levels[i].level == *level) {
      return i;
    }
  }
  return std::nullopt;
}

inline void removeUnavailableDifficultyLevels(
    ChartRecordFilters &filters,
    const std::vector<DifficultyLevelInfo> &levels) {
  auto minIndex = difficultyLevelIndex(levels, filters.difficultyMinLevel);
  auto maxIndex = difficultyLevelIndex(levels, filters.difficultyMaxLevel);
  if (filters.difficultyMinLevel.has_value() && !minIndex.has_value()) {
    filters.difficultyMinLevel.reset();
  }
  if (filters.difficultyMaxLevel.has_value() && !maxIndex.has_value()) {
    filters.difficultyMaxLevel.reset();
  }
}

inline void normalizeDifficultyRange(
    ChartRecordFilters &filters,
    const std::vector<DifficultyLevelInfo> &levels) {
  removeUnavailableDifficultyLevels(filters, levels);
  auto minIndex = difficultyLevelIndex(levels, filters.difficultyMinLevel);
  auto maxIndex = difficultyLevelIndex(levels, filters.difficultyMaxLevel);
  if (minIndex.has_value() && maxIndex.has_value() && *minIndex > *maxIndex) {
    std::swap(filters.difficultyMinLevel, filters.difficultyMaxLevel);
  }
}

inline bool resetDifficultyRangeOnTableChange(
    ChartRecordFilters &filters, std::optional<int> &activeTableId,
    int tableId) {
  if (activeTableId.has_value() && *activeTableId == tableId) {
    return false;
  }
  activeTableId = tableId;
  filters.difficultyMinLevel.reset();
  filters.difficultyMaxLevel.reset();
  return true;
}

inline void setDifficultyMinLevel(
    ChartRecordFilters &filters,
    const std::vector<DifficultyLevelInfo> &levels,
    std::optional<std::string> level) {
  filters.difficultyMinLevel = std::move(level);
  removeUnavailableDifficultyLevels(filters, levels);
  const auto minIndex = difficultyLevelIndex(levels, filters.difficultyMinLevel);
  const auto maxIndex = difficultyLevelIndex(levels, filters.difficultyMaxLevel);
  if (minIndex.has_value() && maxIndex.has_value() && *minIndex > *maxIndex) {
    filters.difficultyMaxLevel = filters.difficultyMinLevel;
  }
}

inline void setDifficultyMaxLevel(
    ChartRecordFilters &filters,
    const std::vector<DifficultyLevelInfo> &levels,
    std::optional<std::string> level) {
  filters.difficultyMaxLevel = std::move(level);
  removeUnavailableDifficultyLevels(filters, levels);
  const auto minIndex = difficultyLevelIndex(levels, filters.difficultyMinLevel);
  const auto maxIndex = difficultyLevelIndex(levels, filters.difficultyMaxLevel);
  if (minIndex.has_value() && maxIndex.has_value() && *minIndex > *maxIndex) {
    filters.difficultyMinLevel = filters.difficultyMaxLevel;
  }
}

inline bool scoreRankFilterEnabled(std::optional<int> clearMarkRank) {
  return !clearMarkRank.has_value() || *clearMarkRank != kNoClearTypeRank;
}

inline void
normalizeSelection(ChartRecordFilters &filters,
                   std::optional<int> fallbackClearMarkRank = std::nullopt) {
  const std::optional<int> effectiveClearMarkRank =
      filters.clearMarkRank.has_value() ? filters.clearMarkRank
                                        : fallbackClearMarkRank;
  if (!scoreRankFilterEnabled(effectiveClearMarkRank)) {
    filters.scoreRank.reset();
    filters.scoreRankOrAbove = false;
    filters.scoreRankOrBelow = false;
  }
}

inline void applyToQuery(ChartMetaQuery &query, ChartRecordFilters filters,
                         bool enableDifficultyRange) {
  const std::optional<int> queryClearMarkRank =
      query.clearMarkFilter ? std::optional<int>(query.clearMarkRank)
                            : std::nullopt;
  normalizeSelection(filters, queryClearMarkRank);
  normalizeBpmRange(filters);
  if (filters.clearMarkRank.has_value()) {
    query.clearMarkFilter = true;
    query.clearMarkRank = *filters.clearMarkRank;
    query.clearMarkOrAbove = filters.clearMarkOrAbove;
    query.clearMarkOrBelow =
        !filters.clearMarkOrAbove && filters.clearMarkOrBelow;
  }
  query.scoreRank = filters.scoreRank;
  if (filters.scoreRank.has_value()) {
    query.scoreRankOrAbove = filters.scoreRankOrAbove;
    query.scoreRankOrBelow =
        !filters.scoreRankOrAbove && filters.scoreRankOrBelow;
  }
  query.bpmMin = filters.bpmMin;
  query.bpmMax = filters.bpmMax;
  if (enableDifficultyRange) {
    query.difficultyMinLevel = filters.difficultyMinLevel;
    query.difficultyMaxLevel = filters.difficultyMaxLevel;
  } else {
    query.difficultyMinLevel.reset();
    query.difficultyMaxLevel.reset();
  }
  query.sortCriterion =
      enableDifficultyRange ||
              filters.sort.criterion != ChartRecordSortCriterion::Difficulty
          ? filters.sort.criterion
          : ChartRecordSortCriterion::Default;
  query.sortDirection = filters.sort.direction;
}

} // namespace chart_record_filters
