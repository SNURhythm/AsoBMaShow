#pragma once

#include "ReplayKeyMode.h"
#include "ReplayLaneCoverChange.h"
#include "ReplaySetup.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace replay {

enum class LogicalControlKind : std::uint8_t {
  Lane,
  ScratchClockwise,
  ScratchCounterClockwise,
  Start,
  Select,
};

[[nodiscard]] inline constexpr bool
isDirectionalScratchControl(LogicalControlKind kind) noexcept {
  return kind == LogicalControlKind::ScratchClockwise ||
         kind == LogicalControlKind::ScratchCounterClockwise;
}

struct LogicalControl {
  LogicalControlKind kind = LogicalControlKind::Lane;
  int player = 1;
  int lane = -1;

  bool operator==(const LogicalControl &) const = default;
};

[[nodiscard]] inline std::optional<LogicalControl>
logicalControlForChartLane(
    int keyMode, int chartLane, bool scratch,
    LogicalControlKind scratchKind =
        LogicalControlKind::ScratchClockwise) noexcept {
  const auto layout = replayKeyModeLayout(keyMode);
  if (!layout || chartLane < 0) {
    return std::nullopt;
  }
  const int playerOffset = keyMode == 48 ? 26 : 8;
  const int player = layout->players == 2 && chartLane >= playerOffset ? 2 : 1;
  const int playerLane = player == 2 ? chartLane - playerOffset : chartLane;
  if (scratch) {
    const int expectedScratch = player == 2 ? 15 : 7;
    if (!layout->hasDirectionalScratch || chartLane != expectedScratch ||
        !isDirectionalScratchControl(scratchKind)) {
      return std::nullopt;
    }
    return LogicalControl{.kind = scratchKind,
                          .player = player,
                          .lane = -1};
  }
  if (playerLane < 0 || playerLane >= layout->laneCodeWidthPerPlayer) {
    return std::nullopt;
  }
  return LogicalControl{.kind = LogicalControlKind::Lane,
                        .player = player,
                        .lane = playerLane};
}

[[nodiscard]] inline std::optional<int>
physicalChartLaneForLogicalControl(int keyMode,
                                   const LogicalControl &control) noexcept {
  const auto layout = replayKeyModeLayout(keyMode);
  if (!layout || control.player < 1 || control.player > layout->players) {
    return std::nullopt;
  }
  if (isDirectionalScratchControl(control.kind)) {
    if (!layout->hasDirectionalScratch || control.lane != -1) {
      return std::nullopt;
    }
    return control.player == 2 ? 15 : 7;
  }
  if (control.kind != LogicalControlKind::Lane || control.lane < 0 ||
      control.lane >= layout->laneCodeWidthPerPlayer) {
    return std::nullopt;
  }
  if (control.player == 1) {
    return control.lane;
  }
  return control.lane + (keyMode == 48 ? 26 : 8);
}

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
  bool laneCoverEnabled = false;
  ReplayLaneCoverChangeKind changeKind = ReplayLaneCoverChangeKind::Value;
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

// Final capture can race the audio cursor by a small amount because accepted
// realtime inputs retain their original steady-clock timestamp. The durable
// completion bound must cover every accepted replay stream rather than one
// later observation of the playback clock.
[[nodiscard]] ReplayTimeBounds replayCaptureTimeBounds(
    ReplayTimeBounds observed, std::span<const InputTransition> input,
    std::span<const ReplayTouchSample> touchSamples,
    std::span<const ReplayLaneCoverEvent> laneCoverEvents) noexcept;

// Realtime touch and lane-cover producers can be observed on different
// threads. Arrival order is not a playback validity fact, so normalize it at
// the local capture boundary while the decoder remains strict for BRD files.
[[nodiscard]] bool normalizeLocalReplayAuxiliaryStreams(
    std::vector<ReplayTouchSample> &touchSamples,
    std::vector<ReplayLaneCoverEvent> &laneCoverEvents,
    std::string &diagnostic,
    const ReplayLimits &limits = kReplayLimits) noexcept;

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
