#include "ReplayPlaybackDriver.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

namespace replay {
namespace {

enum class EventKind { Input, Touch, LaneCover };

struct NextEvent {
  std::int64_t time = 0;
  EventKind kind = EventKind::Input;
};

void consider(std::optional<NextEvent> &selected, NextEvent candidate) {
  if (!selected || candidate.time < selected->time ||
      (candidate.time == selected->time && candidate.kind < selected->kind)) {
    selected = candidate;
  }
}

} // namespace

ReplayLogicalGameplayAdapter::ReplayLogicalGameplayAdapter(
    int keyMode, ReplayLogicalGameplayCallbacks callbacks)
    : keyMode_(keyMode), callbacks_(std::move(callbacks)) {}

bool ReplayLogicalGameplayAdapter::reversesScratchAt(
    std::span<const InputTransition> transitions,
    std::size_t index) const noexcept {
  if (index >= transitions.size()) {
    return false;
  }
  const auto &current = transitions[index];
  if (current.pressed || current.replayOnly ||
      (current.control.kind != LogicalControlKind::ScratchClockwise &&
       current.control.kind !=
           LogicalControlKind::ScratchCounterClockwise)) {
    return false;
  }
  for (std::size_t next = index + 1; next < transitions.size(); ++next) {
    const auto &candidate = transitions[next];
    if (candidate.songTimeMicros != current.songTimeMicros) {
      break;
    }
    if (candidate.pressed && !candidate.replayOnly &&
        candidate.control.player == current.control.player &&
        candidate.control.kind != current.control.kind &&
        (candidate.control.kind == LogicalControlKind::ScratchClockwise ||
         candidate.control.kind ==
             LogicalControlKind::ScratchCounterClockwise)) {
      return true;
    }
  }
  return false;
}

bool ReplayLogicalGameplayAdapter::apply(
    const InputTransition &transition, bool reversesScratch,
    std::int64_t dispatchSongTimeMicros, std::string &diagnostic) {
  const auto kind = transition.control.kind;
  if (kind == LogicalControlKind::Start ||
      kind == LogicalControlKind::Select) {
    if (transition.control.lane != -1 || transition.control.player < 1 ||
        transition.control.player > 2) {
      diagnostic = "Replay command control is invalid.";
      return false;
    }
    if (callbacks_.command) {
      callbacks_.command(transition.control, transition.pressed);
    }
    return true;
  }

  const auto lane =
      physicalChartLaneForLogicalControl(keyMode_, transition.control);
  if (!lane) {
    diagnostic = "Replay logical control has no physical chart lane.";
    return false;
  }
  const double delay = static_cast<double>(std::max<std::int64_t>(
                           0, dispatchSongTimeMicros -
                                  transition.songTimeMicros)) /
                       1'000'000.0;
  const bool scratch = kind == LogicalControlKind::ScratchClockwise ||
                       kind == LogicalControlKind::ScratchCounterClockwise;
  if (!scratch) {
    if (transition.pressed) {
      if (!heldLanes_.insert(*lane).second) {
        diagnostic = "Replay lane is already pressed.";
        return false;
      }
      if (callbacks_.pressLane) {
        callbacks_.pressLane(*lane, delay);
      }
      return true;
    }
    if (heldLanes_.erase(*lane) == 0) {
      diagnostic = "Replay lane release has no matching press.";
      return false;
    }
    if (callbacks_.releaseLane) {
      callbacks_.releaseLane(*lane, delay, false);
    }
    return true;
  }

  auto active = activeScratchDirections_.find(*lane);
  if (transition.replayOnly) {
    if (transition.pressed) {
      if (!heldLanes_.contains(*lane) ||
          active != activeScratchDirections_.end()) {
        diagnostic = "Replay-only scratch press has no ownership handoff.";
        return false;
      }
      heldScratchDirections_[*lane].insert(kind);
      activeScratchDirections_[*lane] = kind;
      return true;
    }
    if (!heldLanes_.contains(*lane) ||
        active == activeScratchDirections_.end() ||
        active->second != kind) {
      diagnostic = "Replay-only scratch release has no active owner.";
      return false;
    }
    if (const auto held = heldScratchDirections_.find(*lane);
        held != heldScratchDirections_.end()) {
      held->second.erase(kind);
      if (held->second.empty()) {
        heldScratchDirections_.erase(held);
      }
    }
    activeScratchDirections_.erase(active);
    return true;
  }

  if (transition.pressed) {
    if (!heldScratchDirections_[*lane].insert(kind).second) {
      diagnostic = "Replay scratch direction is already pressed.";
      return false;
    }
    if (active != activeScratchDirections_.end()) {
      if (callbacks_.releaseLane) {
        callbacks_.releaseLane(*lane, delay, true);
      }
      if (callbacks_.pressLane) {
        callbacks_.pressLane(*lane, delay);
      }
    } else {
      if (!heldLanes_.insert(*lane).second) {
        diagnostic = "Replay scratch physical lane has no logical owner.";
        return false;
      }
      if (callbacks_.pressLane) {
        callbacks_.pressLane(*lane, delay);
      }
    }
    activeScratchDirections_[*lane] = kind;
    return true;
  }
  const auto held = heldScratchDirections_.find(*lane);
  if (held == heldScratchDirections_.end() || held->second.erase(kind) == 0) {
    diagnostic = "Replay scratch release has no matching direction.";
    return false;
  }
  if (active != activeScratchDirections_.end() && active->second != kind) {
    return true;
  }
  if (!held->second.empty()) {
    activeScratchDirections_[*lane] = *held->second.begin();
    if (callbacks_.releaseLane) {
      callbacks_.releaseLane(*lane, delay, true);
    }
    if (callbacks_.pressLane) {
      callbacks_.pressLane(*lane, delay);
    }
    return true;
  }
  heldScratchDirections_.erase(held);
  if (active == activeScratchDirections_.end() ||
      heldLanes_.erase(*lane) == 0) {
    diagnostic = "Replay scratch release has no active physical lane.";
    return false;
  }
  activeScratchDirections_.erase(active);
  if (callbacks_.releaseLane) {
    callbacks_.releaseLane(*lane, delay, reversesScratch);
  }
  return true;
}

bool ReplayLogicalGameplayAdapter::applyBatch(
    std::span<const InputTransition> transitions,
    std::int64_t dispatchSongTimeMicros, std::string &diagnostic) {
  diagnostic.clear();
  if (!replayKeyModeLayout(keyMode_)) {
    diagnostic = "Replay adapter key mode is unsupported.";
    return false;
  }
  if (transitions.empty()) {
    return true;
  }
  const auto timestamp = transitions.front().songTimeMicros;
  for (const auto &transition : transitions) {
    if (transition.songTimeMicros != timestamp) {
      diagnostic = "Replay input batch mixes timestamps.";
      return false;
    }
  }
  for (std::size_t index = 0; index < transitions.size(); ++index) {
    if (!apply(transitions[index], reversesScratchAt(transitions, index),
               dispatchSongTimeMicros, diagnostic)) {
      return false;
    }
  }
  return true;
}

void ReplayLogicalGameplayAdapter::reset() {
  if (callbacks_.releaseLane) {
    for (const int lane : heldLanes_) {
      callbacks_.releaseLane(lane, 0.0, false);
    }
  }
  heldLanes_.clear();
  heldScratchDirections_.clear();
  activeScratchDirections_.clear();
}

ReplayPlaybackDriver::ReplayPlaybackDriver(const ReplayChartDocument &document,
                                           ReplayLimits limits)
    : document_(document), limits_(limits),
      currentTimeMicros_(limits.minimumSongTimeMicros) {
  valid_ = limits.valid() && document.timeBounds.valid();
  if (!valid_) {
    diagnostic_ = "Replay playback limits or completion bounds are invalid.";
  }
}

bool ReplayPlaybackDriver::complete() const noexcept {
  return valid_ && hasAdvanced_ &&
         currentTimeMicros_ >= document_.timeBounds.completionSongTimeMicros &&
         nextInput_ == document_.playback.input.size() &&
         nextTouch_ == document_.playback.touchSamples.size() &&
         nextLaneCover_ == document_.playback.laneCoverEvents.size();
}

ReplayPlaybackAdvanceOutcome ReplayPlaybackDriver::advanceTo(
    std::int64_t songTimeMicros, const ReplayPlaybackSink &sink,
    std::size_t eventBudget) {
  if (!valid_) {
    return {.state = ReplayPlaybackDriverState::InvalidPlayback,
            .diagnostic = diagnostic_};
  }
  if (hasAdvanced_ && songTimeMicros < currentTimeMicros_) {
    return {.state = ReplayPlaybackDriverState::NonMonotonicAdvance,
            .diagnostic = "Replay time cannot move backwards."};
  }
  if (!document_.timeBounds.contains(songTimeMicros, limits_)) {
    return {.state = ReplayPlaybackDriverState::TimeOutOfBounds,
            .diagnostic = "Replay advance lies outside the attempt bounds."};
  }

  std::size_t delivered = 0;
  while (true) {
    std::optional<NextEvent> next;
    if (nextInput_ < document_.playback.input.size()) {
      consider(next, {.time = document_.playback.input[nextInput_]
                                  .songTimeMicros,
                      .kind = EventKind::Input});
    }
    if (nextTouch_ < document_.playback.touchSamples.size()) {
      consider(next, {.time = document_.playback.touchSamples[nextTouch_]
                                  .songTimeMicros,
                      .kind = EventKind::Touch});
    }
    if (nextLaneCover_ < document_.playback.laneCoverEvents.size()) {
      consider(next, {.time = document_.playback
                                  .laneCoverEvents[nextLaneCover_]
                                  .songTimeMicros,
                      .kind = EventKind::LaneCover});
    }
    if (!next || next->time > songTimeMicros) {
      break;
    }
    if (delivered >= eventBudget) {
      return {.state = ReplayPlaybackDriverState::WorkLimitExceeded,
              .deliveredEvents = delivered,
              .diagnostic = "Replay event work limit was exceeded."};
    }

    if (next->kind == EventKind::Input && sink.inputBatch) {
      const auto first = nextInput_;
      auto end = first + 1;
      while (end < document_.playback.input.size() &&
             document_.playback.input[end].songTimeMicros == next->time) {
        ++end;
      }
      const auto count = end - first;
      if (count > eventBudget - delivered) {
        return {.state = ReplayPlaybackDriverState::WorkLimitExceeded,
                .deliveredEvents = delivered,
                .diagnostic = "Replay event work limit was exceeded."};
      }
      std::string diagnostic;
      if (!sink.inputBatch(
              std::span(document_.playback.input).subspan(first, count),
              diagnostic)) {
        return {.state = ReplayPlaybackDriverState::SinkRejected,
                .deliveredEvents = delivered,
                .diagnostic = diagnostic.empty()
                                  ? "Replay input batch sink rejected input."
                                  : std::move(diagnostic)};
      }
      nextInput_ = end;
      delivered += count;
      continue;
    }

    std::string diagnostic;
    bool accepted = true;
    switch (next->kind) {
    case EventKind::Input:
      if (sink.input) {
        accepted = sink.input(document_.playback.input[nextInput_], diagnostic);
      }
      if (accepted) {
        ++nextInput_;
      }
      break;
    case EventKind::Touch:
      if (sink.touch) {
        accepted =
            sink.touch(document_.playback.touchSamples[nextTouch_], diagnostic);
      }
      if (accepted) {
        ++nextTouch_;
      }
      break;
    case EventKind::LaneCover:
      if (sink.laneCover) {
        accepted = sink.laneCover(
            document_.playback.laneCoverEvents[nextLaneCover_], diagnostic);
      }
      if (accepted) {
        ++nextLaneCover_;
      }
      break;
    }
    if (!accepted) {
      return {.state = ReplayPlaybackDriverState::SinkRejected,
              .deliveredEvents = delivered,
              .diagnostic = diagnostic.empty()
                                ? "Replay event sink rejected input."
                                : std::move(diagnostic)};
    }
    ++delivered;
  }

  currentTimeMicros_ = songTimeMicros;
  hasAdvanced_ = true;
  return {.state = complete() ? ReplayPlaybackDriverState::Complete
                              : ReplayPlaybackDriverState::Advanced,
          .deliveredEvents = delivered};
}

} // namespace replay
