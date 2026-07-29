#include "CourseContinuation.h"

#include "BeatorajaReplayPath.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>

namespace replay {
namespace {

CourseContinuationOutcome invalid(CourseContinuationIssue issue) noexcept {
  return {.issue = issue};
}

bool validConstraints(const CourseContinuationConstraints &constraints,
                      const ReplayLimits &limits) noexcept {
  if (!result_contract::isKnownLongNoteMode(constraints.longNoteMode) ||
      constraints.beatorajaConstraintIds.size() > limits.maxCourseStages) {
    return false;
  }
  int previous = kBeatorajaFirstConstraintId - 1;
  for (const int identifier : constraints.beatorajaConstraintIds) {
    if (identifier < kBeatorajaFirstConstraintId ||
        identifier > kBeatorajaLastConstraintId || identifier <= previous) {
      return false;
    }
    previous = identifier;
  }
  return true;
}

bool validGaugeState(const GaugeStateSnapshot &gauge,
                     GaugeType adoptedGauge) noexcept {
  if (!result_contract::isKnownGaugeType(gauge.gaugeType) ||
      !result_contract::isKnownGaugeType(gauge.selectedGaugeType) ||
      !result_contract::isKnownGaugeType(gauge.gaugeAutoShiftLowerBound) ||
      !result_contract::isKnownGaugeProfile(gauge.gaugeProfile) ||
      !result_contract::isKnownGaugeAutoShift(gauge.gaugeAutoShift) ||
      !result_contract::isKnownGaugeType(adoptedGauge) ||
      adoptedGauge != gauge.gaugeType || !std::isfinite(gauge.currentGauge) ||
      gauge.currentGauge < 0.0F || gauge.currentGauge > 100.0F) {
    return false;
  }
  for (const float value : gauge.gaugeValues) {
    if (!std::isfinite(value) || value < 0.0F || value > 100.0F) {
      return false;
    }
  }
  const int adoptedIndex = gaugeTypeIndex(adoptedGauge);
  return adoptedIndex >= 0 &&
         adoptedIndex < static_cast<int>(gauge.gaugeValues.size()) &&
         gauge.gaugeValues[static_cast<std::size_t>(adoptedIndex)] ==
             gauge.currentGauge;
}

bool validState(const CourseContinuationState &state,
                const ReplayLimits &limits) noexcept {
  return state.totalStages > 0 &&
         withinReplayCountLimit(state.totalStages, limits.maxCourseStages) &&
         state.nextStageIndex <= state.totalStages && state.score >= 0 &&
         state.maximumScore >= state.score && state.combo >= 0 &&
         state.maximumCombo >= state.combo &&
         state.restMicrosAfterStage.size() == state.nextStageIndex &&
         state.stageSetups.size() == state.nextStageIndex &&
         std::ranges::all_of(state.restMicrosAfterStage, [&](auto value) {
           return validCourseRestMicros(value, limits);
         }) &&
         validConstraints(state.constraints, limits) &&
         validGaugeState(state.gauge, state.adoptedGauge);
}

bool setupAgreesWithCourse(const ReplaySetup &setup,
                           const CourseContinuationState &state) noexcept {
  return setup.longNoteMode == state.constraints.longNoteMode &&
         setup.initialGaugeType == state.gauge.selectedGaugeType &&
         setup.gaugeProfile == state.gauge.gaugeProfile &&
         setup.gaugeAutoShift == state.gauge.gaugeAutoShift &&
         setup.gaugeAutoShiftLowerBound ==
             state.gauge.gaugeAutoShiftLowerBound;
}

bool addOverflows(std::int64_t left, std::int64_t right) noexcept {
  return right > 0 && left > std::numeric_limits<std::int64_t>::max() - right;
}

} // namespace

CourseContinuationOutcome startCourseContinuation(
    const CourseContinuationStart &start,
    const ReplayLimits &limits) noexcept {
  try {
    if (!limits.valid()) {
      return invalid(CourseContinuationIssue::Limits);
    }
    if (start.totalStages == 0 ||
        !withinReplayCountLimit(start.totalStages, limits.maxCourseStages)) {
      return invalid(CourseContinuationIssue::TotalStages);
    }
    if (!validConstraints(start.constraints, limits)) {
      return invalid(CourseContinuationIssue::Constraints);
    }
    if (!validGaugeState(start.initialGauge,
                         start.initialGauge.gaugeType)) {
      return invalid(CourseContinuationIssue::Gauge);
    }
    CourseContinuationState state{
        .totalStages = start.totalStages,
        .gauge = start.initialGauge,
        .adoptedGauge = start.initialGauge.gaugeType,
        .constraints = start.constraints,
    };
    state.restMicrosAfterStage.reserve(start.totalStages);
    state.stageSetups.reserve(start.totalStages);
    return {.state = std::move(state)};
  } catch (...) {
    return invalid(CourseContinuationIssue::State);
  }
}

CourseContinuationOutcome advanceCourseContinuation(
    const CourseContinuationState &current,
    const CourseStageCompletion &completion,
    const ReplayLimits &limits) noexcept {
  try {
    if (!limits.valid()) {
      return invalid(CourseContinuationIssue::Limits);
    }
    if (!validState(current, limits)) {
      return invalid(CourseContinuationIssue::State);
    }
    if (current.complete()) {
      return invalid(CourseContinuationIssue::Complete);
    }
    if (completion.stageIndex != current.nextStageIndex) {
      return invalid(CourseContinuationIssue::StageOrder);
    }
    if (completion.score < 0 || completion.maximumScore <= 0 ||
        completion.score > completion.maximumScore) {
      return invalid(CourseContinuationIssue::Score);
    }
    if (completion.combo < 0 || completion.maximumCombo < completion.combo) {
      return invalid(CourseContinuationIssue::Combo);
    }
    if (!validGaugeState(completion.gauge, completion.adoptedGauge)) {
      return invalid(CourseContinuationIssue::Gauge);
    }
    if (!validCourseRestMicros(completion.restMicrosAfterStage, limits)) {
      return invalid(CourseContinuationIssue::Rest);
    }
    if (!setupAgreesWithCourse(completion.setup, current)) {
      return invalid(CourseContinuationIssue::Setup);
    }
    if (addOverflows(current.score, completion.score) ||
        addOverflows(current.maximumScore, completion.maximumScore)) {
      return invalid(CourseContinuationIssue::Overflow);
    }

    CourseContinuationState next = current;
    ++next.nextStageIndex;
    next.score += completion.score;
    next.maximumScore += completion.maximumScore;
    next.combo = completion.combo;
    next.maximumCombo =
        std::max(current.maximumCombo, completion.maximumCombo);
    next.gauge = completion.gauge;
    next.adoptedGauge = completion.adoptedGauge;
    next.restMicrosAfterStage.push_back(completion.restMicrosAfterStage);
    next.stageSetups.push_back(completion.setup);
    return {.state = std::move(next)};
  } catch (...) {
    return invalid(CourseContinuationIssue::State);
  }
}

CourseContinuationOutcome recordCourseContinuationRest(
    const CourseContinuationState &current, std::size_t completedStageIndex,
    std::int64_t restMicros, const ReplayLimits &limits) noexcept {
  try {
    if (!limits.valid()) {
      return invalid(CourseContinuationIssue::Limits);
    }
    if (!validState(current, limits)) {
      return invalid(CourseContinuationIssue::State);
    }
    if (current.nextStageIndex == 0 ||
        completedStageIndex + 1 != current.nextStageIndex) {
      return invalid(CourseContinuationIssue::StageOrder);
    }
    if (!validCourseRestMicros(restMicros, limits)) {
      return invalid(CourseContinuationIssue::Rest);
    }

    CourseContinuationState next = current;
    next.restMicrosAfterStage[completedStageIndex] = restMicros;
    return {.state = std::move(next)};
  } catch (...) {
    return invalid(CourseContinuationIssue::State);
  }
}

} // namespace replay
