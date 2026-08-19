#include "AssistOptionUtils.h"
#include "scene/play/GameplayScoreState.h"
#include "scene/play/RhythmState.h"

#include <bit>
#include <cstdint>
#include <cstdlib>
#include <iostream>

namespace {
void require(bool value, const char *message) {
  if (!value) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

GameplayScoreConfig beatorajaScoreConfig(int totalNotes, int keyMode,
                                         double total) {
  bms_parser::ChartMeta meta;
  meta.TotalNotes = totalNotes;
  meta.KeyMode = keyMode;
  meta.HasTotal = true;
  meta.Total = total;
  return {.gaugeRules = compileGameplayGaugeRules(
              GameplayRuleset::Beatoraja, meta, GaugeProfile::Standard),
          .keyMode = keyMode};
}

void requireSame(const GameplayScoreState &left,
                 const GameplayScoreState &right) {
  require(left.judgeCount == right.judgeCount, "judge counts match");
  require(left.judgementFastSlowCount == right.judgementFastSlowCount,
          "fast/slow counts match");
  require(left.combo == right.combo && left.maxCombo == right.maxCombo &&
              left.comboBreak == right.comboBreak &&
              left.stageCombo == right.stageCombo &&
              left.stagePassedNotes == right.stagePassedNotes,
          "combo state matches");
  require(left.gaugeType == right.gaugeType &&
              left.gaugeValues == right.gaugeValues &&
              left.gaugeHistories == right.gaugeHistories &&
              left.gaugeSurvivalFailed == right.gaugeSurvivalFailed &&
              std::bit_cast<std::uint32_t>(left.currentGauge) ==
                  std::bit_cast<std::uint32_t>(right.currentGauge),
          "gauge state is bit-identical");
}

void testConfiguredGaugeHistoryUsesLogicalLimit() {
  GameplayScoreState state(beatorajaScoreConfig(10, 7, 100.0));
  state.gaugeHistory.reserve(8);
  state.configureBoundedGaugeHistory(2);
  const auto *const storage = state.gaugeHistory.data();
  const auto allocatedCapacity = state.gaugeHistory.capacity();
  require(allocatedCapacity >= 8,
          "fixture has allocator capacity above the logical limit");

  state.applyGaugeDelta(1.0F);
  state.applyGaugeDelta(1.0F);
  require(state.gaugeHistory.size() == 2 &&
              !state.gaugeHistoryOverflowed(),
          "configured history accepts exactly its logical limit");

  state.applyGaugeDelta(1.0F);
  require(state.gaugeHistory.size() == 2 &&
              state.gaugeHistoryOverflowed() &&
              state.gaugeHistory.data() == storage &&
              state.gaugeHistory.capacity() == allocatedCapacity,
          "logical history overflow latches without storage growth");
}

void testRhythmStateGaugeHistoryRemainsUnboundedByDefault() {
  bms_parser::Chart chart;
  RhythmState state(&chart, false);
  state.gaugeHistory.reserve(1);
  state.applyGaugeDelta(1.0F);
  state.applyGaugeDelta(1.0F);
  state.applyGaugeDelta(1.0F);
  require(state.gaugeHistory.size() == 3 &&
              !state.gaugeHistoryOverflowed(),
          "default RhythmState compatibility keeps growable gauge history");
}

void testGasRecordsEveryTrackedGaugeHistory() {
  GameplayScoreState state(beatorajaScoreConfig(100, 7, 200.0));
  state.configureGauge(GaugeType::Hard, GaugeAutoShiftMode::BestClear);
  state.applyGaugeJudgement(Great);
  state.applyGaugeJudgement(Bad);

  require(state.gaugeHistory.size() == 2,
          "GAS retains the compatibility active-gauge history");
  for (int index = 0; index < static_cast<int>(kGaugeTypeCount); ++index) {
    const GaugeType type = gaugeTypeAtIndex(index);
    const auto &history = state.gaugeHistoryFor(type);
    require(history.size() == 2,
            "GAS records every tracked gauge for each mutation");
    require(std::bit_cast<std::uint32_t>(history.back()) ==
                std::bit_cast<std::uint32_t>(state.gaugeValues[index]),
            "per-gauge history retains the post-mutation value");
  }
}

void testBoundedGasHistoriesShareLogicalLimit() {
  GameplayScoreState state(beatorajaScoreConfig(100, 7, 200.0));
  state.configureBoundedGaugeHistory(1);
  state.configureGauge(GaugeType::Hard, GaugeAutoShiftMode::BestClear);
  state.applyGaugeDelta(-1.0F);
  state.applyGaugeDelta(-1.0F);

  require(state.gaugeHistory.size() == 1 &&
              state.gaugeHistoryOverflowed(),
          "GAS latches the existing logical history limit");
  for (int index = 0; index < static_cast<int>(kGaugeTypeCount); ++index) {
    require(state.gaugeHistoryFor(gaugeTypeAtIndex(index)).size() == 1,
            "bounded GAS series never grow past the logical limit");
  }
}

void testNonGasRecordsOnlyActiveGaugeHistory() {
  GameplayScoreState state(beatorajaScoreConfig(100, 7, 200.0));
  state.configureGauge(GaugeType::Hard, GaugeAutoShiftMode::None);
  state.applyGaugeDelta(-1.0F);

  for (int index = 0; index < static_cast<int>(kGaugeTypeCount); ++index) {
    const GaugeType type = gaugeTypeAtIndex(index);
    require(state.gaugeHistoryFor(type).size() ==
                (type == GaugeType::Hard ? 1U : 0U),
            "non-GAS records only the active gauge series");
  }
}

void testStageComboRemainsLocalWhenCourseComboCarriesAcrossCharts() {
  GameplayScoreState state(beatorajaScoreConfig(100, 7, 200.0));
  // Gameplay's visible combo intentionally carries across course charts.  A
  // Beatoraja play skin's full-combo timer instead reads JudgeManager's
  // per-chart combo, which starts fresh for the next chart.
  state.combo = 37;
  state.commitJudge(JudgeResult(PGreat, 0));
  require(state.combo == 38 && state.stageCombo == 1,
          "stage combo stays independent from the carried course combo");

  state.commitJudge(JudgeResult(Poor, 0));
  require(state.combo == 0 && state.stageCombo == 0,
          "a combo break resets both the displayed and stage-local combos");
}

void testAssistedEasyGaugeUsesOrdinaryAssistClear() {
  require(clearTypeForGauge(GaugeType::AssistedEasy, 100.0F, false) ==
              ClearType::AssistedEasyClear,
          "legacy Assist Easy gauge helper uses the ordinary Assist Easy lamp");

  GameplayScoreState state(beatorajaScoreConfig(100, 7, 200.0));
  state.configureGauge(GaugeType::AssistedEasy, GaugeAutoShiftMode::None);
  state.applyGaugeDelta(100.0F);

  require(state.getClearType() == ClearType::AssistedEasyClear &&
              state.getClearTypeRank() == kClearTypeAssistedEasyClearRank &&
              std::string_view(state.getClearTypeLabel()) ==
                  "ASSIST EASY CLEAR",
          "Assisted Easy gauge uses the ordinary Assist Easy lamp");

  state.setAssistClearMark(AssistClearMark::LightAssistedEasy);
  require(state.getClearType() == ClearType::LightAssistedEasyClear,
          "explicit light-assist modifiers retain the Light Assist Easy lamp");
}

void testBpmGuideUsesLightAssistOnlyForVariableTempoCharts() {
  GameplayScoreState state(beatorajaScoreConfig(100, 7, 200.0));
  state.configureGauge(GaugeType::Normal, GaugeAutoShiftMode::None);
  state.applyGaugeDelta(100.0F);

  state.setAssistClearMark(clear_policy::assistClearMarkRequired(
      assist_options::kBpmGuide, 120.0, 180.0, {}));
  require(state.getClearType() == ClearType::LightAssistedEasyClear,
          "BPM Guide on a variable-tempo chart produces Light Assist Easy");

  state.setAssistClearMark(clear_policy::assistClearMarkRequired(
      assist_options::kBpmGuide, 120.0, 120.0, {}));
  require(state.getClearType() == ClearType::NormalClear,
          "BPM Guide on a constant-tempo chart leaves the clear type intact");

  state.setAssistClearMark(clear_policy::assistClearMarkRequired(
      assist_options::kDrag, 120.0, 120.0, {}));
  require(state.getClearType() == ClearType::LightAssistedEasyClear,
          "Drag assist follows BPM Guide's Light Assist Easy result class");
  require(clear_policy::fullComboRankForPlayback(
              state.getClearTypeRank(), true, audio::PlaybackRate{}) ==
              kClearTypeLightAssistedEasyClearRank,
          "full combo does not replace a light-assist lamp");
  require(clear_policy::fullComboRankForPlayback(
              kClearTypeAssistedEasyClearRank, true, audio::PlaybackRate{}) ==
              kClearTypeFullComboRank,
          "an Assisted Easy gauge alone still permits a full-combo lamp");
}

void testAlteredPlaybackUsesLightAssistEasy() {
  const audio::PlaybackRate alteredPlayback{
      .percent = 75, .mode = audio::PlaybackMode::PitchShift};
  GameplayScoreState state(beatorajaScoreConfig(100, 7, 200.0));
  state.configureGauge(GaugeType::Normal, GaugeAutoShiftMode::None);
  state.applyGaugeDelta(100.0F);
  state.setAssistClearMark(clear_policy::assistClearMarkRequired(
      assist_options::kOff, 120.0, 120.0, alteredPlayback));

  require(state.getClearType() == ClearType::LightAssistedEasyClear,
          "altered playback produces Light Assist Easy without a Beatoraja "
          "Assist Easy equivalent");
  require(clear_policy::capRankForPlayback(kClearTypeHardClearRank,
                                           alteredPlayback) ==
              kClearTypeLightAssistedEasyClearRank,
          "altered playback caps persisted clears at Light Assist Easy");
}
} // namespace

int main() {
  testConfiguredGaugeHistoryUsesLogicalLimit();
  testRhythmStateGaugeHistoryRemainsUnboundedByDefault();
  testGasRecordsEveryTrackedGaugeHistory();
  testBoundedGasHistoriesShareLogicalLimit();
  testNonGasRecordsOnlyActiveGaugeHistory();
  testStageComboRemainsLocalWhenCourseComboCarriesAcrossCharts();
  testAssistedEasyGaugeUsesOrdinaryAssistClear();
  testBpmGuideUsesLightAssistOnlyForVariableTempoCharts();
  testAlteredPlaybackUsesLightAssistEasy();

  bms_parser::Chart chart;
  chart.Meta.TotalNotes = 432;
  chart.Meta.KeyMode = 7;
  chart.Meta.HasTotal = true;
  chart.Meta.Total = 280.0;

  RhythmState legacy(&chart, false);
  GameplayScoreState runtime(beatorajaScoreConfig(432, 7, 280.0));
  legacy.configureGauge(GaugeType::Hard, GaugeAutoShiftMode::BestClear);
  runtime.configureGauge(GaugeType::Hard, GaugeAutoShiftMode::BestClear);
  legacy.configureBoundedGaugeHistory(64);
  runtime.configureBoundedGaugeHistory(64);

  for (const auto judge : {PGreat, Great, Good, Bad, Poor, Kpoor}) {
    legacy.commitJudge(JudgeResult(judge, judge == PGreat ? -10 : 10));
    runtime.commitJudge(JudgeResult(judge, judge == PGreat ? -10 : 10));
  }
  legacy.applyGaugeJudgementRate(Great, 0.5F);
  runtime.applyGaugeJudgementRate(Great, 0.5F);
  legacy.applyGaugeDelta(-3.25F);
  runtime.applyGaugeDelta(-3.25F);
  requireSame(legacy, runtime);
  require(legacy.getScore() == runtime.getScore(), "EX score matches");
  require(legacy.getClearTypeRank() == runtime.getClearTypeRank(),
          "clear state matches");
  return 0;
}
