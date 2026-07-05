#include "../src/ChartRecordFilters.h"
#include "../src/scene/MainMenuLibrary.h"
#include "../src/scene/play/RhythmState.h"

#include <iostream>

#define ASSERT_EQ(expected, actual, label)                                     \
  if ((expected) != (actual)) {                                                \
    std::cerr << label << " expected " << (expected) << " actual "            \
              << (actual) << std::endl;                                       \
    return 1;                                                                 \
  }

#define ASSERT_TRUE(value, label)                                              \
  if (!(value)) {                                                              \
    std::cerr << label << " expected true" << std::endl;                      \
    return 1;                                                                 \
  }

#define ASSERT_FALSE(value, label)                                             \
  if (value) {                                                                \
    std::cerr << label << " expected false" << std::endl;                     \
    return 1;                                                                 \
  }

#define ASSERT_ENUM_EQ(expected, actual, label)                                \
  if ((expected) != (actual)) {                                                \
    std::cerr << label << " expected "                                        \
              << static_cast<int>(expected) << " actual "                     \
              << static_cast<int>(actual) << std::endl;                       \
    return 1;                                                                 \
  }

int main() {
  ChartRecordFilters filters;
  ASSERT_FALSE(chart_record_filters::hasActiveCriteria(filters),
               "empty filters active");

  ASSERT_TRUE(main_menu_library::difficultyRangeEnabledForFolder(
                  true, false, 1, ""),
              "difficulty table enables difficulty range");
  ASSERT_TRUE(main_menu_library::difficultyRangeEnabledForFolder(
                  false, true, 1, ""),
              "difficulty table direct clear mark enables difficulty range");
  ASSERT_FALSE(main_menu_library::difficultyRangeEnabledForFolder(
                   false, true, 1, "12"),
               "difficulty level clear mark disables difficulty range");
  ASSERT_FALSE(main_menu_library::difficultyRangeEnabledForFolder(
                   false, true, 0, ""),
               "non-table clear mark disables difficulty range");

  filters.sort = chart_record_filters::nextSortState(
      filters.sort, ChartRecordSortCriterion::Score);
  ASSERT_ENUM_EQ(ChartRecordSortCriterion::Score, filters.sort.criterion,
                 "score sort criterion");
  ASSERT_ENUM_EQ(ChartRecordSortDirection::Descending, filters.sort.direction,
                 "score default direction");

  filters.sort = chart_record_filters::nextSortState(
      filters.sort, ChartRecordSortCriterion::Score);
  ASSERT_ENUM_EQ(ChartRecordSortDirection::Ascending, filters.sort.direction,
                 "score toggled direction");

  filters.sort = chart_record_filters::nextSortState(
      filters.sort, ChartRecordSortCriterion::Title);
  ASSERT_ENUM_EQ(ChartRecordSortCriterion::Title, filters.sort.criterion,
                 "title sort criterion");
  ASSERT_ENUM_EQ(ChartRecordSortDirection::Ascending, filters.sort.direction,
                 "title default direction");

  ASSERT_TRUE(chart_record_filters::scoreRankFilterEnabled(std::nullopt),
              "score rank enabled without clear mark");
  ASSERT_TRUE(chart_record_filters::scoreRankFilterEnabled(
                  kClearTypeFailedRank),
              "score rank enabled with played clear mark");
  ASSERT_FALSE(chart_record_filters::scoreRankFilterEnabled(kNoClearTypeRank),
               "score rank disabled with no play clear mark");

  filters = {};
  filters.clearMarkRank = kClearTypeHardClearRank;
  filters.scoreRank = "AAA";
  filters.bpmMin = 150.0;
  filters.bpmMax = 180.0;
  filters.difficultyMinLevel = "2";
  filters.difficultyMaxLevel = "4";
  filters.sort = {ChartRecordSortCriterion::Difficulty,
                  ChartRecordSortDirection::Descending};

  ChartMetaQuery query;
  chart_record_filters::applyToQuery(query, filters, true);
  ASSERT_TRUE(query.clearMarkFilter, "clear mark query filter");
  ASSERT_EQ(kClearTypeHardClearRank, query.clearMarkRank,
            "clear mark query rank");
  ASSERT_TRUE(query.scoreRank.has_value(), "score rank query has value");
  ASSERT_EQ(std::string("AAA"), *query.scoreRank, "score rank query value");
  ASSERT_TRUE(query.bpmMin.has_value(), "bpm min query has value");
  ASSERT_EQ(150.0, *query.bpmMin, "bpm min query value");
  ASSERT_TRUE(query.bpmMax.has_value(), "bpm max query has value");
  ASSERT_EQ(180.0, *query.bpmMax, "bpm max query value");
  ASSERT_TRUE(query.difficultyMinLevel.has_value(),
              "difficulty min query has value");
  ASSERT_EQ(std::string("2"), *query.difficultyMinLevel,
            "difficulty min query value");
  ASSERT_TRUE(query.difficultyMaxLevel.has_value(),
              "difficulty max query has value");
  ASSERT_EQ(std::string("4"), *query.difficultyMaxLevel,
            "difficulty max query value");
  ASSERT_ENUM_EQ(ChartRecordSortCriterion::Difficulty, query.sortCriterion,
                 "query sort criterion");
  ASSERT_ENUM_EQ(ChartRecordSortDirection::Descending, query.sortDirection,
                 "query sort direction");

  ChartMetaQuery nonDifficultyQuery;
  chart_record_filters::applyToQuery(nonDifficultyQuery, filters, false);
  ASSERT_FALSE(nonDifficultyQuery.difficultyMinLevel.has_value(),
               "disabled difficulty min query");
  ASSERT_FALSE(nonDifficultyQuery.difficultyMaxLevel.has_value(),
               "disabled difficulty max query");

  filters = {};
  query = {};
  query.clearMarkFilter = true;
  query.clearMarkRank = kClearTypeEasyClearRank;
  chart_record_filters::applyToQuery(query, filters, false);
  ASSERT_TRUE(query.clearMarkFilter, "folder clear mark preserved");
  ASSERT_EQ(kClearTypeEasyClearRank, query.clearMarkRank,
            "folder clear mark rank preserved");

  filters.clearMarkRank = kClearTypeFullComboRank;
  chart_record_filters::applyToQuery(query, filters, false);
  ASSERT_TRUE(query.clearMarkFilter, "filter clear mark override enabled");
  ASSERT_EQ(kClearTypeFullComboRank, query.clearMarkRank,
            "filter clear mark rank override");

  filters = {};
  filters.scoreRank = "AAA";
  query = {};
  query.clearMarkFilter = true;
  query.clearMarkRank = kNoClearTypeRank;
  chart_record_filters::applyToQuery(query, filters, false);
  ASSERT_FALSE(query.scoreRank.has_value(),
               "no play folder clear mark disables score rank query");

  filters = {};
  filters.clearMarkRank = kClearTypeFullComboRank;
  filters.clearMarkOrAbove = true;
  filters.clearMarkOrBelow = true;
  filters.scoreRank = "AA";
  filters.scoreRankOrAbove = true;
  filters.scoreRankOrBelow = true;
  query = {};
  chart_record_filters::applyToQuery(query, filters, false);
  ASSERT_TRUE(query.clearMarkOrAbove, "clear mark or above query");
  ASSERT_FALSE(query.clearMarkOrBelow,
               "clear mark above takes precedence over below query");
  ASSERT_TRUE(query.scoreRankOrAbove, "score rank or above query");
  ASSERT_FALSE(query.scoreRankOrBelow,
               "score rank above takes precedence over below query");

  filters.clearMarkRank.reset();
  filters.scoreRank.reset();
  query = {};
  chart_record_filters::applyToQuery(query, filters, false);
  ASSERT_FALSE(query.clearMarkFilter,
               "clear mark range disabled without selected rank");
  ASSERT_FALSE(query.clearMarkOrAbove,
               "clear mark above disabled without selected rank");
  ASSERT_FALSE(query.clearMarkOrBelow,
               "clear mark below disabled without selected rank");
  ASSERT_FALSE(query.scoreRank.has_value(),
               "score rank disabled without selected rank");
  ASSERT_FALSE(query.scoreRankOrAbove,
               "score rank above disabled without selected rank");
  ASSERT_FALSE(query.scoreRankOrBelow,
               "score rank below disabled without selected rank");

  filters = {};
  filters.clearMarkRank = kNoClearTypeRank;
  filters.scoreRank = "AAA";
  filters.scoreRankOrAbove = true;
  filters.scoreRankOrBelow = true;
  chart_record_filters::normalizeSelection(filters);
  ASSERT_FALSE(filters.scoreRank.has_value(),
               "no play clear mark resets score rank");
  ASSERT_FALSE(filters.scoreRankOrAbove,
               "no play clear mark resets score rank above");
  ASSERT_FALSE(filters.scoreRankOrBelow,
               "no play clear mark resets score rank below");

  filters = {};
  filters.bpmMin = 220.0;
  filters.bpmMax = 120.0;
  query = {};
  chart_record_filters::applyToQuery(query, filters, false);
  ASSERT_EQ(120.0, *query.bpmMin, "normalized bpm min query value");
  ASSERT_EQ(220.0, *query.bpmMax, "normalized bpm max query value");

  const std::vector<DifficultyLevelInfo> difficultyLevels = {
      {.level = "4"}, {.level = "5"}, {.level = "6"}};

  filters = {};
  filters.difficultyMaxLevel = "4";
  chart_record_filters::setDifficultyMinLevel(filters, difficultyLevels, "5");
  ASSERT_EQ(std::string("5"), *filters.difficultyMinLevel,
            "difficulty min changed side wins");
  ASSERT_EQ(std::string("5"), *filters.difficultyMaxLevel,
            "difficulty max clamps to changed min");

  filters = {};
  filters.difficultyMinLevel = "5";
  chart_record_filters::setDifficultyMaxLevel(filters, difficultyLevels, "4");
  ASSERT_EQ(std::string("4"), *filters.difficultyMinLevel,
            "difficulty min clamps to changed max");
  ASSERT_EQ(std::string("4"), *filters.difficultyMaxLevel,
            "difficulty max changed side wins");

  return 0;
}
