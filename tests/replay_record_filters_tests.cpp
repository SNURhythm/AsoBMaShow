#include "../src/ReplayRecordFilters.h"

#include <iostream>

#define ASSERT_EQ(expected, actual, label)                                     \
  if ((expected) != (actual)) {                                                \
    std::cerr << label << " expected " << (expected) << " actual "            \
              << (actual) << std::endl;                                       \
    return 1;                                                                 \
  }

namespace {
ReplaySummary makeSummary(int id, int clearType, int score, int maxScore,
                          int maxCombo,
                          std::optional<std::string> option = std::nullopt) {
  ReplaySummary summary;
  summary.id = id;
  summary.clearType = clearType;
  summary.finalScore = score;
  summary.maxScore = maxScore;
  summary.maxCombo = maxCombo;
  summary.playOption = option;
  return summary;
}
} // namespace

int main() {
  std::vector<ReplaySummary> summaries = {
      makeSummary(1, kClearTypeFailedRank, 600, 1000, 120, std::nullopt),
      makeSummary(2, kClearTypeFullComboRank, 900, 1000, 450, "MIRROR"),
      makeSummary(3, kClearTypeHardClearRank, 800, 1000, 300, "RANDOM"),
  };

  ReplayRecordFilters filters;
  filters.clearMarkRank = kClearTypeFullComboRank;
  auto filtered = replay_record_filters::apply(summaries, filters);
  ASSERT_EQ(1U, filtered.size(), "full combo filter size");
  ASSERT_EQ(2, filtered[0].id, "full combo filter id");

  filters = {};
  filters.clearMarkRank = kClearTypeFullComboRank;
  std::vector<ReplaySummary> effectiveFullComboSummaries = {
      makeSummary(4, kClearTypeNormalClearRank, 700, 1000, 500),
      makeSummary(5, kClearTypeNormalClearRank, 700, 1000, 499),
  };
  filtered =
      replay_record_filters::apply(effectiveFullComboSummaries, filters);
  ASSERT_EQ(1U, filtered.size(), "effective full combo filter size");
  ASSERT_EQ(4, filtered[0].id, "effective full combo filter id");

  filters = {};
  filters.playOption = "RANDOM";
  filtered = replay_record_filters::apply(summaries, filters);
  ASSERT_EQ(1U, filtered.size(), "play option filter size");
  ASSERT_EQ(3, filtered[0].id, "play option filter id");

  filters = {};
  filters.scoreRank = "AAA";
  filtered = replay_record_filters::apply(summaries, filters);
  ASSERT_EQ(1U, filtered.size(), "score rank filter size");
  ASSERT_EQ(2, filtered[0].id, "score rank filter id");

  filters = {};
  filters.sort = ReplayRecordSortCriterion::ClearMark;
  filtered = replay_record_filters::apply(summaries, filters);
  ASSERT_EQ(2, filtered[0].id, "clear sort first id");

  filters.sort = ReplayRecordSortCriterion::ClearMark;
  std::vector<ReplaySummary> tiedClearMarkSummaries = {
      makeSummary(31, kClearTypeHardClearRank, 850, 1000, 260),
      makeSummary(32, kClearTypeHardClearRank, 850, 1000, 180),
  };
  filtered = replay_record_filters::apply(tiedClearMarkSummaries, filters);
  ASSERT_EQ(31, filtered[0].id, "clear sort max combo tie id");

  filters.sort = ReplayRecordSortCriterion::Score;
  filtered = replay_record_filters::apply(summaries, filters);
  ASSERT_EQ(2, filtered[0].id, "score sort first id");

  filters.sort = ReplayRecordSortCriterion::Score;
  std::vector<ReplaySummary> tiedScoreSummaries = {
      makeSummary(42, kClearTypeHardClearRank, 900, 1000, 220),
      makeSummary(41, kClearTypeNormalClearRank, 900, 1000, 360),
  };
  filtered = replay_record_filters::apply(tiedScoreSummaries, filters);
  ASSERT_EQ(42, filtered[0].id, "score sort clear mark tie id");

  filters.sort = ReplayRecordSortCriterion::MaxCombo;
  filtered = replay_record_filters::apply(summaries, filters);
  ASSERT_EQ(2, filtered[0].id, "max combo sort first id");

  filters.sort = ReplayRecordSortCriterion::MaxCombo;
  std::vector<ReplaySummary> clearMarkVsMaxComboSummaries = {
      makeSummary(50, kClearTypeHardClearRank, 740, 1000, 220),
      makeSummary(49, kClearTypeNormalClearRank, 900, 1000, 360),
  };
  filtered = replay_record_filters::apply(clearMarkVsMaxComboSummaries, filters);
  ASSERT_EQ(49, filtered[0].id, "max combo sort combo precedence id");

  filters.sort = ReplayRecordSortCriterion::MaxCombo;
  std::vector<ReplaySummary> tiedMaxComboSummaries = {
      makeSummary(51, kClearTypeNormalClearRank, 740, 1000, 360),
      makeSummary(52, kClearTypeNormalClearRank, 810, 1000, 360),
  };
  filtered = replay_record_filters::apply(tiedMaxComboSummaries, filters);
  ASSERT_EQ(52, filtered[0].id, "max combo sort score tie id");

  filters.sort = ReplayRecordSortCriterion::MaxCombo;
  std::vector<ReplaySummary> tiedMaxComboAndScoreSummaries = {
      makeSummary(61, kClearTypeNormalClearRank, 810, 1000, 360),
      makeSummary(62, kClearTypeHardClearRank, 810, 1000, 360),
  };
  filtered =
      replay_record_filters::apply(tiedMaxComboAndScoreSummaries, filters);
  ASSERT_EQ(62, filtered[0].id, "max combo sort clear mark tie id");

  filters.sort = ReplayRecordSortCriterion::Newest;
  filtered = replay_record_filters::apply(summaries, filters);
  ASSERT_EQ(3, filtered[0].id, "newest sort first id");

  return 0;
}
