#include "ReplayPlayback.h"

#include "ReplayKeyMode.h"

#include <array>
#include <cmath>
#include <optional>

namespace replay {
namespace {

ReplayPlaybackValidation invalid(ReplayPlaybackIssue issue) noexcept {
  return {.issue = issue};
}

bool scratchKind(LogicalControlKind kind) noexcept {
  return kind == LogicalControlKind::ScratchClockwise ||
         kind == LogicalControlKind::ScratchCounterClockwise;
}

bool validControl(const LogicalControl &control, int keyMode) noexcept {
  const auto layout = replayKeyModeLayout(keyMode);
  if (!layout || control.player < 1 || control.player > layout->players) {
    return false;
  }
  switch (control.kind) {
  case LogicalControlKind::Lane:
    return control.lane >= 0 && control.lane < layout->logicalLanesPerPlayer;
  case LogicalControlKind::ScratchClockwise:
  case LogicalControlKind::ScratchCounterClockwise:
    return layout->hasDirectionalScratch && control.lane == -1;
  case LogicalControlKind::Start:
  case LogicalControlKind::Select:
    return control.lane == -1;
  }
  return false;
}

std::size_t controlSlot(const LogicalControl &control) noexcept {
  switch (control.kind) {
  case LogicalControlKind::Lane:
    return static_cast<std::size_t>(control.player * 64 + control.lane);
  case LogicalControlKind::ScratchClockwise:
    return static_cast<std::size_t>(256 + control.player * 2);
  case LogicalControlKind::ScratchCounterClockwise:
    return static_cast<std::size_t>(257 + control.player * 2);
  case LogicalControlKind::Start:
    return static_cast<std::size_t>(300 + control.player);
  case LogicalControlKind::Select:
    return static_cast<std::size_t>(304 + control.player);
  }
  return 319;
}

struct PendingScratchHandoff {
  std::int64_t songTimeMicros = 0;
  LogicalControlKind released = LogicalControlKind::ScratchClockwise;
  bool active = false;
};

ReplayPlaybackIssue validateInput(std::span<const InputTransition> input,
                                  int keyMode, ReplayTimeBounds bounds,
                                  const ReplayLimits &limits) noexcept {
  if (!withinReplayCountLimit(input.size(), limits.maxInputTransitions)) {
    return ReplayPlaybackIssue::InputCount;
  }
  std::array<bool, 320> states{};
  std::array<std::optional<LogicalControlKind>, 3> activeScratch{};
  std::array<PendingScratchHandoff, 3> pending{};
  std::int64_t previous = 0;
  bool hasPrevious = false;

  for (const auto &transition : input) {
    if (!bounds.contains(transition.songTimeMicros, limits)) {
      return ReplayPlaybackIssue::InputTime;
    }
    if (hasPrevious && transition.songTimeMicros < previous) {
      return ReplayPlaybackIssue::InputOrder;
    }
    previous = transition.songTimeMicros;
    hasPrevious = true;

    for (std::size_t player = 1; player < pending.size(); ++player) {
      if (pending[player].active &&
          transition.songTimeMicros > pending[player].songTimeMicros) {
        return ReplayPlaybackIssue::ScratchHandoff;
      }
    }
    if (!validControl(transition.control, keyMode)) {
      return ReplayPlaybackIssue::InputControl;
    }
    const std::size_t slot = controlSlot(transition.control);
    if (states[slot] == transition.pressed) {
      return ReplayPlaybackIssue::RedundantInput;
    }
    states[slot] = transition.pressed;

    const int player = transition.control.player;
    if (transition.replayOnly) {
      if (!scratchKind(transition.control.kind)) {
        return ReplayPlaybackIssue::ScratchHandoff;
      }
      auto &active = activeScratch[static_cast<std::size_t>(player)];
      auto &handoff = pending[static_cast<std::size_t>(player)];
      if (!transition.pressed) {
        if (handoff.active || !active || *active != transition.control.kind) {
          return ReplayPlaybackIssue::ScratchHandoff;
        }
        handoff = {.songTimeMicros = transition.songTimeMicros,
                   .released = transition.control.kind,
                   .active = true};
      } else {
        if (!handoff.active ||
            handoff.songTimeMicros != transition.songTimeMicros ||
            handoff.released == transition.control.kind) {
          return ReplayPlaybackIssue::ScratchHandoff;
        }
        active = transition.control.kind;
        handoff = {};
      }
      continue;
    }

    if (scratchKind(transition.control.kind)) {
      auto &active = activeScratch[static_cast<std::size_t>(player)];
      if (pending[static_cast<std::size_t>(player)].active) {
        return ReplayPlaybackIssue::ScratchHandoff;
      }
      if (transition.pressed) {
        active = transition.control.kind;
      } else if (active && *active == transition.control.kind) {
        active.reset();
      }
    }
  }

  for (const auto &handoff : pending) {
    if (handoff.active) {
      return ReplayPlaybackIssue::ScratchHandoff;
    }
  }
  return ReplayPlaybackIssue::None;
}

bool validTouchAction(ReplayTouchAction action) noexcept {
  switch (action) {
  case ReplayTouchAction::Down:
  case ReplayTouchAction::Move:
  case ReplayTouchAction::Up:
  case ReplayTouchAction::Cancel:
    return true;
  }
  return false;
}

ReplayPlaybackIssue validateTouch(std::span<const ReplayTouchSample> samples,
                                  ReplayTimeBounds bounds,
                                  const ReplayLimits &limits) noexcept {
  if (!withinReplayCountLimit(samples.size(), limits.maxTouchSamples)) {
    return ReplayPlaybackIssue::TouchCount;
  }
  std::int64_t previous = 0;
  bool hasPrevious = false;
  for (const auto &sample : samples) {
    if (!bounds.contains(sample.songTimeMicros, limits)) {
      return ReplayPlaybackIssue::TouchTime;
    }
    if (hasPrevious && sample.songTimeMicros < previous) {
      return ReplayPlaybackIssue::TouchOrder;
    }
    previous = sample.songTimeMicros;
    hasPrevious = true;
    if (!validTouchAction(sample.action)) {
      return ReplayPlaybackIssue::TouchAction;
    }
    if (!std::isfinite(sample.x) || !std::isfinite(sample.y) ||
        sample.x < 0.0F || sample.x > 1.0F || sample.y < 0.0F ||
        sample.y > 1.0F) {
      return ReplayPlaybackIssue::TouchCoordinate;
    }
  }
  return ReplayPlaybackIssue::None;
}

ReplayPlaybackIssue
validateLaneCover(std::span<const ReplayLaneCoverEvent> events,
                  ReplayTimeBounds bounds,
                  const ReplayLimits &limits) noexcept {
  if (!withinReplayCountLimit(events.size(), limits.maxLaneCoverEvents)) {
    return ReplayPlaybackIssue::LaneCoverCount;
  }
  std::int64_t previous = 0;
  bool hasPrevious = false;
  for (const auto &event : events) {
    if (!bounds.contains(event.songTimeMicros, limits)) {
      return ReplayPlaybackIssue::LaneCoverTime;
    }
    if (hasPrevious && event.songTimeMicros < previous) {
      return ReplayPlaybackIssue::LaneCoverOrder;
    }
    previous = event.songTimeMicros;
    hasPrevious = true;
    if (event.noteStartPositionPercent < 0 ||
        event.noteStartPositionPercent > 100) {
      return ReplayPlaybackIssue::LaneCoverPercent;
    }
  }
  return ReplayPlaybackIssue::None;
}

} // namespace

ReplayPlaybackValidation validateReplayPlayback(const ReplayPlaybackData &data,
                                                ReplaySetupSource source,
                                                ReplayTimeBounds timeBounds,
                                                const ReplayLimits &limits) {
  const auto setup = validateReplaySetup(data.setup, source, limits);
  if (!setup.valid()) {
    return {.issue = ReplayPlaybackIssue::Setup, .setupIssue = setup.issue};
  }
  if (!timeBounds.valid()) {
    return invalid(ReplayPlaybackIssue::TimeBounds);
  }
  if (const auto issue = validateInput(data.input, data.setup.chart.keyMode,
                                       timeBounds, limits);
      issue != ReplayPlaybackIssue::None) {
    return invalid(issue);
  }
  if (const auto issue = validateTouch(data.touchSamples, timeBounds, limits);
      issue != ReplayPlaybackIssue::None) {
    return invalid(issue);
  }
  if (const auto issue =
          validateLaneCover(data.laneCoverEvents, timeBounds, limits);
      issue != ReplayPlaybackIssue::None) {
    return invalid(issue);
  }
  return {};
}

ReplayPlaybackValidation
validateCourseReplayPlayback(const CourseReplayPlaybackData &data,
                             std::span<const ReplaySetupSource> sources,
                             std::span<const ReplayTimeBounds> timeBounds,
                             const ReplayLimits &limits) {
  if (!limits.valid() || data.stages.empty() ||
      !withinReplayCountLimit(data.stages.size(), limits.maxCourseStages)) {
    return invalid(ReplayPlaybackIssue::CourseStageCount);
  }
  if (sources.size() != data.stages.size() ||
      timeBounds.size() != data.stages.size() ||
      data.restMicrosAfterStage.size() != data.stages.size()) {
    return invalid(ReplayPlaybackIssue::CourseShape);
  }
  for (std::size_t index = 0; index < data.stages.size(); ++index) {
    auto stage = validateReplayPlayback(data.stages[index], sources[index],
                                        timeBounds[index], limits);
    if (!stage.valid()) {
      stage.stageIndex = index;
      return stage;
    }
    if (!validCourseRestMicros(data.restMicrosAfterStage[index], limits)) {
      return {.issue = ReplayPlaybackIssue::CourseRest, .stageIndex = index};
    }
  }
  return {};
}

} // namespace replay
