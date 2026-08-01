#include "../src/ReplaySummaryFormatting.h"
#include "../src/ReplayResultStateBuilder.h"
#include "../src/ReplayAutoPlay.h"
#include "../src/scene/play/Pacemaker.h"

#include <cmath>
#include <iostream>

#define ASSERT_CONTAINS(haystack, needle, label)                               \
  if ((haystack).find(needle) == std::string::npos) {                          \
    std::cerr << label << " expected to contain " << (needle) << " in "        \
              << (haystack) << std::endl;                                      \
    return 1;                                                                  \
  }

#define ASSERT_NOT_CONTAINS(haystack, needle, label)                           \
  if ((haystack).find(needle) != std::string::npos) {                          \
    std::cerr << label << " expected not to contain " << (needle) << " in "    \
              << (haystack) << std::endl;                                      \
    return 1;                                                                  \
  }

namespace {
bms_parser::ChartMeta makeSevenKeyMeta() {
  bms_parser::ChartMeta meta;
  meta.KeyMode = 7;
  meta.IsDP = false;
  return meta;
}

void addClassicLongNote(bms_parser::Chart &chart, long long headMicros,
                        long long tailMicros, int lane) {
  auto *measure = new bms_parser::Measure();
  auto *headTimeline = new bms_parser::TimeLine(8, false);
  auto *tailTimeline = new bms_parser::TimeLine(8, false);
  headTimeline->Timing = headMicros;
  tailTimeline->Timing = tailMicros;
  auto *head = new bms_parser::LongNote(
      bms_parser::Parser::NoWav, bms_parser::LongNoteType::LongNote);
  auto *tail = new bms_parser::LongNote(
      bms_parser::Parser::NoWav, bms_parser::LongNoteType::LongNote);
  head->Tail = tail;
  tail->Head = head;
  headTimeline->SetNote(lane, head);
  tailTimeline->SetNote(lane, tail);
  measure->TimeLines.push_back(headTimeline);
  measure->TimeLines.push_back(tailTimeline);
  chart.Measures.push_back(measure);
}
} // namespace

int main() {
  ReplaySummary summary;
  summary.initialGaugeType = GaugeType::Hard;
  summary.finalGauge = 78.25f;
  summary.eventCount = 1234;
  summary.touchSampleCount = 5678;
  summary.playOption = "MIRROR";
  summary.chartMeta = makeSevenKeyMeta();

  const std::string detail = replay_summary_ui::detailLabel(summary);

  ASSERT_CONTAINS(detail, "HARD", "gauge type");
  ASSERT_CONTAINS(detail, "Gauge 78.2%", "final gauge");
  ASSERT_CONTAINS(detail, "MIRROR", "play option");
  ASSERT_CONTAINS(detail, "Lane S7654321", "lane order");
  ASSERT_NOT_CONTAINS(detail, "Events", "events count");
  ASSERT_NOT_CONTAINS(detail, "Touches", "touch count");

  ReplaySummary normalSummary;
  normalSummary.initialGaugeType = GaugeType::Normal;
  normalSummary.finalGauge = 12.0f;
  normalSummary.chartMeta = makeSevenKeyMeta();
  const std::string normalDetail =
      replay_summary_ui::detailLabel(normalSummary);
  ASSERT_NOT_CONTAINS(normalDetail, "Lane", "normal lane order");

  bms_parser::Chart chart;
  chart.Meta.TotalNotes = 1;
  chart.Meta.Total = 100.0;
  ReplayData practiceGaugeReplay;
  practiceGaugeReplay.initialGaugeType = GaugeType::Normal;
  practiceGaugeReplay.provenance.startingGaugePercent = 37;
  const RhythmState practiceGaugeState =
      replay_result::BuildResultState(chart, practiceGaugeReplay);
  if (practiceGaugeState.currentGauge != 37.0f) {
    std::cerr << "export result state must restore the recorded starting gauge"
              << std::endl;
    return 1;
  }

  ReplayData bestClearReplay;
  bestClearReplay.initialGaugeType = GaugeType::Normal;
  bestClearReplay.gaugeAutoShift = GaugeAutoShiftMode::BestClear;
  bestClearReplay.provenance.startingGaugePercent = 37;
  const RhythmState bestClearInitial =
      replay_result::BuildInitialGaugeState(chart, bestClearReplay);
  if (bestClearInitial.gaugeType != GaugeType::Hazard ||
      bestClearInitial.currentGauge != 37.0f) {
    std::cerr << "export HUD must use the effective Best Clear start state"
              << std::endl;
    return 1;
  }

  ReplayData gasHistoryReplay = bestClearReplay;
  gasHistoryReplay.events = {
      {.action = ReplayEventAction::Gauge,
       .songTimeMicros = 500,
       .judgement = Great,
       .gauge = 75.0f,
       .gaugeType = GaugeType::ExHard},
      {.action = ReplayEventAction::Gauge,
       .songTimeMicros = 1000,
       .judgement = Bad,
       .gauge = 50.0f,
       .gaugeType = GaugeType::Hard},
  };
  const RhythmState gasHistoryResult =
      replay_result::BuildResultState(chart, gasHistoryReplay);
  if (gasHistoryResult.gaugeHistoryFor(GaugeType::Hard).size() != 2 ||
      gasHistoryResult.gaugeHistoryFor(GaugeType::Hard).back() != 50.0f ||
      gasHistoryResult.gaugeHistoryFor(GaugeType::ExHard).size() != 2 ||
      gasHistoryResult.gaugeHistoryFor(GaugeType::ExHard).front() != 75.0f) {
    std::cerr << "replay result must rebuild complete per-gauge GAS histories"
              << std::endl;
    return 1;
  }

  ReplayData lr2RulesetReplay;
  lr2RulesetReplay.initialGaugeType = GaugeType::Normal;
  lr2RulesetReplay.gaugeAutoShift = GaugeAutoShiftMode::BestClear;
  lr2RulesetReplay.provenance.ruleset =
      RulesetDescriptor::For(GameplayRuleset::LR2);
  lr2RulesetReplay.events.push_back({.action = ReplayEventAction::Release,
                                     .judgement = Bad,
                                     .gauge = 40.0f,
                                     .gaugeType = GaugeType::Hard});
  const RhythmState lr2RulesetResult =
      replay_result::BuildResultState(chart, lr2RulesetReplay);
  RhythmState directlySimulatedLr2(&chart, false, GameplayRuleset::LR2);
  directlySimulatedLr2.configureGauge(GaugeType::Normal,
                                      GaugeAutoShiftMode::BestClear);
  directlySimulatedLr2.applyGaugeJudgement(Bad);
  if (lr2RulesetResult.gaugeHistoryFor(GaugeType::Easy) !=
      directlySimulatedLr2.gaugeHistoryFor(GaugeType::Easy)) {
    std::cerr << "replay result must rebuild non-active gauge histories with "
                 "the recorded LR2 ruleset"
              << std::endl;
    return 1;
  }
  if (lr2RulesetResult.gaugeRules().ruleset != GameplayRuleset::LR2) {
    std::cerr << "replay result must retain the recorded LR2 gauge ruleset"
              << std::endl;
    return 1;
  }

  bms_parser::Chart multiBadChart;
  multiBadChart.Meta.TotalNotes = 2;
  multiBadChart.Meta.Total = 100.0;
  ReplayData multiBadReplay;
  multiBadReplay.initialGaugeType = GaugeType::Normal;
  multiBadReplay.events = {
      {.action = ReplayEventAction::MultiBad,
       .lane = 1,
       .noteTimeMicros = 800'000,
       .songTimeMicros = 1'000'000,
       .judgeTimeMicros = 1'000'000,
       .judgement = Bad,
       .diffMicros = 200'000,
       .gauge = 18.0f,
       .gaugeType = GaugeType::Normal,
       .combo = 0,
       .score = 0},
      {.action = ReplayEventAction::Press,
       .lane = 2,
       .noteTimeMicros = 950'000,
       .songTimeMicros = 1'000'000,
       .judgeTimeMicros = 1'000'000,
       .judgement = Good,
       .diffMicros = 50'000,
       .gauge = 20.0f,
       .gaugeType = GaugeType::Normal,
       .combo = 1,
       .score = 0},
  };
  const RhythmState multiBadResult =
      replay_result::BuildResultState(multiBadChart, multiBadReplay);
  const auto multiBadProgression =
      pacemaker::buildReplayScoreProgression(multiBadReplay, 2);
  if (multiBadResult.judgeCount.at(Bad) != 1 ||
      multiBadResult.judgeCount.at(Good) != 1 ||
      multiBadResult.maxCombo != 1 || multiBadProgression.size() != 3) {
    std::cerr << "a replay must count every multi-BAD judgement while "
                 "retaining one physical press boundary"
              << std::endl;
    return 1;
  }

  ReplayData legacyRulesetReplay = lr2RulesetReplay;
  legacyRulesetReplay.provenance = ScoreProvenance::Legacy();
  const RhythmState legacyRulesetResult =
      replay_result::BuildResultState(chart, legacyRulesetReplay);
  if (legacyRulesetResult.gaugeRules().ruleset !=
      GameplayRuleset::Beatoraja) {
    std::cerr << "legacy replay result gauge reconstruction remains Beatoraja"
              << std::endl;
    return 1;
  }

  bms_parser::Chart terminalLongHeadChart;
  terminalLongHeadChart.Meta = makeSevenKeyMeta();
  terminalLongHeadChart.Meta.LnMode = 1;
  terminalLongHeadChart.Meta.TotalNotes = 1;
  terminalLongHeadChart.Meta.TotalLongNotes = 1;
  addClassicLongNote(terminalLongHeadChart, 1'000'000, 2'000'000, 1);
  ReplayData terminalLongHeadReplay;
  terminalLongHeadReplay.provenance.ruleset =
      RulesetDescriptor::For(GameplayRuleset::LR2);
  terminalLongHeadReplay.events.push_back({
      .action = ReplayEventAction::Press,
      .lane = 1,
      .noteTimeMicros = 1'000'000,
      .songTimeMicros = 850'000,
      .judgeTimeMicros = 850'000,
      .judgement = Bad,
      .diffMicros = -150'000,
      .gauge = 80.0f,
      .gaugeType = GaugeType::Normal,
      .combo = 0,
      .score = 0,
  });
  const RhythmState terminalLongHeadResult = replay_result::BuildResultState(
      terminalLongHeadChart, terminalLongHeadReplay);
  if (terminalLongHeadResult.judgeCount.at(Bad) != 1 ||
      terminalLongHeadResult.comboBreak != 1 ||
      terminalLongHeadResult.fastCount != 1) {
    std::cerr << "a terminal LR2 classic long-note head BAD must remain in the "
                 "reconstructed result"
              << std::endl;
    return 1;
  }

  bms_parser::Chart deferredLongHeadChart;
  deferredLongHeadChart.Meta = makeSevenKeyMeta();
  deferredLongHeadChart.Meta.LnMode = 1;
  deferredLongHeadChart.Meta.TotalNotes = 1;
  deferredLongHeadChart.Meta.TotalLongNotes = 1;
  addClassicLongNote(deferredLongHeadChart, 1'000'000, 2'000'000, 1);
  ReplayData deferredLongHeadReplay = terminalLongHeadReplay;
  deferredLongHeadReplay.events.front().judgement = Good;
  deferredLongHeadReplay.events.front().diffMicros = -50'000;
  const RhythmState deferredLongHeadResult = replay_result::BuildResultState(
      deferredLongHeadChart, deferredLongHeadReplay);
  if (deferredLongHeadResult.judgeCount.at(Good) != 0 ||
      deferredLongHeadResult.comboBreak != 0 ||
      deferredLongHeadResult.fastCount != 0) {
    std::cerr << "a classic long-note head without a terminal BAD or tail "
                 "result must remain deferred"
              << std::endl;
    return 1;
  }

  bms_parser::Chart releasedLongNoteChart;
  releasedLongNoteChart.Meta = makeSevenKeyMeta();
  releasedLongNoteChart.Meta.LnMode = 1;
  releasedLongNoteChart.Meta.TotalNotes = 1;
  releasedLongNoteChart.Meta.TotalLongNotes = 1;
  addClassicLongNote(releasedLongNoteChart, 1'000'000, 2'000'000, 1);
  ReplayData releasedLongNoteReplay = terminalLongHeadReplay;
  releasedLongNoteReplay.events.push_back({
      .action = ReplayEventAction::Release,
      .lane = 1,
      .noteTimeMicros = 2'000'000,
      .songTimeMicros = 851'000,
      .judgeTimeMicros = 851'000,
      .judgement = Bad,
      .diffMicros = -1'149'000,
      .gauge = 80.0f,
      .gaugeType = GaugeType::Normal,
      .combo = 0,
      .score = 0,
  });
  const RhythmState releasedLongNoteResult =
      replay_result::BuildResultState(releasedLongNoteChart,
                                      releasedLongNoteReplay);
  if (releasedLongNoteResult.judgeCount.at(Bad) != 1 ||
      releasedLongNoteResult.comboBreak != 1 ||
      releasedLongNoteResult.fastCount != 1) {
    std::cerr << "an LR2 classic long-note head and tail must reconstruct as "
                 "one final BAD"
              << std::endl;
    return 1;
  }

  ReplayData firstCourseStage;
  firstCourseStage.initialGaugeType = GaugeType::Hard;
  firstCourseStage.events.push_back({.action = ReplayEventAction::Gauge,
                                     .songTimeMicros = 500,
                                     .gauge = 42.0f,
                                     .gaugeType = GaugeType::Hard});
  const RhythmState firstCourseResult =
      replay_result::BuildResultState(chart, firstCourseStage);
  const GaugeStateSnapshot carriedGauge = firstCourseResult.gaugeSnapshot();
  ReplayData secondCourseStage;
  secondCourseStage.initialGaugeType = GaugeType::Hard;
  const RhythmState secondCourseInitial =
      replay_result::BuildInitialGaugeState(
          chart, secondCourseStage, GaugeProfile::Standard, &carriedGauge);
  if (secondCourseInitial.gaugeType != GaugeType::Hard ||
      secondCourseInitial.currentGauge != 42.0f) {
    std::cerr << "course export HUD must start from the carried gauge state"
              << std::endl;
    return 1;
  }

  ReplayData failedReplay;
  failedReplay.initialGaugeType = GaugeType::Hard;
  failedReplay.events.push_back({.action = ReplayEventAction::Gauge,
                                 .songTimeMicros = 500,
                                 .gauge = 0.0f,
                                 .gaugeType = GaugeType::Hard});
  const RhythmState failedResult =
      replay_result::BuildResultState(chart, failedReplay);
  const GaugeStateSnapshot failedCarry = failedResult.gaugeSnapshot();
  if (!failedCarry.gaugeSurvivalFailed[gaugeTypeIndex(GaugeType::Hard)] ||
      replay_result::FindGaugeFailureMicros(
          chart, failedReplay, GaugeProfile::Standard, &failedCarry) != 0) {
    std::cerr << "course export must carry terminal survival gauge state"
              << std::endl;
    return 1;
  }

  ReplayData continuedReplay;
  continuedReplay.initialGaugeType = GaugeType::Hard;
  continuedReplay.gaugeAutoShift = GaugeAutoShiftMode::Continue;
  continuedReplay.provenance.startingGaugePercent = 0;
  if (replay_result::FindGaugeFailureMicros(chart, continuedReplay)
          .has_value()) {
    std::cerr << "Continue export must not stop at a zero percent start"
              << std::endl;
    return 1;
  }

  ReplayData survivalOnlyReplay;
  survivalOnlyReplay.initialGaugeType = GaugeType::ExHard;
  survivalOnlyReplay.gaugeAutoShift = GaugeAutoShiftMode::SelectToUnder;
  survivalOnlyReplay.gaugeAutoShiftLowerBound = GaugeType::Hard;
  survivalOnlyReplay.provenance.startingGaugePercent = 0;
  if (replay_result::FindGaugeFailureMicros(chart, survivalOnlyReplay) != 0) {
    std::cerr << "survival-only GAS export must fail at a zero percent start"
              << std::endl;
    return 1;
  }

  ReplayData assistedReplay;
  assistedReplay.initialGaugeType = GaugeType::Hard;
  assistedReplay.maxCombo = 1;
  assistedReplay.finalGauge = 100.0f;
  assistedReplay.clearType = kClearTypeFullComboRank;
  assistedReplay.provenance.playback = {
      .percent = 75, .mode = audio::PlaybackMode::PitchShift};
  assistedReplay.provenance.eligibility = ScoreEligibility::Modified;
  assistedReplay.events.push_back({.action = ReplayEventAction::Release,
                                   .judgement = PGreat,
                                   .gauge = 100.0f,
                                   .gaugeType = GaugeType::Hard,
                                   .combo = 1,
                                   .score = 2});
  const RhythmState assistedState =
      replay_result::BuildResultState(chart, assistedReplay);
  if (assistedState.getClearTypeRank() != kClearTypeAssistedEasyClearRank) {
    std::cerr << "export result state must retain playback assist cap"
              << std::endl;
    return 1;
  }

  ReplayData neutralReplay = assistedReplay;
  neutralReplay.provenance = ScoreProvenance::Legacy();
  const RhythmState neutralState =
      replay_result::BuildResultState(chart, neutralReplay);
  if (neutralState.getClearTypeRank() != kClearTypeHardClearRank) {
    std::cerr << "legacy neutral export result state remains unassisted"
              << std::endl;
    return 1;
  }

  const ReplayData autoExport = replay_autoplay::BuildReplayData(
      chart, GaugeType::Normal, GaugeAutoShiftMode::None, {.percent = 200},
      std::nullopt, std::nullopt, std::nullopt, std::nullopt,
      assist_options::kOff, true, GaugeType::Hard);
  if (autoExport.provenance.playback.percent != 200) {
    std::cerr << "synthetic Auto export retains the selected playback rate"
              << std::endl;
    return 1;
  }
  if (!autoExport.provenance.clubMode) {
    std::cerr << "synthetic Auto export retains Club Beat" << std::endl;
    return 1;
  }
  if (autoExport.gaugeAutoShiftLowerBound != GaugeType::Hard) {
    std::cerr << "synthetic Auto export retains the GAS lower bound"
              << std::endl;
    return 1;
  }
  if (autoExport.provenance.ruleset !=
      RulesetDescriptor::For(GameplayRuleset::LR2)) {
    std::cerr << "synthetic Auto export defaults to LR2 provenance"
              << std::endl;
    return 1;
  }
  const ReplayData beatorajaAutoExport = replay_autoplay::BuildReplayData(
      chart, GaugeType::Normal, GaugeAutoShiftMode::None, {}, std::nullopt,
      std::nullopt, std::nullopt, std::nullopt, assist_options::kOff, false,
      GaugeType::AssistedEasy, GameplayRuleset::Beatoraja);
  if (beatorajaAutoExport.provenance.ruleset !=
      RulesetDescriptor::For(GameplayRuleset::Beatoraja)) {
    std::cerr << "synthetic Auto export retains a selected Beatoraja ruleset"
              << std::endl;
    return 1;
  }

  const ReplaySummary assistedAutoSummary = replay_autoplay::BuildSummary(
      chart.Meta, GaugeType::Normal, GaugeAutoShiftMode::None, std::nullopt,
      std::nullopt, std::nullopt, std::nullopt, assist_options::kOff,
      {.percent = 200});
  if (assistedAutoSummary.playback.percent != 200 ||
      assistedAutoSummary.clearType != kClearTypeAssistedEasyClearRank) {
    std::cerr << "synthetic Auto summary must retain playback assist limits"
              << std::endl;
    return 1;
  }

  bms_parser::ChartMeta rulesetMeta;
  rulesetMeta.KeyMode = 7;
  rulesetMeta.TotalNotes = 100;
  rulesetMeta.HasTotal = true;
  rulesetMeta.Total = 100.5;
  const ReplaySummary lr2AutoSummary = replay_autoplay::BuildSummary(
      rulesetMeta, GaugeType::Normal, GaugeAutoShiftMode::None);
  bms_parser::Chart rulesetChart;
  rulesetChart.Meta = rulesetMeta;
  RhythmState expectedLr2Auto(&rulesetChart, false, GameplayRuleset::LR2);
  expectedLr2Auto.configureGauge(GaugeType::Normal,
                                 GaugeAutoShiftMode::None);
  for (int note = 0; note < rulesetMeta.TotalNotes; ++note) {
    expectedLr2Auto.applyGaugeJudgement(PGreat);
  }
  if (std::abs(lr2AutoSummary.finalGauge -
               expectedLr2Auto.currentGauge) > 0.01f) {
    std::cerr << "synthetic Auto summary defaults to the LR2 gauge ruleset"
              << std::endl;
    return 1;
  }

  bms_parser::ChartMeta pmsMeta;
  pmsMeta.KeyMode = 9;
  pmsMeta.TotalNotes = 100;
  pmsMeta.HasTotal = true;
  pmsMeta.Total = 100.0;
  const auto buildBeatorajaSummary = [](const bms_parser::ChartMeta &meta) {
    return replay_autoplay::BuildSummary(
        meta, GaugeType::Normal, GaugeAutoShiftMode::None, std::nullopt,
        std::nullopt, std::nullopt, std::nullopt, assist_options::kOff, {},
        GameplayRuleset::Beatoraja);
  };
  const ReplaySummary pmsAutoSummary = buildBeatorajaSummary(pmsMeta);
  if (pmsAutoSummary.finalGauge != 120.0f) {
    std::cerr << "synthetic Auto summary must use the PMS gauge result"
              << std::endl;
    return 1;
  }

  pmsMeta.Total = 10.0;
  const ReplaySummary lowTotalAutoSummary = buildBeatorajaSummary(pmsMeta);
  if (std::abs(lowTotalAutoSummary.finalGauge - 40.0f) > 0.01f) {
    std::cerr << "synthetic Auto summary must respect authored TOTAL"
              << std::endl;
    return 1;
  }

  return 0;
}
