#include "AssistOptionUtils.h"
#include "scene/play/GameplayScoreState.h"
#include "scene/play/RhythmState.h"
#include "scene/play/QuickRetry.h"
#include "scene/play/QuickRetrySummary.h"

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

void testQuickRetryReason() {
  GameplayScoreState state(beatorajaScoreConfig(100, 7, 100));
  state.configureGauge(GaugeType::Normal, GaugeAutoShiftMode::None);
  state.stagePassedNotes = 99;
  state.gaugeValues[gaugeTypeIndex(GaugeType::Normal)] = 78;
  quick_retry::Targets targets{.pacemakerScore = 2, .requireGaugeClear = true};
  require(quick_retry::failureReason(state, 100, targets, {}, "AAA") ==
              "NORMAL CLEAR UNREACHABLE",
          "retry reason identifies failed gauge while score remains reachable");
  targets.allConditionsFail = true;
  require(quick_retry::failureReason(state, 100, targets, {}, "AAA").empty(),
          "all-fail policy has no reason while a target remains reachable");
  targets.pacemakerScore = 3;
  require(quick_retry::failureReason(state, 100, targets, {}, "AAA") ==
              "ALL TARGETS UNREACHABLE",
          "all-fail reason describes the combined failure");
  state.gaugeValues[gaugeTypeIndex(GaugeType::Normal)] = 79;
  targets.allConditionsFail = false;
  require(quick_retry::failureReason(state, 100, targets, {}, "AAA") ==
              "AAA UNREACHABLE",
          "reachable gauge leaves the failed pacemaker as the reason");
  require(quick_retry::failureReason(state, 100, {.lamp = kClearTypeFullComboRank}) ==
              "FULL COMBO UNREACHABLE",
          "goal-only retry names the unavailable lamp");
  state.configureGauge(GaugeType::Hard, GaugeAutoShiftMode::SurvivalToGroove);
  state.gaugeSurvivalFailed[gaugeTypeIndex(GaugeType::Hard)] = true;
  state.gaugeValues[gaugeTypeIndex(GaugeType::Normal)] = 78;
  require(quick_retry::failureReason(state, 100, {.requireGaugeClear = true}) ==
              "NORMAL CLEAR UNREACHABLE",
          "survival-to-groove reason names the final fallback gauge");
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

void testQuickRetryProjectedSurvivalToGroove() {
  GameplayScoreState state(beatorajaScoreConfig(100, 7, 100.0));
  state.configureGauge(GaugeType::Hazard, GaugeAutoShiftMode::SurvivalToGroove);
  for (int i = 0; i < 80; ++i) state.commitJudge(JudgeResult(PGreat, 0));
  quick_retry::Projection pending;
  pending.pendingClassicJudgements[Bad] = 1;
  auto played = state;
  played.commitJudge(JudgeResult(Bad, 0));
  for (int i = 0; i < 19; ++i) played.commitJudge(JudgeResult(PGreat, 0));
  require(played.getClearTypeRank() == kClearTypeNormalClearRank,
          "deferred BAD on Hazard can shift to Normal and clear");
  require(!quick_retry::shouldRetry(
              state, 100, {.lamp = kClearTypeNormalClearRank, .requireGaugeClear = true}, pending),
          "both gauge and goal checks allow the projected Normal fallback before Hazard fails");
  require(quick_retry::shouldRetry(state, 100, {.lamp = kClearTypeHardClearRank}, pending),
          "Normal fallback cannot satisfy a survival clear goal");
}

void testQuickRetryRequiredGauge() {
  const quick_retry::Targets gaugeOnly{.requireGaugeClear = true};
  require(!gaugeOnly.empty(), "gauge requirement enables retry without pacemaker targets");
  GameplayScoreState state(beatorajaScoreConfig(100, 7, 100.0));
  state.configureGauge(GaugeType::Normal, GaugeAutoShiftMode::None);
  const auto armed = quick_retry::armReachableTargets(
      state, 100, {.score = 201, .pacemakerScore = 199, .requireGaugeClear = true});
  require(armed.score < 0 && armed.pacemakerScore == 199 && armed.requireGaugeClear,
          "impossible initial goals leave independent pacemaker and gauge requirements armed");
  const auto goalOnly = quick_retry::armReachableTargets(
      state, 100, {.score = 198, .pacemakerScore = 201});
  require(goalOnly.score == 198 && goalOnly.pacemakerScore < 0,
          "impossible initial pacemaker leaves achievable goals armed");
  GameplayScoreState lowTotal(beatorajaScoreConfig(1, 7, 1.0));
  lowTotal.configureGauge(GaugeType::Normal, GaugeAutoShiftMode::None);
  const auto scoreOnly = quick_retry::armReachableTargets(
      lowTotal, 1, {.score = 2, .requireGaugeClear = true});
  require(scoreOnly.score == 2 && !scoreOnly.requireGaugeClear,
          "impossible initial gauge does not loop or disable an achievable score target");
  state.stagePassedNotes = 99;
  state.comboBreak = 1;
  const auto setGauge = [&](GaugeType type, float value) {
    state.gaugeValues[gaugeTypeIndex(type)] = value;
    if (state.gaugeType == type) state.currentGauge = value;
  };
  setGauge(GaugeType::Normal, 78);
  require(quick_retry::shouldRetry(state, 100, gaugeOnly),
          "selected groove gauge retries before chart end when clear is impossible");
  require(quick_retry::shouldRetry(state, 100, {.score = 0, .requireGaugeClear = true}),
          "reached score does not suppress the independent gauge requirement");
  require(quick_retry::shouldRetry(state, 100, {.score = 2, .requireGaugeClear = true}),
          "reachable score does not suppress the independent gauge requirement");
  require(!quick_retry::shouldRetry(state, 100, gaugeOnly, {.hellChargeTicks = 2}),
          "gauge-only retry includes remaining HCN recovery");
  setGauge(GaugeType::Normal, 79);
  state.lightAssistClearMark = true;
  require(!quick_retry::shouldRetry(state, 100, gaugeOnly),
          "physical gauge border is independent of assist lamp caps");
  require(quick_retry::shouldRetry(state, 100, {.score = 3, .requireGaugeClear = true}),
          "achievable gauge does not suppress unreachable goals retry");
  require(quick_retry::shouldRetry(state, 100, {.score = 0, .pacemakerScore = 3}),
          "reachable goal does not suppress an independently enabled pacemaker retry");
  require(quick_retry::shouldRetry(state, 100, {.score = 3, .pacemakerScore = 2}),
          "reachable pacemaker does not suppress independently enabled goals retry");
  setGauge(GaugeType::Normal, 78);
  require(!quick_retry::shouldRetry(state, 100, {.pacemakerScore = 2}),
          "pacemaker-only selection ignores unselected goal and gauge requirements");
  state.lightAssistClearMark = false;

  for (const auto mode : {GaugeAutoShiftMode::SelectToUnder, GaugeAutoShiftMode::BestClear}) {
    state.configureGauge(GaugeType::Hard, mode, GaugeProfile::Standard, GaugeType::Easy);
    setGauge(GaugeType::Easy, 78);
    require(quick_retry::shouldRetry(state, 100, gaugeOnly),
            "unreachable auto-shift lower bound retries even with a surviving higher gauge");
    state.gaugeSurvivalFailed.fill(true);
    state.gaugeSurvivalFailed[gaugeTypeIndex(GaugeType::Easy)] = false;
    setGauge(GaugeType::Easy, 79);
    require(!quick_retry::shouldRetry(state, 100, gaugeOnly),
            "recoverable lower bound keeps attempt after higher gauges die");
  }
  state.configureGauge(GaugeType::Easy, GaugeAutoShiftMode::SelectToUnder,
                       GaugeProfile::Standard, GaugeType::Hard);
  setGauge(GaugeType::Easy, 79);
  require(!quick_retry::shouldRetry(state, 100, gaugeOnly),
          "lower bound above selected gauge uses the effective clamped bound");

  for (const auto mode : {GaugeAutoShiftMode::None, GaugeAutoShiftMode::Continue}) {
    state.configureGauge(GaugeType::Hard, mode);
    state.gaugeSurvivalFailed[gaugeTypeIndex(GaugeType::Hard)] = true;
    require(quick_retry::shouldRetry(state, 100, gaugeOnly),
            "failed selected survival gauge retries including Continue mode");
  }
  state.configureGauge(GaugeType::Hard, GaugeAutoShiftMode::SurvivalToGroove);
  setGauge(GaugeType::Normal, 78);
  require(!quick_retry::shouldRetry(state, 100, gaugeOnly),
          "Survival-to-Groove keeps a surviving selected gauge");
  state.gaugeSurvivalFailed[gaugeTypeIndex(GaugeType::Hard)] = true;
  state.gaugeType = GaugeType::Normal;
  setGauge(GaugeType::Normal, 79);
  require(!quick_retry::shouldRetry(state, 100, gaugeOnly),
          "Survival-to-Groove permits recovery through Normal");
  setGauge(GaugeType::Normal, 78);
  require(quick_retry::shouldRetry(state, 100, gaugeOnly),
          "Survival-to-Groove retries when Normal fallback cannot recover");
}

void testQuickRetryFailurePolicy() {
  GameplayScoreState state(beatorajaScoreConfig(100, 7, 100.0));
  state.configureGauge(GaugeType::Normal, GaugeAutoShiftMode::None);
  state.stagePassedNotes = 99;
  state.comboBreak = 1;
  for (int enabled = 0; enabled < 8; ++enabled) {
    for (int failed = 0; failed < 8; ++failed) {
      state.currentGauge = state.gaugeValues[gaugeTypeIndex(GaugeType::Normal)] =
          (failed & 1) ? 78 : 79;
      for (const bool all : {false, true}) {
        const quick_retry::Targets targets{
            .score = (enabled & 4) ? ((failed & 4) ? 3 : 2) : -1,
            .pacemakerScore = (enabled & 2) ? ((failed & 2) ? 3 : 2) : -1,
            .requireGaugeClear = bool(enabled & 1), .allConditionsFail = all};
        const bool expected = enabled != 0 &&
            (all ? (enabled & failed) == enabled : (enabled & failed) != 0);
        require(quick_retry::shouldRetry(state, 100, targets) == expected,
                "Any/All combines only enabled gauge, pacemaker and goal groups");
      }
    }
  }
  const quick_retry::Targets alternatives{.score = 3, .lamp = kClearTypeNormalClearRank,
      .pacemakerScore = 3, .allConditionsFail = true};
  state.currentGauge = state.gaugeValues[gaugeTypeIndex(GaugeType::Normal)] = 79;
  require(!quick_retry::shouldRetry(state, 100, alternatives),
          "All waits while an alternative goal lamp remains reachable");
  require(!quick_retry::shouldRetry(state, 100,
              {.score = 0, .pacemakerScore = 3, .allConditionsFail = true}),
          "a goal already reached this attempt keeps All mode from retrying");
  GameplayScoreState initial(beatorajaScoreConfig(100, 7, 100.0));
  const auto armed = quick_retry::armReachableTargets(initial, 100,
      {.score = 201, .pacemakerScore = 199, .allConditionsFail = true});
  require(armed.allConditionsFail && armed.score < 0 && armed.pacemakerScore == 199,
          "All mode preserves its policy while discarding impossible starting conditions");
  require(quick_retry::shouldRetry(state, 100, armed),
          "discarded starting conditions do not prevent All mode from retrying");
}

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
  require(quick_retry::remainingHellChargeTicks(1'000'000, 2'000'000, 1'500'000, -100'000) == 2,
          "negative HCN debt can be discharged before future recovery");
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

void testQuickRetrySummaryStartupEligibility() {
  GameplayScoreState initial(beatorajaScoreConfig(100, 7, 200));
  initial.configureGauge(GaugeType::Hard, GaugeAutoShiftMode::None);
  initial.setAssistClearMark(AssistClearMark::LightAssistedEasy);
  require(quick_retry::startupTargetEligible(initial, 100, {.lamp = kClearTypeHardClearRank}, true) == false,
          "Light Assist excludes the impossible HARD summary even with HCN recovery");
  require(quick_retry::startupTargetEligible(initial, 100, {.pacemakerScore = 178}, true) == true,
          "the independent AAA pacemaker remains in the summary");
  require(quick_retry::startupTargetEligible(initial, 100, {.score = 201}, false) == false,
          "summary omits scores beyond a perfect play");
  require(quick_retry::startupTargetEligible(initial, 100, {.requireGaugeClear = true}, true) == true,
          "assist caps do not remove the physical HARD gauge requirement");
  GameplayScoreState lowTotal(beatorajaScoreConfig(1, 7, 1));
  require(quick_retry::startupTargetEligible(lowTotal, 1, {.requireGaugeClear = true}, false) == false,
          "unreachable initial groove gauge is omitted without long notes");
  require(!quick_retry::startupTargetEligible(lowTotal, 1, {.requireGaugeClear = true}, true).has_value(),
          "unknown HCN recovery defers a summary instead of disabling a possible target");
}

void testQuickRetrySummaryCollapsesActualHurdles() {
  using namespace quick_retry;
  const Hurdle aa{HurdleKind::Score, 140, "AA"}, aaa{HurdleKind::Score, 160, "AAA"};
  const Hurdle clear{HurdleKind::Clear, 300, "CLEAR"}, hard{HurdleKind::Clear, 400, "HARD"};
  const std::vector<HurdleAlternatives> groups{{{aa}}, {{aaa}}};
  require(hurdleText(summarizeHurdles(groups, false), "or") == "AAA",
          "Any-fails uses the strictest independent score hurdle");
  require(hurdleText(summarizeHurdles(groups, true), "or") == "AA",
          "All-fail uses the easiest independent score hurdle");
  require(hurdleText(summarizeHurdles({{{hard}}, {{clear}}}, false), "or") == "HARD" &&
              hurdleText(summarizeHurdles({{{hard}}, {{clear}}}, true), "or") == "CLEAR",
          "comparable clear requirements follow the same strict/easy policy");
  const std::vector<HurdleAlternatives> mixed{{{hard}}, {{aaa}}, {{aa}, {clear}}};
  require(hurdleText(summarizeHurdles(mixed, false), "or") == "HARD + AAA",
          "a stricter score hurdle subsumes weaker alternative goals");
  const auto all = summarizeHurdles(mixed, true);
  require(hurdleText(all, "or") == "AA or CLEAR",
          "All-fail retains the weakest score and clear alternatives");
  const std::vector<HurdleAlternatives> shared{
      {{Hurdle{HurdleKind::Gauge, 0, "HARD"}}}, {{Hurdle{HurdleKind::Score, 0, "AAA"}}}};
  require(hurdleText(summarizeHurdles(shared, false), "or") == "HARD + AAA" &&
              hurdleText(summarizeHurdles(shared, true), "or") == "HARD or AAA",
          "shared gauge and grade labels preserve Any/All without chart-specific thresholds");
  const Hurdle physical{HurdleKind::Gauge, 300, "NORMAL"};
  require(summarizeHurdles({{{physical}}, {{hard}}}, false).front().size() == 2,
          "auto-shift physical gauge checks are not conflated with a clear lamp");
  // Compare the simplified expression with the original groups at boundaries.
  for (bool allFail : {false, true}) {
    const auto summary = summarizeHurdles(mixed, allFail);
    for (int score : {139, 140, 159, 160, 200}) {
      for (int lamp : {0, 300, 400, 600}) {
        const auto satisfied = [&](const HurdleClause &clause) {
          return std::all_of(clause.begin(), clause.end(), [&](const Hurdle &h) {
            return (h.kind == HurdleKind::Score ? score : lamp) >= h.value;
          });
        };
        const bool original = allFail
            ? (lamp >= 400 || score >= 160 || score >= 140 || lamp >= 300)
            : (lamp >= 400 && score >= 160 && (score >= 140 || lamp >= 300));
        require(std::any_of(summary.begin(), summary.end(), satisfied) == original,
                "concise hurdles preserve the original retry boundaries");
      }
    }
  }
  std::vector<chart_goals::GoalProgress> goals(4);
  goals[0].goal.longNoteMode = 1;
  goals[0].goal.target = {.lamp = 400, .scoreNumerator = 24};
  goals[0].progress.lampMet = true;
  goals[1].goal.longNoteMode = 1;
  goals[1].goal.target = {.scoreNumerator = 21};
  goals[2].goal.longNoteMode = 2;
  goals[2].goal.target = {.scoreNumerator = 18};
  goals[3].goal.longNoteMode = 1;
  goals[3].goal.target = {.lamp = 300};
  goals[3].progress.achieved = true;
  const auto compiled = compileGoalTargets(goals, 1, 100);
  require(compiled.targets.score == 156 && compiled.targets.lamp < 0 &&
              compiled.scoreLabel->scoreNumerator == 21,
          "summary and gameplay share unfinished, mode-specific goal thresholds");
}

int main() {
  testQuickRetrySummaryStartupEligibility();
  testQuickRetrySummaryCollapsesActualHurdles();
  testQuickRetryProjectedSurvivalToGroove();
  testQuickRetryRequiredGauge();
  testQuickRetryFailurePolicy();
  testQuickRetryReason();
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
