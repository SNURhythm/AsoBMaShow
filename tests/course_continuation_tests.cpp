#if __has_include("replay/CourseContinuation.h")
#include "replay/CourseContinuation.h"
#define ASOBMASHOW_HAS_COURSE_CONTINUATION 1
#else
#define ASOBMASHOW_HAS_COURSE_CONTINUATION 0
#endif

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

#if ASOBMASHOW_HAS_COURSE_CONTINUATION

GaugeStateSnapshot gauge(float current, GaugeType adopted) {
  GaugeStateSnapshot value;
  value.gaugeType = adopted;
  value.selectedGaugeType = GaugeType::Hard;
  value.gaugeAutoShiftLowerBound = GaugeType::Easy;
  value.gaugeProfile = GaugeProfile::Standard;
  value.gaugeAutoShift = GaugeAutoShiftMode::Continue;
  value.currentGauge = current;
  for (std::size_t index = 0; index < value.gaugeValues.size(); ++index) {
    value.gaugeValues[index] = current + static_cast<float>(index);
    value.gaugeSurvivalFailed[index] = index == 5;
  }
  value.gaugeValues[static_cast<std::size_t>(gaugeTypeIndex(adopted))] =
      current;
  return value;
}

bool sameGauge(const GaugeStateSnapshot &left,
               const GaugeStateSnapshot &right) {
  return left.gaugeType == right.gaugeType &&
         left.selectedGaugeType == right.selectedGaugeType &&
         left.gaugeAutoShiftLowerBound == right.gaugeAutoShiftLowerBound &&
         left.gaugeProfile == right.gaugeProfile &&
         left.gaugeAutoShift == right.gaugeAutoShift &&
         left.currentGauge == right.currentGauge &&
         left.gaugeValues == right.gaugeValues &&
         left.gaugeSurvivalFailed == right.gaugeSurvivalFailed;
}

replay::ReplaySetup setup(int keyMode, char shaDigit) {
  replay::ReplaySetup value;
  value.chart.md5 = std::string(32, shaDigit);
  value.chart.sha256 = std::string(64, shaDigit);
  value.chart.keyMode = keyMode;
  value.longNoteMode = 1;
  value.initialGaugeType = GaugeType::Hard;
  value.gaugeProfile = GaugeProfile::Standard;
  value.gaugeAutoShift = GaugeAutoShiftMode::Continue;
  value.gaugeAutoShiftLowerBound = GaugeType::Easy;
  return value;
}

replay::CourseContinuationState initialState(std::size_t totalStages = 2) {
  const replay::CourseContinuationStart start{
      .totalStages = totalStages,
      .initialGauge = gauge(75.0F, GaugeType::Hard),
      .constraints = {.beatorajaConstraintIds = {4, 9},
                      .longNoteMode = 1},
  };
  const auto outcome = replay::startCourseContinuation(start);
  expect(outcome.ready(), "valid course continuation start is accepted");
  return outcome.state.value_or(replay::CourseContinuationState{});
}

replay::CourseStageCompletion completion(std::size_t stageIndex,
                                         std::int64_t score,
                                         std::int64_t maximumScore,
                                         int combo, int maximumCombo,
                                         float currentGauge, int keyMode,
                                         char shaDigit,
                                         std::int64_t restMicros) {
  return {
      .stageIndex = stageIndex,
      .score = score,
      .maximumScore = maximumScore,
      .combo = combo,
      .maximumCombo = maximumCombo,
      .gauge = gauge(currentGauge, GaugeType::Hard),
      .adoptedGauge = GaugeType::Hard,
      .restMicrosAfterStage = restMicros,
      .setup = setup(keyMode, shaDigit),
  };
}

void testContiguousMixedSetupCourseCarriesEveryStateFact() {
  auto state = initialState();
  const auto first = replay::advanceCourseContinuation(
      state, completion(0, 120, 200, 4, 7, 68.0F, 7, 'a',
                        replay::kReplayLimits.maxCourseRestMicros),
      replay::ReplaySetupSource::LocalCapture);
  expect(first.advanced(), "first contiguous course stage advances");
  if (!first.state) {
    return;
  }
  expect(first.state->nextStageIndex == 1 && first.state->totalStages == 2,
         "stage index and total stage count advance exactly once");
  expect(first.state->score == 120 && first.state->maximumScore == 200,
         "course score and maximum score are accumulated");
  expect(first.state->combo == 4 && first.state->maximumCombo == 7,
         "ending combo and course maximum combo are carried");
  expect(sameGauge(first.state->gauge,
                   gauge(68.0F, GaugeType::Hard)) &&
             first.state->adoptedGauge == GaugeType::Hard,
         "all gauge values, failures, selection, and adoption are carried");
  expect(first.state->restMicrosAfterStage.size() == 1 &&
             first.state->restMicrosAfterStage.front() ==
                 replay::kReplayLimits.maxCourseRestMicros,
         "inclusive maximum course rest is retained");
  expect(first.state->stageSetups.size() == 1 &&
             first.state->stageSetups.front().chart.keyMode == 7 &&
             first.state->constraints.beatorajaConstraintIds ==
                 std::vector<int>({4, 9}),
         "constraints and first-stage setup remain explicit");

  const auto second = replay::advanceCourseContinuation(
      *first.state, completion(1, 330, 400, 0, 9, 55.0F, 14, 'b', 0),
      replay::ReplaySetupSource::LocalCapture);
  expect(second.advanced() && second.state && second.state->complete(),
         "second contiguous stage completes the course");
  if (second.state) {
    expect(second.state->score == 450 &&
               second.state->maximumScore == 600 &&
               second.state->combo == 0 &&
               second.state->maximumCombo == 9,
           "second stage updates aggregate score and carried combo");
    expect(second.state->stageSetups.size() == 2 &&
               second.state->stageSetups[1].chart.keyMode == 14,
           "mixed per-stage replay setup is preserved in order");
  }
}

void testInvalidTransitionsLeaveThePriorStateUntouched() {
  const auto state = initialState();

  auto outOfOrder = completion(1, 1, 2, 0, 0, 70.0F, 7, 'a', 0);
  const auto order = replay::advanceCourseContinuation(
      state, outOfOrder, replay::ReplaySetupSource::LocalCapture);
  expect(order.issue == replay::CourseContinuationIssue::StageOrder &&
             !order.state,
         "an out-of-order stage is rejected instead of inserted");

  auto excessiveRest = completion(
      0, 1, 2, 0, 0, 70.0F, 7, 'a',
      replay::kReplayLimits.maxCourseRestMicros + 1);
  const auto rest = replay::advanceCourseContinuation(
      state, excessiveRest, replay::ReplaySetupSource::LocalCapture);
  expect(rest.issue == replay::CourseContinuationIssue::Rest && !rest.state,
         "rest above the shared limit is rejected instead of clamped");

  auto invalidGauge = completion(0, 1, 2, 0, 0, 70.0F, 7, 'a', 0);
  invalidGauge.gauge.gaugeValues[0] =
      std::numeric_limits<float>::quiet_NaN();
  const auto gauges = replay::advanceCourseContinuation(
      state, invalidGauge, replay::ReplaySetupSource::LocalCapture);
  expect(gauges.issue == replay::CourseContinuationIssue::Gauge &&
             !gauges.state,
         "a malformed per-gauge value rejects the transition");
}

void testScoreOverflowAndCompletedCourseCannotAdvance() {
  auto state = initialState();
  state.score = std::numeric_limits<std::int64_t>::max() - 2;
  state.maximumScore = std::numeric_limits<std::int64_t>::max() - 2;
  const auto overflow = replay::advanceCourseContinuation(
      state, completion(0, 3, 3, 0, 0, 70.0F, 7, 'a', 0),
      replay::ReplaySetupSource::LocalCapture);
  expect(overflow.issue == replay::CourseContinuationIssue::Overflow &&
             !overflow.state,
         "aggregate score arithmetic fails closed on overflow");

  state = initialState(1);
  const auto completed = replay::advanceCourseContinuation(
      state, completion(0, 1, 2, 0, 0, 70.0F, 7, 'a', 0),
      replay::ReplaySetupSource::LocalCapture);
  expect(completed.advanced() && completed.state && completed.state->complete(),
         "one-stage course reaches complete state");
  if (completed.state) {
    const auto extra = replay::advanceCourseContinuation(
        *completed.state, completion(1, 1, 2, 0, 0, 70.0F, 7, 'b', 0),
        replay::ReplaySetupSource::LocalCapture);
    expect(extra.issue == replay::CourseContinuationIssue::Complete &&
               !extra.state,
           "a completed course cannot accept another stage");
  }
}

#endif

} // namespace

int main() {
#if ASOBMASHOW_HAS_COURSE_CONTINUATION
  testContiguousMixedSetupCourseCarriesEveryStateFact();
  testInvalidTransitionsLeaveThePriorStateUntouched();
  testScoreOverflowAndCompletedCourseCannotAdvance();
#else
  expect(false, "CourseContinuation contract is not implemented");
#endif
  if (failures != 0) {
    std::cerr << failures << " course continuation test(s) failed\n";
    return 1;
  }
  std::cout << "course continuation tests passed\n";
  return 0;
}
