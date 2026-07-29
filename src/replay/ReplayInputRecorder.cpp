#include "ReplayInputRecorder.h"

#include "ReplayKeyMode.h"

#include <algorithm>
#include <exception>
#include <ranges>
#include <span>
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

std::optional<std::vector<InputTransition>>
normalizeReplayInput(std::span<const InputTransition> input,
                     ReplayTimeBounds bounds, std::string &diagnostic,
                     const ReplayLimits &limits) noexcept {
  diagnostic.clear();
  try {
    if (!limits.valid() || !bounds.valid() ||
        input.size() > limits.maxInputTransitions) {
      diagnostic = "Replay input limits or completion bounds are invalid";
      return std::nullopt;
    }
    const ReplayTimeBounds captureBounds =
        replayCaptureTimeBounds(bounds, input, {}, {});

    std::vector<InputTransition> ordered(input.begin(), input.end());
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto &left,
                                                        const auto &right) {
      return left.songTimeMicros < right.songTimeMicros;
    });

    struct ControlState {
      LogicalControl control;
      bool pressed = false;
    };
    std::vector<ControlState> states;
    std::vector<InputTransition> normalized;
    states.reserve(64);
    normalized.reserve(ordered.size());
    for (const auto &transition : ordered) {
      if (!structurallyValidControl(transition.control)) {
        diagnostic = "Replay input control is unsupported";
        return std::nullopt;
      }
      if (!captureBounds.contains(transition.songTimeMicros, limits)) {
        diagnostic = "Replay input is outside the completion bounds";
        return std::nullopt;
      }
      const auto state =
          std::ranges::find(states, transition.control, &ControlState::control);
      const bool current = state != states.end() && state->pressed;
      if (current == transition.pressed) {
        continue;
      }
      normalized.push_back(transition);
      if (state == states.end()) {
        states.push_back(
            {.control = transition.control, .pressed = transition.pressed});
      } else {
        state->pressed = transition.pressed;
      }
    }
    return normalized;
  } catch (const std::exception &error) {
    diagnostic = std::string("Could not normalize replay input: ") +
                 error.what();
    return std::nullopt;
  } catch (...) {
    diagnostic = "Could not normalize replay input";
    return std::nullopt;
  }
}

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
    if (transitions_.size() >= limits_.maxInputTransitions) {
      return rejectCapture(diagnostic,
                           "Replay input transition limit exceeded");
    }

    transitions_.push_back({.songTimeMicros = songTimeMicros,
                            .control = control,
                            .pressed = pressed,
                            .replayOnly = replayOnly});
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
  auto normalized = normalizeReplayInput(transitions_, bounds, diagnostic,
                                         limits_);
  transitions_.clear();
  return normalized;
}

} // namespace replay
