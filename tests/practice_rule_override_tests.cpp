#include "scene/play/GamePlayStartOptions.h"

#include <array>
#include <cassert>
#include <iostream>
#include <map>
#include <string>
#include <utility>

namespace {
void testJudgeScaleRunsAfterCourseConstraint() {
  StartOptions options;
  options.courseConstraints.judgement = CourseJudgementConstraint::NoGood;
  options.playback.percent = 75;
  options.judgeWindowScalePercent = 80;
  const Judge judge = makeEffectiveJudgeAtPlayStart(options, 1);

  assert((judge.timingWindows.at(PGreat) ==
          std::pair<long long, long long>(-6000, 6000)));
  assert(judge.timingWindows.at(Good) == judge.timingWindows.at(Great));
  assert((judge.timingWindows.at(Good) ==
          std::pair<long long, long long>(-18000, 18000)));
}

void testJudgeScaleRoundsSignedWindowEdges() {
  Judge judge(0);
  judge.applyWindowScale(75, 45);

  assert((judge.timingWindows.at(Bad) ==
          std::pair<long long, long long>(-129938, 165375)));
}

void testStartingGaugeUpdatesSelectedGaugeAndClamps() {
  bms_parser::Chart chart;
  chart.Meta.TotalNotes = 100;
  RhythmState state(&chart, false);
  state.configureGauge(GaugeType::Hard, GaugeAutoShiftMode::None);

  state.setStartingGaugePercent(37);
  assert(state.currentGauge == 37.0f);
  assert(state.gaugeValues[gaugeTypeIndex(GaugeType::Hard)] == 37.0f);
  assert(state.gaugeValues[gaugeTypeIndex(GaugeType::Normal)] == 20.0f);

  state.setStartingGaugePercent(101);
  assert(state.currentGauge == 100.0f);
  assert(state.gaugeValues[gaugeTypeIndex(GaugeType::Hard)] == 100.0f);

  state.setStartingGaugePercent(-1);
  assert(state.currentGauge == 0.0f);
  assert(state.gaugeValues[gaugeTypeIndex(GaugeType::Hard)] == 0.0f);
  assert(state.activeGaugeFailed());

  state.setStartingGaugePercent(37);
  assert(!state.activeGaugeFailed());
}

void testStartingGaugeUpdatesEveryAutoShiftCandidateAndSnapshot() {
  bms_parser::Chart chart;
  chart.Meta.TotalNotes = 100;
  RhythmState state(&chart, false);
  state.configureGauge(GaugeType::Hard, GaugeAutoShiftMode::BestClear);

  state.setStartingGaugePercent(37);
  for (const float value : state.gaugeValues) {
    assert(value == 37.0f);
  }
  assert(state.currentGauge == 37.0f);

  const GaugeStateSnapshot snapshot = state.gaugeSnapshot();
  assert(snapshot.currentGauge == 37.0f);
  for (const float value : snapshot.gaugeValues) {
    assert(value == 37.0f);
  }
}

void testSurvivalToGrooveStartsBothGaugeCandidates() {
  bms_parser::Chart chart;
  chart.Meta.KeyMode = 9;
  chart.Meta.TotalNotes = 100;
  RhythmState state(&chart, false);
  state.configureGauge(GaugeType::Hard,
                       GaugeAutoShiftMode::SurvivalToGroove);

  assert(gaugeStartingMaximumValue(
             GaugeType::Hard, GaugeAutoShiftMode::SurvivalToGroove,
             GaugeType::AssistedEasy, state.gaugeProfile) == 120.0f);
  state.setStartingGaugePercent(110);
  assert(state.currentGauge == 100.0f);
  assert(state.gaugeValues[gaugeTypeIndex(GaugeType::Hard)] == 100.0f);
  assert(state.gaugeValues[gaugeTypeIndex(GaugeType::Normal)] == 110.0f);
}

void testZeroStartingGaugeResolvesAutoShiftImmediately() {
  bms_parser::Chart chart;
  chart.Meta.TotalNotes = 100;

  RhythmState continued(&chart, false);
  continued.configureGauge(GaugeType::Hard, GaugeAutoShiftMode::Continue);
  continued.setStartingGaugePercent(0);
  assert(continued.gaugeType == GaugeType::Hard);
  assert(continued.gaugeSurvivalFailed[gaugeTypeIndex(GaugeType::Hard)]);
  assert(!continued.activeGaugeFailed());

  RhythmState survivalToGroove(&chart, false);
  survivalToGroove.configureGauge(
      GaugeType::Hard, GaugeAutoShiftMode::SurvivalToGroove);
  survivalToGroove.setStartingGaugePercent(0);
  assert(survivalToGroove.gaugeType == GaugeType::Normal);
  assert(survivalToGroove.currentGauge == 0.0f);
  assert(!survivalToGroove.activeGaugeFailed());

  RhythmState bestClear(&chart, false);
  bestClear.configureGauge(GaugeType::Hard, GaugeAutoShiftMode::BestClear);
  bestClear.setStartingGaugePercent(0);
  assert(bestClear.gaugeType == GaugeType::Normal);
  assert(!bestClear.activeGaugeFailed());

  RhythmState survivalOnly(&chart, false);
  survivalOnly.configureGauge(GaugeType::ExHard,
                              GaugeAutoShiftMode::SelectToUnder,
                              GaugeProfile::Standard, GaugeType::Hard);
  survivalOnly.setStartingGaugePercent(0);
  assert(survivalOnly.gaugeType == GaugeType::Hard);
  assert(survivalOnly.activeGaugeFailed());
}

void testPracticeConfigurationCopiesGaugeAutoShiftToGameplayOptions() {
  practice::Configuration configuration;
  configuration.startMicros = 2'000'000;
  configuration.gaugeType = GaugeType::ExHard;
  configuration.gaugeAutoShift = GaugeAutoShiftMode::BestClear;
  configuration.playback.percent = 75;
  configuration.judge.scalePercent = 80;
  configuration.startingGaugePercent = 37;

  StartOptions options;
  options.startingGaugeState = GaugeStateSnapshot{
      .gaugeType = GaugeType::Hard,
      .selectedGaugeType = GaugeType::Hard,
      .gaugeAutoShiftLowerBound = GaugeType::AssistedEasy,
      .gaugeProfile = GaugeProfile::Standard,
      .gaugeAutoShift = GaugeAutoShiftMode::None,
      .currentGauge = 100.0F,
  };
  applyPracticeConfigurationToStartOptions(options, configuration);
  assert(options.startPosition == 2'000'000);
  assert(options.gaugeType == GaugeType::ExHard);
  assert(options.gaugeAutoShift == GaugeAutoShiftMode::BestClear);
  assert(options.playback.percent == 75);
  assert(options.judgeWindowScalePercent == 80);
  assert(options.startingGaugePercent == 37);
  assert(!options.startingGaugeState.has_value());
}

std::map<Judgement, std::pair<long long, long long>>
windowsFromStage(const ScoreStageProvenance &stage) {
  std::map<Judgement, std::pair<long long, long long>> windows;
  for (const auto &window : stage.effectiveJudgeWindows) {
    windows[window.judgement] = {window.earlyMicros, window.lateMicros};
  }
  return windows;
}

void testSavedPracticeReplayRestoresGaugeAndExactWindows() {
  bms_parser::ChartMeta meta;
  meta.MD5 = "practice-override-md5";
  meta.SHA256 = std::string(64, 'a');
  meta.Rank = 1;
  meta.TotalNotes = 100;

  StartOptions recordedOptions;
  recordedOptions.practiceMode = true;
  recordedOptions.playback = {.percent = 75,
                              .mode = audio::PlaybackMode::PitchShift};
  recordedOptions.judgeWindowScalePercent = 80;
  recordedOptions.startingGaugePercent = 37;
  recordedOptions.gaugeType = GaugeType::Hard;
  recordedOptions.inputDeviceCategories = {InputDeviceCategory::Keyboard};

  const Judge recordedJudge =
      makeEffectiveJudgeAtPlayStart(recordedOptions, meta);
  const ScoreProvenance captured = captureScoreProvenanceAtPlayStart(
      recordedOptions, meta, recordedJudge.timingWindows);

  std::string error;
  const auto persisted =
      deserializeScoreProvenance(serializeScoreProvenance(captured), error);
  assert(error.empty());
  assert(persisted.has_value());

  JudgedPlaybackData replay =
      analysis::retrySourceFromProvenance(meta, *persisted);
  StartOptions replayOptions;
  applyJudgedPlaybackSetupToStartOptions(replayOptions, replay);

  const Judge restoredJudge =
      makeEffectiveJudgeAtPlayStart(replayOptions, meta);
  assert(captured.stages.size() == 1);
  assert(windowsFromStage(captured.stages.front()) ==
         recordedJudge.timingWindows);
  assert(restoredJudge.timingWindows == recordedJudge.timingWindows);

  bms_parser::Chart chart;
  chart.Meta.TotalNotes = 100;
  RhythmState restoredState(&chart, false);
  restoredState.configureGauge(replay.setup.initialGaugeType,
                               GaugeAutoShiftMode::None);
  assert(replayOptions.startingGaugePercent.has_value());
  restoredState.setStartingGaugePercent(*replayOptions.startingGaugePercent);
  assert(restoredState.currentGauge == 37.0f);
}
} // namespace

int main() {
  testJudgeScaleRunsAfterCourseConstraint();
  testJudgeScaleRoundsSignedWindowEdges();
  testStartingGaugeUpdatesSelectedGaugeAndClamps();
  testStartingGaugeUpdatesEveryAutoShiftCandidateAndSnapshot();
  testSurvivalToGrooveStartsBothGaugeCandidates();
  testZeroStartingGaugeResolvesAutoShiftImmediately();
  testPracticeConfigurationCopiesGaugeAutoShiftToGameplayOptions();
  testSavedPracticeReplayRestoresGaugeAndExactWindows();
  std::cout << "practice rule override tests passed\n";
  return 0;
}
