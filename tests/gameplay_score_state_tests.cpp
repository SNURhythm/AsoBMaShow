#include "AssistOptionUtils.h"
#include "scene/play/GameplayScoreState.h"
#include "scene/play/RhythmState.h"
#include "scene/play/QuickRetry.h"

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
  state.setAssistClearMark(clear_policy::assistClearMarkRequired(
      "ASSISTED", 120.0, 120.0, {}));
  require(state.getClearType() == ClearType::LightAssistedEasyClear &&
              clear_policy::fullComboRankForPlayback(
                  state.getClearTypeRank(), true, {}) ==
                  kClearTypeLightAssistedEasyClearRank,
          "generic assisted attempts keep the light-assist cap even at full combo");
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

void testPinnedCountsAndComboByMode() {
  for (int mode : {5, 7, 9, 10, 14, 24, 48}) {
    GameplayScoreState state(beatorajaScoreConfig(100, mode, 200.0));
    state.commitJudge(JudgeResult(PGreat, 0));
    state.commitJudge(JudgeResult(Kpoor, -100000));
    require(state.stagePassedNotes == 1, "empty POOR does not consume a chart note");
    require(state.combo == (mode == 5 || mode == 9 || mode == 10 ? 0 : 1),
            "five-key and PMS empty POOR break combo");
    require(state.judgementFastSlowCount.at(PGreat).fast == 1 &&
                state.judgementFastSlowCount.at(Kpoor).fast == 1,
            "zero timing and empty POOR are included in per-judge fast counts");
    require(state.fastCount == 1 && state.slowCount == 0 && state.comboBreak == 0,
            "display totals exclude PGREAT and combo-break count excludes empty POOR");
    state.commitJudge(JudgeResult(Bad, -100000));
    require(state.stagePassedNotes == (mode == 9 ? 1 : 2),
            "PMS BAD does not consume a chart note");
    state.commitJudge(JudgeResult(Bad, 10000), true);
    require(state.stagePassedNotes == (mode == 9 ? 2 : 3) &&
                state.judgementFastSlowCount.at(Bad).slow == 1,
            "explicit vanishing LN BAD consumes a note and records slow timing");
  }
}

void testCourseSurvivalToGrooveContinuesAfterGaugeDeath() {
  bms_parser::ChartMeta meta;
  meta.TotalNotes = 100;
  meta.Total = 240.0;
  meta.HasTotal = true;
  GameplayScoreState state({
      .gaugeRules = compileGameplayGaugeRules(
          GameplayRuleset::Beatoraja, meta, GaugeProfile::Course7Keys),
      .keyMode = 7});
  state.configureGauge(GaugeType::Hard, GaugeAutoShiftMode::SurvivalToGroove,
                       GaugeProfile::Course7Keys);
  state.applyGaugeDelta(-100.0F);
  require(!state.activeGaugeFailed() && state.gaugeType == GaugeType::Hard,
          "course Survival to Groove stays on the dead course gauge without terminating");
  state.commitJudge(JudgeResult(PGreat, 0));
  require(state.getScore() == 2 && state.currentGauge == 0.0F &&
              state.getClearType() == ClearType::Failed,
          "continued course play accumulates score while the dead gauge stays failed");
}

void testBestClearFallsBackToBottomGaugeWhenNoneQualifies() {
  GameplayScoreState state(beatorajaScoreConfig(1000, 7, 240.0));
  state.configureGauge(GaugeType::Hard, GaugeAutoShiftMode::BestClear);
  state.applyGaugeDelta(-100.0F);
  require(state.gaugeType == GaugeType::AssistedEasy,
          "BEST CLEAR falls back to bottom admitted gauge if none qualifies");
  for (const auto shift : {GaugeAutoShiftMode::BestClear,
                           GaugeAutoShiftMode::SelectToUnder}) {
    state.configureGauge(GaugeType::ExHard, shift, GaugeProfile::Standard,
                         GaugeType::Hard);
    state.applyGaugeDelta(-100.0F);
    require(!state.activeGaugeFailed() && state.getClearType() == ClearType::Failed,
            "GAS continues even when every admitted survival gauge is dead");
  }
}
} // namespace

void testQuickRetryDeferredBadGaugeFloor() {
  struct Case { float gauge; int remaining; int bads; int hcnTicks; bool clears; };
  for (const auto &test : std::array{
           Case{2, 80, 1, 0, true}, Case{2, 78, 1, 0, false},
           Case{20, 63, 1, 0, false}, Case{2, 81, 2, 0, true},
           Case{2, 78, 1, 2, true}}) {
    GameplayScoreState state(beatorajaScoreConfig(100, 7, 100.0));
    state.configureGauge(GaugeType::Normal, GaugeAutoShiftMode::None);
    state.stagePassedNotes = 100 - test.remaining;
    state.comboBreak = 1;
    state.currentGauge = state.gaugeValues[gaugeTypeIndex(GaugeType::Normal)] = test.gauge;
    quick_retry::Projection pending;
    pending.pendingClassicJudgements[Bad] = test.bads;
    pending.hellChargeTicks = test.hcnTicks;

    auto played = state;
    for (int i = 0; i < test.bads; ++i) played.commitJudge(JudgeResult(Bad, 0));
    for (int i = test.bads; i < test.remaining; ++i) played.commitJudge(JudgeResult(PGreat, 0));
    for (int i = 0; i < test.hcnTicks; ++i) played.applyGaugeJudgementRate(Great, 0.5F);
    if ((played.getClearTypeRank() == kClearTypeNormalClearRank) != test.clears)
      std::cerr << "start=" << test.gauge << " remaining=" << test.remaining
                << " bads=" << test.bads << " ticks=" << test.hcnTicks
                << " final=" << played.currentGauge << '\n';
    require((played.getClearTypeRank() == kClearTypeNormalClearRank) == test.clears,
            "deferred BAD fixture confirms attainable clear with real gauge clamping");
    require(quick_retry::shouldRetry(state, 100, {.lamp = kClearTypeNormalClearRank}, pending) ==
                !test.clears,
            "projection applies deferred damage at the floor before optimistic recovery");
  }
}

void testQuickRetryReachability() {
  require(quick_retry::remainingHellChargeTicks(1'000'000, 1'200'000, 0, 0) == 0,
          "exactly 200ms future HCN cannot earn a strict recovery tick");
  require(quick_retry::remainingHellChargeTicks(1'000'000, 2'000'000, 1'500'000, -100'000) == 1,
          "negative carried HCN balance reduces remaining recovery");
  const std::array mixed{quick_retry::Condition{.score = 199},
                         quick_retry::Condition{.score = 198},
                         quick_retry::Condition{.lamp = 400},
                         quick_retry::Condition{.lamp = 200}};
  const auto targets = quick_retry::compileTargets(mixed);
  require(targets.score == 198 && targets.lamp == 200,
          "OR goals compile to the weakest unfinished score and lamp once");
  quick_retry::ProjectionCache cache;
  cache.reset(4);
  cache.setClassic(1, Great);
  cache.setClassic(1, Great);
  cache.setHellCharge(2, 4);
  cache.setHellCharge(3, 2);
  require(cache.get().pendingClassicJudgements[Great] == 1 && cache.get().hellChargeTicks == 6,
          "per-note updates replace contributions rather than double count");
  cache.resolveClassic(1);
  cache.setHellCharge(2, 1);
  require(cache.get().pendingClassicJudgements[Great] == 0 && cache.get().hellChargeTicks == 3,
          "resolved heads and earned ticks update cached totals");
  GameplayScoreState state(beatorajaScoreConfig(100, 7, 100.0));
  state.configureGauge(GaugeType::Normal, GaugeAutoShiftMode::None);
  const std::array score{quick_retry::Condition{.score = 199}};
  require(!quick_retry::shouldRetry(state, 100, quick_retry::compileTargets(score)), "perfect start can reach target");
  quick_retry::Projection pending;
  pending.pendingClassicJudgements[Great] = 1;
  const std::array maxScore{quick_retry::Condition{.score = 200}};
  require(quick_retry::shouldRetry(state, 100, quick_retry::compileTargets(maxScore), pending),
          "accepted GREAT classic head makes MAX impossible before its tail");
  state.commitJudge(JudgeResult(Great, 0));
  require(!quick_retry::shouldRetry(state, 100, quick_retry::compileTargets(score)), "exactly attainable EX target continues");
  state.commitJudge(JudgeResult(Great, 0));
  require(quick_retry::shouldRetry(state, 100, quick_retry::compileTargets(score)), "one EX below target retries immediately");
  state.commitJudge(JudgeResult(Kpoor, 0));
  require(state.stagePassedNotes == 2, "empty poor consumes no remaining score opportunity");
  const std::array alternatives{quick_retry::Condition{.score = 199},
                               quick_retry::Condition{.score = 198}};
  require(!quick_retry::shouldRetry(state, 100, quick_retry::compileTargets(alternatives)), "any achievable alternative keeps attempt");
  require(!quick_retry::shouldRetry(state, 100, {}), "no targets must not cause retry loop");

  state.comboBreak = 1;
  state.stagePassedNotes = 99;
  state.currentGauge = state.gaugeValues[gaugeTypeIndex(GaugeType::Normal)] = 78;
  const std::array clear{quick_retry::Condition{.lamp = 300}};
  require(quick_retry::shouldRetry(state, 100, quick_retry::compileTargets(clear)), "one recovery point cannot reach 80 border");
  require(!quick_retry::shouldRetry(state, 100, quick_retry::compileTargets(clear), {.hellChargeTicks = 2}), "HCN recovery can rescue clear target");
  state.currentGauge = state.gaugeValues[gaugeTypeIndex(GaugeType::Normal)] = 79;
  require(!quick_retry::shouldRetry(state, 100, quick_retry::compileTargets(clear)), "exact gauge border remains attainable");
  const std::array compound{quick_retry::Condition{.score = 199},
                           quick_retry::Condition{.lamp = 300}};
  require(!quick_retry::shouldRetry(state, 100, quick_retry::compileTargets(compound)), "unreachable score does not discard reachable lamp");
  const std::array fc{quick_retry::Condition{.lamp = 600}};
  require(quick_retry::shouldRetry(state, 100, quick_retry::compileTargets(fc)), "broken combo cannot recover full combo");
  state.comboBreak = 0;
  state.combo = state.maxCombo = 99;
  require(!quick_retry::shouldRetry(state, 100, quick_retry::compileTargets(fc)), "normal gauge can still earn full combo");
  state.lightAssistClearMark = true;
  require(quick_retry::shouldRetry(state, 100, quick_retry::compileTargets(clear)), "assist cap prevents normal clear goal");

  state.lightAssistClearMark = false;
  state.configureGauge(GaugeType::Hard, GaugeAutoShiftMode::None);
  state.gaugeSurvivalFailed[gaugeTypeIndex(GaugeType::Hard)] = true;
  const std::array reached{quick_retry::Condition{.score = 2}};
  require(!quick_retry::shouldRetry(state, 100, quick_retry::compileTargets(reached)),
          "survival failure must preserve a score target already reached");
  const std::array lostAtFailure{quick_retry::Condition{.score = 3}};
  require(quick_retry::shouldRetry(state, 100, quick_retry::compileTargets(lostAtFailure)),
          "terminal survival failure cannot earn remaining EX");
  state.configureGauge(GaugeType::Hard, GaugeAutoShiftMode::BestClear);
  state.comboBreak = 1;
  state.gaugeSurvivalFailed[gaugeTypeIndex(GaugeType::Hazard)] = true;
  state.gaugeSurvivalFailed[gaugeTypeIndex(GaugeType::ExHard)] = true;
  const std::array hard{quick_retry::Condition{.lamp = 400}};
  require(!quick_retry::shouldRetry(state, 100, quick_retry::compileTargets(hard)), "Best Clear retains surviving hard gauge");
  state.gaugeSurvivalFailed[gaugeTypeIndex(GaugeType::Hard)] = true;
  require(quick_retry::shouldRetry(state, 100, quick_retry::compileTargets(hard)), "dead hard gauges cannot recover after shift");
}

int main() {
  testQuickRetryDeferredBadGaugeFloor();
  testQuickRetryReachability();
  static_assert(kClearTypeLightAssistedEasyClearRank == 50);
  static_assert(kClearTypeAssistedEasyClearRank == 100);
  static_assert(normalizeClearLamp(150) == kClearTypeLightAssistedEasyClearRank);
  require(clear_policy::fullComboRankForPlayback(150, true, {}) == 50,
          "legacy Light Assist remains assisted even with zero combo breaks");
  GameplayScoreState recalled(beatorajaScoreConfig(10, 7, 100));
  recalled.restoreClearTypeRankForResult(150);
  require(recalled.getClearTypeRank() == 50, "legacy Light Assist recall uses canonical rank");
  require(std::string(clearTypeRankToLabel(150)) == "LIGHT ASSIST EASY CLEAR",
          "legacy lamp label remains Light Assist");
  require(std::string(clearTypeRankToLabel(100)) == "ASSIST EASY CLEAR",
          "Assist Easy remains stronger than canonical Light Assist");
  testCourseSurvivalToGrooveContinuesAfterGaugeDeath();
  testPinnedCountsAndComboByMode();
  testBestClearFallsBackToBottomGaugeWhenNoneQualifies();
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
