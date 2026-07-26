#include "ReplayPlaybackDriver.h"

#include <algorithm>
#include <utility>

namespace replay {
namespace {

constexpr int kFirstPlayerScratchLane = 7;
constexpr int kSecondPlayerScratchLane = 15;

bool scratchKind(LogicalControlKind kind) noexcept {
  return kind == LogicalControlKind::ScratchClockwise ||
         kind == LogicalControlKind::ScratchCounterClockwise;
}

} // namespace

ReplayPlaybackDriver::ReplayPlaybackDriver(const ReplayPlaybackData &replay,
                                           IRhythmControl &control,
                                           CommandCallback commandCallback)
    : replay_(replay), control_(control),
      commandCallback_(std::move(commandCallback)) {}

int ReplayPlaybackDriver::physicalLane(
    const LogicalControl &logical) const noexcept {
  if (scratchKind(logical.kind)) {
    return logical.player == 2 ? kSecondPlayerScratchLane
                               : kFirstPlayerScratchLane;
  }
  if (logical.kind != LogicalControlKind::Lane || logical.lane < 0) {
    return -1;
  }
  if (logical.player == 1) {
    return logical.lane;
  }
  if (logical.player != 2) {
    return -1;
  }
  if (replay_.setup.keyMode == 10 || replay_.setup.keyMode == 14) {
    return logical.lane + 8;
  }
  if (replay_.setup.keyMode == 48) {
    return logical.lane + 26;
  }
  return -1;
}

bool ReplayPlaybackDriver::isScratch(
    const LogicalControl &logical) const noexcept {
  return scratchKind(logical.kind);
}

bool ReplayPlaybackDriver::reversesScratchAt(std::size_t index) const noexcept {
  if (index >= replay_.input.size()) {
    return false;
  }
  const auto &current = replay_.input[index];
  if (current.pressed || !isScratch(current.control)) {
    return false;
  }
  for (std::size_t candidate = index + 1; candidate < replay_.input.size();
       ++candidate) {
    const auto &next = replay_.input[candidate];
    if (next.songTimeMicros != current.songTimeMicros) {
      break;
    }
    if (next.pressed && isScratch(next.control) &&
        next.control.player == current.control.player &&
        next.control.kind != current.control.kind) {
      return true;
    }
  }
  return false;
}

void ReplayPlaybackDriver::apply(std::size_t index) {
  const auto &transition = replay_.input[index];
  const double inputDelay = static_cast<double>(
                                std::max<std::int64_t>(
                                    0, advanceTimeMicros_ -
                                           transition.songTimeMicros)) /
                            1'000'000.0;
  if (transition.control.kind == LogicalControlKind::Start ||
      transition.control.kind == LogicalControlKind::Select) {
    if (commandCallback_) {
      commandCallback_(transition.control, transition.pressed);
    }
    return;
  }

  const int lane = physicalLane(transition.control);
  if (lane < 0) {
    return;
  }
  if (!isScratch(transition.control)) {
    if (transition.pressed) {
      if (heldLanes_.insert(lane).second) {
        control_.pressLane(lane, inputDelay);
      }
    } else if (heldLanes_.erase(lane) != 0) {
      control_.releaseLane(lane, inputDelay, false);
    }
    return;
  }

  const auto active = activeScratchDirections_.find(lane);
  if (transition.replayOnly && !transition.pressed &&
      transition.control.lane == -1 &&
      (transition.control.player == 1 || transition.control.player == 2) &&
      active != activeScratchDirections_.end() &&
      active->second == transition.control.kind && heldLanes_.contains(lane) &&
      index + 1 < replay_.input.size()) {
    for (std::size_t candidateIndex = index + 1;
         candidateIndex < replay_.input.size(); ++candidateIndex) {
      const auto &candidate = replay_.input[candidateIndex];
      if (candidate.songTimeMicros != transition.songTimeMicros) {
        break;
      }
      if (!isScratch(candidate.control) ||
          candidate.control.player != transition.control.player) {
        continue;
      }
      if (candidate.replayOnly && candidate.pressed &&
          candidate.control.lane == -1 &&
          candidate.control.kind != transition.control.kind) {
        active->second = candidate.control.kind;
        return;
      }
      break;
    }
  }
  if (transition.pressed) {
    if (active != activeScratchDirections_.end() &&
        active->second != transition.control.kind) {
      control_.releaseLane(lane, inputDelay, true);
      control_.pressLane(lane, inputDelay);
    } else if (active == activeScratchDirections_.end()) {
      control_.pressLane(lane, inputDelay);
      heldLanes_.insert(lane);
    }
    activeScratchDirections_[lane] = transition.control.kind;
    return;
  }
  if (active == activeScratchDirections_.end() ||
      active->second != transition.control.kind) {
    return;
  }
  control_.releaseLane(lane, inputDelay, reversesScratchAt(index));
  activeScratchDirections_.erase(active);
  heldLanes_.erase(lane);
}

void ReplayPlaybackDriver::advanceTo(std::int64_t songTimeMicros) {
  advanceTimeMicros_ = songTimeMicros;
  while (next_ < replay_.input.size() &&
         replay_.input[next_].songTimeMicros <= songTimeMicros) {
    apply(next_);
    ++next_;
  }
}

void ReplayPlaybackDriver::reset() {
  for (const int lane : heldLanes_) {
    control_.releaseLane(lane, 0.0, false);
  }
  heldLanes_.clear();
  activeScratchDirections_.clear();
  next_ = 0;
  advanceTimeMicros_ = 0;
}

bool ReplayPlaybackDriver::finished() const noexcept {
  return next_ >= replay_.input.size();
}

} // namespace replay
