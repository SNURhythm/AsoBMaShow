#include "../src/ResultRecallBuilder.h"
#include "../src/ResultPersistenceModel.h"
#include "../src/replay/LegacyReplayIdentity.h"

#include <atomic>
#include <cassert>
#include <memory>
#include <string>

namespace {

constexpr const char *kAttemptId =
    "123e4567-e89b-42d3-a456-426614174000";

ScoreProvenance verifiedProvenance(GaugeType gauge = GaugeType::Hard) {
  ScoreProvenance provenance = ScoreProvenance::Legacy();
  provenance.schemaVersion = ScoreProvenance::kSchemaVersion;
  provenance.ruleset = RulesetDescriptor::For(GameplayRuleset::LR2);
  provenance.gaugeType = gauge;
  provenance.eligibility = ScoreEligibility::Verified;
  return provenance;
}

result_persistence::PersistedChartResult validResult(
    int resultId = 41, std::string path = "charts/recall.bms") {
  result_persistence::PersistedChartResult result{
      .resultId = resultId,
      .attemptId = kAttemptId,
      .score =
          {
              .chartPath = std::move(path),
              .chartMd5 = "0123456789abcdef0123456789abcdef",
              .chartSha256 =
                  "0123456789abcdef0123456789abcdef"
                  "0123456789abcdef0123456789abcdef",
              .chartTitle = "Recall Title",
              .chartArtist = "Recall Artist",
              .longNoteMode = 2,
              .score = 3,
              .maxScore = 4,
              .maxCombo = 2,
              .comboBreak = 0,
              .pGreat = 1,
              .great = 1,
              .fast = 1,
              .finalGauge = 93.25F,
              .clearType = kClearTypeFullComboRank,
              .provenance = verifiedProvenance(),
          },
      .keyMode = 7,
      .adoptedGaugeHistory = {20.0F, 61.5F, 93.25F},
      .playedAtUnixMillis = 1784420645000LL,
  };
  result.judgementTiming.emplace();
  result.judgementTiming->byJudgement[PGreat] = {.fast = 1, .slow = 0};
  result.resultFingerprint = result_persistence::resultFingerprint(result);
  return result;
}

result_recall::ResultChartLoader chartLoader(int *calls = nullptr) {
  return [calls](const result_persistence::PersistedChartResult &result,
                 std::atomic_bool &) {
    if (calls != nullptr) {
      ++*calls;
    }
    auto chart = std::make_unique<bms_parser::Chart>();
    chart->Meta.BmsPath = result.score.chartPath;
    chart->Meta.MD5 = result.score.chartMd5;
    chart->Meta.SHA256 = result.score.chartSha256;
    chart->Meta.Title = "Changed on disk";
    chart->Meta.Artist = "Changed on disk";
    chart->Meta.KeyMode = 14;
    chart->Meta.TotalNotes = 999;
    chart->Meta.Banner = "banner.png";
    chart->Meta.StageFile = "stage.png";
    return chart;
  };
}

void assertStateMatches(const RhythmState &state,
                        const result_persistence::ChartScoreWrite &score,
                        const std::vector<float> &gaugeHistory,
                        const result_persistence::ChartJudgementTiming &timing) {
  assert(state.getScore() == score.score);
  assert(state.maxCombo == score.maxCombo);
  assert(state.comboBreak == score.comboBreak);
  assert(state.judgeCount.at(PGreat) == score.pGreat);
  assert(state.judgeCount.at(Great) == score.great);
  assert(state.judgeCount.at(Good) == score.good);
  assert(state.judgeCount.at(Bad) == score.bad);
  assert(state.judgeCount.at(Poor) == score.poor);
  assert(state.judgeCount.at(Kpoor) == score.kPoor);
  assert(state.fastCount == score.fast);
  assert(state.slowCount == score.slow);
  assert(state.currentGauge == score.finalGauge);
  assert(state.gaugeHistory == gaugeHistory);
  assert(state.judgementFastSlowCount.at(PGreat) ==
         timing.byJudgement[PGreat]);
  assert(state.getClearTypeRank() == score.clearType);
}

void testChartRecallUsesPersistedFactsOnly() {
  auto persisted = validResult();
  const auto expected = persisted;
  int calls = 0;
  std::atomic_bool cancelled = false;
  auto outcome = result_recall::BuildChartResult(
      std::move(persisted), cancelled, chartLoader(&calls));

  assert(outcome.value.has_value());
  assert(outcome.diagnostic.empty());
  assert(calls == 1);
  const auto &recalled = *outcome.value;
  assert(recalled.result == expected);
  assert(recalled.chart != nullptr);
  assert(recalled.chart->Meta.Banner == "banner.png");
  assert(recalled.chart->Meta.StageFile == "stage.png");
  assert(recalled.chart->Meta.BmsPath == expected.score.chartPath);
  assert(recalled.chart->Meta.Title == expected.score.chartTitle);
  assert(recalled.chart->Meta.Artist == expected.score.chartArtist);
  assert(recalled.chart->Meta.MD5 == expected.score.chartMd5);
  assert(recalled.chart->Meta.SHA256 == expected.score.chartSha256);
  assert(recalled.chart->Meta.KeyMode == expected.keyMode);
  assert(recalled.chart->Meta.TotalNotes == expected.score.maxScore / 2);
  assert(recalled.chart->Meta.LnMode == expected.score.longNoteMode);
  assertStateMatches(recalled.state, expected.score,
                     expected.adoptedGaugeHistory,
                     *expected.judgementTiming);
  assert(recalled.result.playedAtUnixMillis == 1784420645000LL);
  assert(recalled.result.score.provenance == expected.score.provenance);
}

void testChartRecallRejectsInvalidResultBeforeLoadingAssets() {
  auto persisted = validResult();
  persisted.score.score = 1;
  int calls = 0;
  std::atomic_bool cancelled = false;
  auto outcome = result_recall::BuildChartResult(
      std::move(persisted), cancelled, chartLoader(&calls));
  assert(!outcome.value.has_value());
  assert(outcome.diagnostic ==
         "saved chart result is invalid: score range is inconsistent with "
         "result counters");
  assert(calls == 0);
}

void testChartRecallDoesNotPublishMissingOrCancelledAssets() {
  std::atomic_bool cancelled = false;
  auto missing = result_recall::BuildChartResult(
      validResult(), cancelled,
      [](const result_persistence::PersistedChartResult &,
         std::atomic_bool &) { return std::unique_ptr<bms_parser::Chart>{}; });
  assert(!missing.value.has_value());
  assert(missing.diagnostic == "saved chart is unavailable");

  cancelled = true;
  auto stopped = result_recall::BuildChartResult(validResult(), cancelled,
                                                  chartLoader());
  assert(!stopped.value.has_value());
  assert(stopped.diagnostic == "saved chart is unavailable");
}

void testChartRecallRejectsChangedChartIdentity() {
  std::atomic_bool cancelled = false;
  auto outcome = result_recall::BuildChartResult(
      validResult(), cancelled,
      [](const result_persistence::PersistedChartResult &result,
         std::atomic_bool &) {
        auto chart = std::make_unique<bms_parser::Chart>();
        chart->Meta.BmsPath = result.score.chartPath;
        chart->Meta.MD5 = std::string(32, 'd');
        chart->Meta.SHA256 = std::string(64, 'e');
        return chart;
      });

  assert(!outcome.value.has_value());
  assert(outcome.diagnostic ==
         "saved chart no longer matches its stored identity");
}

void testChartRecallAcceptsMatchingMd5OnlyMigrationIdentity() {
  auto persisted = validResult();
  const std::string realSha = std::string(64, 'e');
  persisted.score.chartSha256 =
      *replay::legacyReplaySha256ForMd5(persisted.score.chartMd5);
  persisted.resultFingerprint =
      result_persistence::resultFingerprint(persisted);
  std::atomic_bool cancelled = false;
  auto outcome = result_recall::BuildChartResult(
      persisted, cancelled,
      [realSha](const result_persistence::PersistedChartResult &result,
                std::atomic_bool &) {
        auto chart = std::make_unique<bms_parser::Chart>();
        chart->Meta.BmsPath = result.score.chartPath;
        chart->Meta.MD5 = result.score.chartMd5;
        chart->Meta.SHA256 = realSha;
        return chart;
      });

  assert(outcome.value.has_value());
  assert(outcome.value->chart->Meta.SHA256 == persisted.score.chartSha256);
}

result_persistence::PersistedCourseResult validCourseResult() {
  auto first = validResult(101, "charts/stage-1.bms");
  auto second = validResult(102, "charts/stage-2.bms");
  second.score.chartTitle = "Stage Two";
  second.score.score = 2;
  second.score.pGreat = 1;
  second.score.great = 0;
  second.score.maxCombo = 1;
  second.score.comboBreak = 1;
  second.score.fast = 0;
  second.score.finalGauge = 62.5F;
  second.score.clearType = kClearTypeNormalClearRank;
  second.adoptedGaugeHistory = {93.25F, 62.5F};
  second.judgementTiming->byJudgement[PGreat] = {};

  result_persistence::PersistedCourseResult result{
      .resultId = 9,
      .attemptId = kAttemptId,
      .courseKey = "course:v1:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
      .legacyCourseId = 7,
      .courseName = "Recall Course",
      .courseGroupName = "Records",
      .constraintJson = "{}",
      .completedCharts = 2,
      .totalCharts = 2,
      .requestedPlayOption = "NORMAL",
      .assistOption = "OFF",
      .initialGaugeType = GaugeType::Normal,
      .gaugeProfile = GaugeProfile::Course7Keys,
      .gaugeAutoShift = GaugeAutoShiftMode::None,
      .gaugeAutoShiftLowerBound = GaugeType::AssistedEasy,
      .longNoteMode = 2,
      .finalScore = first.score.score + second.score.score,
      .maxScore = first.score.maxScore + second.score.maxScore,
      .maxCombo = 2,
      .finalGauge = second.score.finalGauge,
      .clearType = second.score.clearType,
      .provenance = verifiedProvenance(GaugeType::Normal),
      .stages = {
          {.stageIndex = 0,
           .score = first.score,
           .keyMode = first.keyMode,
           .adoptedGaugeHistory = first.adoptedGaugeHistory,
           .judgementTiming = first.judgementTiming},
          {.stageIndex = 1,
           .score = second.score,
           .keyMode = second.keyMode,
           .adoptedGaugeHistory = second.adoptedGaugeHistory,
           .judgementTiming = second.judgementTiming},
      },
      .playedAtUnixMillis = 1784420645000LL,
  };
  result.resultFingerprint = result_persistence::resultFingerprint(result);
  return result;
}

void testCourseRecallUsesOrderedPersistedStageFacts() {
  auto persisted = validCourseResult();
  const auto expected = persisted;
  int calls = 0;
  std::atomic_bool cancelled = false;
  auto outcome = result_recall::BuildCourseResult(
      std::move(persisted), cancelled, chartLoader(&calls));

  assert(outcome.value.has_value());
  assert(calls == 2);
  assert(outcome.value->result == expected);
  const auto &session = outcome.value->session;
  assert(session != nullptr);
  assert(session->courseId == expected.legacyCourseId);
  assert(session->courseKey == expected.courseKey);
  assert(session->courseName == expected.courseName);
  assert(session->entries.size() == 2);
  assert(session->completedResults.size() == 2);
  assert(session->ownedResultBrowseCharts.size() == 2);
  assert(session->courseReplayData == nullptr);
  assert(session->stageProvenance.at(0) ==
         std::optional(expected.stages[0].score.provenance));
  assertStateMatches(session->completedResults[0].state,
                     expected.stages[0].score,
                     expected.stages[0].adoptedGaugeHistory,
                     *expected.stages[0].judgementTiming);
  assertStateMatches(session->completedResults[1].state,
                     expected.stages[1].score,
                     expected.stages[1].adoptedGaugeHistory,
                     *expected.stages[1].judgementTiming);
}

void testCourseRecallDoesNotPublishPartialSession() {
  auto persisted = validCourseResult();
  int calls = 0;
  result_recall::ResultChartLoader failingLoader =
      [&calls](const result_persistence::PersistedChartResult &stage,
               std::atomic_bool &) {
        ++calls;
        if (calls == 2) {
          return std::unique_ptr<bms_parser::Chart>{};
        }
        auto chart = std::make_unique<bms_parser::Chart>();
        chart->Meta.BmsPath = stage.score.chartPath;
        chart->Meta.MD5 = stage.score.chartMd5;
        chart->Meta.SHA256 = stage.score.chartSha256;
        return chart;
      };
  std::atomic_bool cancelled = false;
  auto outcome = result_recall::BuildCourseResult(
      std::move(persisted), cancelled, std::move(failingLoader));
  assert(!outcome.value.has_value());
  assert(outcome.diagnostic == "saved course stage is unavailable");
  assert(calls == 2);
}

void testCourseRecallRejectsChangedStageIdentity() {
  auto persisted = validCourseResult();
  int calls = 0;
  result_recall::ResultChartLoader changedStageLoader =
      [&calls](const result_persistence::PersistedChartResult &stage,
               std::atomic_bool &) {
        ++calls;
        auto chart = std::make_unique<bms_parser::Chart>();
        chart->Meta.BmsPath = stage.score.chartPath;
        chart->Meta.MD5 = stage.score.chartMd5;
        chart->Meta.SHA256 =
            calls == 2 ? std::string(64, 'e') : stage.score.chartSha256;
        return chart;
      };
  std::atomic_bool cancelled = false;
  auto outcome = result_recall::BuildCourseResult(
      std::move(persisted), cancelled, std::move(changedStageLoader));

  assert(!outcome.value.has_value());
  assert(outcome.diagnostic ==
         "saved course stage no longer matches its stored identity");
  assert(calls == 2);
}

} // namespace

int main() {
  testChartRecallUsesPersistedFactsOnly();
  testChartRecallRejectsInvalidResultBeforeLoadingAssets();
  testChartRecallDoesNotPublishMissingOrCancelledAssets();
  testChartRecallRejectsChangedChartIdentity();
  testChartRecallAcceptsMatchingMd5OnlyMigrationIdentity();
  testCourseRecallUsesOrderedPersistedStageFacts();
  testCourseRecallDoesNotPublishPartialSession();
  testCourseRecallRejectsChangedStageIdentity();
  return 0;
}
