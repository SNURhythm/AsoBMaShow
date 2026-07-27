#pragma once

#include "ReplaySetup.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace replay {

enum class LogicalControlKind : std::uint8_t {
  Lane,
  ScratchClockwise,
  ScratchCounterClockwise,
  Start,
  Select,
};

struct LogicalControl {
  LogicalControlKind kind = LogicalControlKind::Lane;
  int player = 1;
  int lane = -1;

  bool operator==(const LogicalControl &) const = default;
};

struct InputTransition {
  std::int64_t songTimeMicros = 0;
  LogicalControl control;
  bool pressed = false;
  bool replayOnly = false;

  bool operator==(const InputTransition &) const = default;
};

enum class ReplayTouchAction : std::uint8_t {
  Down,
  Move,
  Up,
  Cancel,
};

struct ReplayTouchSample {
  ReplayTouchAction action = ReplayTouchAction::Move;
  std::int64_t fingerId = 0;
  std::int64_t songTimeMicros = 0;
  float x = 0.0F;
  float y = 0.0F;

  bool operator==(const ReplayTouchSample &) const = default;
};

struct ReplayLaneCoverEvent {
  std::int64_t songTimeMicros = 0;
  int noteStartPositionPercent = 0;
  bool resetVisibleTimeReference = false;

  bool operator==(const ReplayLaneCoverEvent &) const = default;
};

struct ReplayPlaybackData {
  ReplaySetup setup;
  std::vector<InputTransition> input;
  std::vector<ReplayTouchSample> touchSamples;
  std::vector<ReplayLaneCoverEvent> laneCoverEvents;

  bool operator==(const ReplayPlaybackData &) const = default;
};

struct CourseReplayPlaybackData {
  std::vector<ReplayPlaybackData> stages;
  std::vector<std::int64_t> restMicrosAfterStage;

  bool operator==(const CourseReplayPlaybackData &) const = default;
};

enum class ReplayPlaybackIssue : std::uint8_t {
  None,
  Setup,
  TimeBounds,
  InputCount,
  InputTime,
  InputOrder,
  InputControl,
  RedundantInput,
  ScratchHandoff,
  TouchCount,
  TouchTime,
  TouchOrder,
  TouchAction,
  TouchCoordinate,
  LaneCoverCount,
  LaneCoverTime,
  LaneCoverOrder,
  LaneCoverPercent,
  CourseStageCount,
  CourseShape,
  CourseRest,
};

struct ReplayPlaybackValidation {
  ReplayPlaybackIssue issue = ReplayPlaybackIssue::None;
  ReplaySetupIssue setupIssue = ReplaySetupIssue::None;
  std::size_t stageIndex = 0;

  [[nodiscard]] constexpr bool valid() const noexcept {
    return issue == ReplayPlaybackIssue::None;
  }
};

[[nodiscard]] ReplayPlaybackValidation validateReplayPlayback(
    const ReplayPlaybackData &data, ReplaySetupSource source,
    ReplayTimeBounds timeBounds,
    const ReplayLimits &limits = kReplayLimits);

[[nodiscard]] ReplayPlaybackValidation validateCourseReplayPlayback(
    const CourseReplayPlaybackData &data,
    std::span<const ReplaySetupSource> sources,
    std::span<const ReplayTimeBounds> timeBounds,
    const ReplayLimits &limits = kReplayLimits);

} // namespace replay
