#pragma once

#include "ReplaySetup.h"

#include "../scene/play/GameplayScoreState.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace replay {

struct CourseContinuationConstraints {
  std::vector<int> beatorajaConstraintIds;
  int longNoteMode = 0;

  bool operator==(const CourseContinuationConstraints &) const = default;
};

struct CourseContinuationStart {
  std::size_t totalStages = 0;
  GaugeStateSnapshot initialGauge;
  CourseContinuationConstraints constraints;
};

struct CourseStageCompletion {
  std::size_t stageIndex = 0;
  std::int64_t score = 0;
  std::int64_t maximumScore = 0;
  int combo = 0;
  int maximumCombo = 0;
  GaugeStateSnapshot gauge;
  GaugeType adoptedGauge = GaugeType::Normal;
  std::int64_t restMicrosAfterStage = 0;
  std::optional<ReplaySetup> setup;
};

struct CourseContinuationState {
  std::size_t nextStageIndex = 0;
  std::size_t totalStages = 0;
  std::int64_t score = 0;
  std::int64_t maximumScore = 0;
  int combo = 0;
  int maximumCombo = 0;
  GaugeStateSnapshot gauge;
  GaugeType adoptedGauge = GaugeType::Normal;
  CourseContinuationConstraints constraints;
  std::vector<std::int64_t> restMicrosAfterStage;
  std::vector<std::optional<ReplaySetup>> stageSetups;

  [[nodiscard]] bool complete() const noexcept {
    return totalStages > 0 && nextStageIndex == totalStages;
  }
};

enum class CourseContinuationIssue : std::uint8_t {
  None,
  Limits,
  TotalStages,
  Constraints,
  State,
  Complete,
  StageOrder,
  Score,
  Combo,
  Gauge,
  Rest,
  Setup,
  Overflow,
};

struct CourseContinuationOutcome {
  CourseContinuationIssue issue = CourseContinuationIssue::None;
  std::optional<CourseContinuationState> state;

  [[nodiscard]] bool ready() const noexcept {
    return issue == CourseContinuationIssue::None && state.has_value();
  }

  [[nodiscard]] bool advanced() const noexcept { return ready(); }
};

[[nodiscard]] CourseContinuationOutcome startCourseContinuation(
    const CourseContinuationStart &start,
    const ReplayLimits &limits = kReplayLimits) noexcept;

[[nodiscard]] CourseContinuationOutcome advanceCourseContinuation(
    const CourseContinuationState &current,
    const CourseStageCompletion &completion,
    const ReplayLimits &limits = kReplayLimits) noexcept;

// Live play learns the result-screen rest duration after the stage transition
// has already produced the gauge/combo state needed for presentation. Replace
// that one completed-stage fact through the same validation boundary; callers
// must never clamp or mutate the continuation vector directly.
[[nodiscard]] CourseContinuationOutcome recordCourseContinuationRest(
    const CourseContinuationState &current, std::size_t completedStageIndex,
    std::int64_t restMicros,
    const ReplayLimits &limits = kReplayLimits) noexcept;

} // namespace replay
