#include "ReplayInputRecorder.h"

#include "ReplayKeyMode.h"

#include <algorithm>
#include <exception>
#include <ranges>
#include <utility>

namespace replay {
namespace {

bool structurallyValidControl(const LogicalControl &control) noexcept {
  if (control.player < 1 || control.player > 2) {
    return false;
  }
  switch (control.kind) {
  case LogicalControlKind::Lane: {
    int maximumLanes = 0;
    for (const auto &layout : kReplayKeyModeLayouts) {
      maximumLanes = std::max(maximumLanes, layout.logicalLanesPerPlayer);
    }
    return control.lane >= 0 && control.lane < maximumLanes;
  }
  case LogicalControlKind::ScratchClockwise:
  case LogicalControlKind::ScratchCounterClockwise:
  case LogicalControlKind::Start:
  case LogicalControlKind::Select:
    return control.lane == -1;
  }
  return false;
}

} // namespace

ReplayInputRecorder::ReplayInputRecorder(ReplayClock clock,
                                         ReplayLimits limits)
    : clock_(clock), limits_(limits) {
  if (limits_.valid()) {
    transitions_.reserve(
        std::min<std::size_t>(limits_.maxInputTransitions, 4096U));
  } else {
    failureDiagnostic_ = "Replay input limits are invalid";
  }
}

bool ReplayInputRecorder::rejectCapture(std::string &diagnostic,
                                        std::string message) noexcept {
  try {
    if (failureDiagnostic_.empty()) {
      failureDiagnostic_ = std::move(message);
    }
    diagnostic = failureDiagnostic_;
  } catch (...) {
    failureDiagnostic_.clear();
    diagnostic = "Replay input capture failed";
  }
  return false;
}

void ReplayInputRecorder::fail(std::string diagnostic) noexcept {
  std::string ignored;
  (void)rejectCapture(ignored,
                      diagnostic.empty() ? "Replay input capture failed"
                                         : std::move(diagnostic));
}

bool ReplayInputRecorder::record(std::int64_t steadyTimestampMicros,
                                 LogicalControl control, bool pressed,
                                 std::string &diagnostic,
                                 bool replayOnly) noexcept {
  if (finished_) {
    diagnostic = "Replay input recording has already finished";
    return false;
  }
  if (!failureDiagnostic_.empty()) {
    diagnostic = failureDiagnostic_;
    return false;
  }
  if (clock_.mapSteadyToSong == nullptr) {
    return rejectCapture(diagnostic, "Replay input clock is unavailable");
  }
  const auto songTime =
      clock_.mapSteadyToSong(clock_.context, steadyTimestampMicros);
  if (!songTime.has_value()) {
    return rejectCapture(diagnostic,
                         "Replay input clock could not map the timestamp");
  }
  return recordSongTime(*songTime, control, pressed, diagnostic, replayOnly);
}

bool ReplayInputRecorder::recordSongTime(std::int64_t songTimeMicros,
                                         LogicalControl control, bool pressed,
                                         std::string &diagnostic,
                                         bool replayOnly) noexcept {
  try {
    if (finished_) {
      diagnostic = "Replay input recording has already finished";
      return false;
    }
    if (!failureDiagnostic_.empty()) {
      diagnostic = failureDiagnostic_;
      return false;
    }
    if (!structurallyValidControl(control)) {
      return rejectCapture(diagnostic, "Replay input control is unsupported");
    }
    if (songTimeMicros < limits_.minimumSongTimeMicros) {
      return rejectCapture(diagnostic,
                           "Replay input is before the supported pre-roll");
    }
    if (lastSongTimeMicros_.has_value() &&
        songTimeMicros < *lastSongTimeMicros_) {
      return rejectCapture(diagnostic,
                           "Replay input song time decreased");
    }
    const auto state =
        std::ranges::find(states_, control, &ControlState::control);
    const bool current = state != states_.end() && state->pressed;
    if (current == pressed) {
      return rejectCapture(diagnostic,
                           pressed ? "Duplicate replay input press"
                                   : "Unmatched replay input release");
    }
    if (transitions_.size() >= limits_.maxInputTransitions) {
      return rejectCapture(diagnostic,
                           "Replay input transition limit exceeded");
    }

    transitions_.push_back({.songTimeMicros = songTimeMicros,
                            .control = control,
                            .pressed = pressed,
                            .replayOnly = replayOnly});
    if (state == states_.end()) {
      states_.push_back({.control = control, .pressed = pressed});
    } else {
      state->pressed = pressed;
    }
    lastSongTimeMicros_ = songTimeMicros;
    diagnostic.clear();
    return true;
  } catch (const std::exception &error) {
    return rejectCapture(
        diagnostic,
        std::string("Could not record replay input: ") + error.what());
  } catch (...) {
    return rejectCapture(diagnostic, "Could not record replay input");
  }
}

std::optional<std::vector<InputTransition>>
ReplayInputRecorder::finish(ReplayTimeBounds bounds,
                            std::string &diagnostic) noexcept {
  if (finished_) {
    diagnostic = "Replay input recording has already finished";
    return std::nullopt;
  }
  finished_ = true;
  if (!failureDiagnostic_.empty()) {
    diagnostic = failureDiagnostic_;
    transitions_.clear();
    return std::nullopt;
  }
  if (!bounds.valid() ||
      std::ranges::any_of(transitions_, [&](const auto &transition) {
        return !bounds.contains(transition.songTimeMicros, limits_);
      })) {
    diagnostic = "Replay input is outside the completion bounds";
    transitions_.clear();
    return std::nullopt;
  }
  diagnostic.clear();
  return std::move(transitions_);
}

} // namespace replay
