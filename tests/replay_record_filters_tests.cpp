#include "../src/ReplayRecordFilters.h"
#include "../src/ResultRecordSummary.h"

#include <iostream>

#define ASSERT_EQ(expected, actual, label)                                     \
  if ((expected) != (actual)) {                                                \
    std::cerr << label << " expected " << (expected) << " actual " << (actual) \
              << std::endl;                                                    \
    return 1;                                                                  \
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

ResultRecordSummary
makeResultRecord(int id, int clearType, int score, int maxScore,
                 std::optional<int> maxCombo, std::int64_t displayedTime,
                 std::optional<std::string> option = std::nullopt,
                 bool course = false, bool autoPlay = false) {
  ReplaySummary local =
      makeSummary(id, clearType, score, maxScore, maxCombo.value_or(0), option);
  local.maxCombo = maxCombo.value_or(0);
  local.courseReplay = course;
  local.autoPlay = autoPlay;
  ResultRecordSummary result = makeLocalResultRecord(std::move(local));
  result.maxCombo = maxCombo;
  result.displayedTimeUnixMillis = displayedTime;
  return result;
}

ResultRecordSummary makeRemoteResultRecordForFilter(
    std::string id, int clearType, int score, int maxScore,
    std::optional<int> maxCombo, std::int64_t displayedTime,
    std::optional<std::string> option = std::nullopt) {
  return {
      .identity =
          IrRemoteRecordId{
              .providerId = "tachi",
              .serverOrigin = "https://boku.tachi.ac",
              .remoteScoreId = std::move(id),
          },
      .capabilities = {.resultRecall = true},
      .course = false,
      .autoPlay = false,
      .score = score,
      .maxScore = maxScore,
      .maxCombo = maxCombo,
      .clearRank = clearType,
      .displayedTimeUnixMillis = displayedTime,
      .displayedTime = {},
      .playOption = std::move(option),
      .irState = ir::IrRecordState::Uploaded,
      .local = std::nullopt,
      .remote = std::nullopt,
  };
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
  filtered = replay_record_filters::apply(effectiveFullComboSummaries, filters);
  ASSERT_EQ(1U, filtered.size(), "effective full combo filter size");
  ASSERT_EQ(4, filtered[0].id, "effective full combo filter id");

  ReplaySummary assistedFullCombo =
      makeSummary(6, kClearTypeNormalClearRank, 700, 1000, 500);
  assistedFullCombo.playback = {.percent = 75,
                                .mode = audio::PlaybackMode::PitchShift};
  ASSERT_EQ(kClearTypeAssistedEasyClearRank,
            replay_clear_mark::effectiveClearRank(assistedFullCombo),
            "rate-assisted effective full combo cap");
  filters = {};
  filters.clearMarkRank = kClearTypeFullComboRank;
  filtered = replay_record_filters::apply({assistedFullCombo}, filters);
  ASSERT_EQ(0U, filtered.size(), "assisted replay excluded from full combo");
  filters.clearMarkRank = kClearTypeAssistedEasyClearRank;
  filtered = replay_record_filters::apply({assistedFullCombo}, filters);
  ASSERT_EQ(1U, filtered.size(), "assisted replay included in assisted clear");

  ReplaySummary legacyNeutral =
      makeSummary(7, kClearTypeNormalClearRank, 700, 1000, 500);
  ASSERT_EQ(kClearTypeFullComboRank,
            replay_clear_mark::effectiveClearRank(legacyNeutral),
            "legacy neutral summary retains effective full combo");

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
  filters.scoreRank = "AAA";
  std::vector<ReplaySummary> courseSummaries = {
      makeSummary(21, kClearTypeNormalClearRank, 900, 0, 120),
      makeSummary(22, kClearTypeHardClearRank, 800, 0, 220),
  };
  courseSummaries[0].courseReplay = true;
  courseSummaries[1].courseReplay = true;
  filtered = replay_record_filters::apply(courseSummaries, filters);
  ASSERT_EQ(2U, filtered.size(), "course score rank filter ignored size");

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
  filtered =
      replay_record_filters::apply(clearMarkVsMaxComboSummaries, filters);
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

  ResultRecordSummary autoPlay = makeResultRecord(
      -1, kClearTypeFailedRank, 0, 2'000, 0, 0, std::nullopt, false, true);
  ResultRecordSummary remoteMissing = makeRemoteResultRecordForFilter(
      "remote-missing", kClearTypeFailedRank, 0, 0, std::nullopt, 500);
  ResultRecordSummary remoteExplicitZero = makeRemoteResultRecordForFilter(
      "remote-zero", kClearTypeNormalClearRank, 0, 2'000, 0, 400, "RANDOM");
  ResultRecordSummary course = makeResultRecord(
      70, kClearTypeHardClearRank, 850, 0, 350, 300, std::nullopt, true);
  course.local->playOption2 = "MIRROR";
  ResultRecordSummary localNormal =
      makeResultRecord(71, kClearTypeFullComboRank, 1'800, 2'000, 900, 200);
  ResultRecordSummary tiedNewest =
      makeRemoteResultRecordForFilter("remote-tied", kClearTypeEasyClearRank,
                                      1'200, 2'000, 600, 500, "R-RANDOM");
  LegacyChartResultSummary unknownLegacy;
  unknownLegacy.legacyReplayId = 99;
  unknownLegacy.partial = true;
  ResultRecordSummary legacyUnknown =
      makeLegacyChartResultRecord(unknownLegacy);

  std::vector<ResultRecordSummary> resultRecords{
      localNormal, remoteMissing,      tiedNewest,
      course,      remoteExplicitZero, autoPlay};
  ReplayRecordFilters resultFilters;
  auto filteredRecords =
      replay_record_filters::apply(resultRecords, resultFilters);
  ASSERT_EQ(-1, *filteredRecords[0].localReplayId(),
            "tagged newest keeps Auto Play first");
  ASSERT_EQ(std::string("remote-missing"),
            std::string(*filteredRecords[1].remoteScoreId()),
            "tagged newest sorts by displayed time");
  ASSERT_EQ(std::string("remote-tied"),
            std::string(*filteredRecords[2].remoteScoreId()),
            "tagged newest is stable for equal displayed time");

  resultFilters = {};
  resultFilters.clearMarkRank = kClearTypeHardClearRank;
  filteredRecords = replay_record_filters::apply(resultRecords, resultFilters);
  ASSERT_EQ(1U, filteredRecords.size(), "tagged clear filter size");
  ASSERT_EQ(70, *filteredRecords[0].localReplayId(),
            "tagged clear filter preserves local course");

  auto knownAndUnknown = resultRecords;
  knownAndUnknown.push_back(legacyUnknown);
  resultFilters = {};
  resultFilters.clearMarkRank = kClearTypeFailedRank;
  filteredRecords =
      replay_record_filters::apply(knownAndUnknown, resultFilters);
  ASSERT_EQ(2U, filteredRecords.size(),
            "unknown legacy lamp is not filtered as a failed lamp");
  resultFilters = {};
  resultFilters.scoreRank = "F";
  filteredRecords =
      replay_record_filters::apply(knownAndUnknown, resultFilters);
  ASSERT_EQ(2U, filteredRecords.size(),
            "unknown legacy score is not filtered as a zero score");

  resultFilters = {};
  resultFilters.playOption = "NORMAL";
  filteredRecords = replay_record_filters::apply(resultRecords, resultFilters);
  ASSERT_EQ(2U, filteredRecords.size(),
            "tagged NORMAL excludes missing remote option");
  ASSERT_EQ(-1, *filteredRecords[0].localReplayId(),
            "tagged NORMAL keeps Auto Play local semantics");
  ASSERT_EQ(71, *filteredRecords[1].localReplayId(),
            "tagged NORMAL keeps missing local option semantics");

  resultFilters.playOption = "MIRROR";
  filteredRecords = replay_record_filters::apply(resultRecords, resultFilters);
  ASSERT_EQ(1U, filteredRecords.size(),
            "tagged play option checks local secondary option");
  ASSERT_EQ(70, *filteredRecords[0].localReplayId(),
            "tagged play option retains local course option behavior");

  resultFilters = {};
  resultFilters.scoreRank = "F";
  filteredRecords = replay_record_filters::apply(resultRecords, resultFilters);
  ASSERT_EQ(2U, filteredRecords.size(),
            "tagged score filter excludes unavailable max score");
  ASSERT_EQ(-1, *filteredRecords[0].localReplayId(),
            "tagged score filter keeps explicit local zero score");
  ASSERT_EQ(std::string("remote-zero"),
            std::string(*filteredRecords[1].remoteScoreId()),
            "tagged score filter keeps explicit remote zero score");
  ASSERT_EQ(true, replay_record_filters::supportsScoreRankFilter(resultRecords),
            "tagged score-rank availability requires positive max score");
  ASSERT_EQ(false,
            replay_record_filters::supportsScoreRankFilter(
                std::vector<ResultRecordSummary>{remoteMissing, course}),
            "tagged score-rank unavailable without positive max score");

  resultFilters = {};
  resultFilters.sort = ReplayRecordSortCriterion::MaxCombo;
  filteredRecords = replay_record_filters::apply(resultRecords, resultFilters);
  ASSERT_EQ(-1, *filteredRecords[0].localReplayId(),
            "tagged combo sort keeps Auto Play first");
  ASSERT_EQ(71, *filteredRecords[1].localReplayId(),
            "tagged combo sort orders present combo descending");
  ASSERT_EQ(std::string("remote-zero"),
            std::string(*filteredRecords[4].remoteScoreId()),
            "tagged combo sort keeps explicit zero ahead of absence");
  ASSERT_EQ(std::string("remote-missing"),
            std::string(*filteredRecords[5].remoteScoreId()),
            "tagged combo sort keeps missing combo absent instead of zero");

  resultFilters.sort = ReplayRecordSortCriterion::Score;
  filteredRecords = replay_record_filters::apply(resultRecords, resultFilters);
  ASSERT_EQ(-1, *filteredRecords[0].localReplayId(),
            "tagged score sort keeps Auto Play first");
  ASSERT_EQ(71, *filteredRecords[1].localReplayId(),
            "tagged score sort orders score descending");

  return 0;
}
