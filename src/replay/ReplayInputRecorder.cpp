#include "ReplayInputRecorder.h"
#include "ReplayInputValidation.h"

#include <algorithm>
#include <exception>
#include <utility>

namespace replay {
namespace {

bool validControl(const LogicalControl &control) noexcept {
  if (control.player != 1 && control.player != 2) {
    return false;
  }
  switch (control.kind) {
  case LogicalControlKind::Lane:
    return control.lane >= 0 && control.lane < 26;
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
                                         ReplayInputRecorderLimits limits)
    : clock_(clock), limits_(limits) {
  transitions_.reserve(std::min<std::size_t>(limits.maximumTransitions, 4096));
}

bool ReplayInputRecorder::rejectCapture(std::string &diagnostic,
                                        std::string message) {
  diagnostic = std::move(message);
  if (failureDiagnostic_.empty()) {
    failureDiagnostic_ = diagnostic;
  }
  return false;
}

bool ReplayInputRecorder::record(std::int64_t steadyTimestampMicros,
                                 LogicalControl control, bool pressed,
                                 std::string &diagnostic,
                                 bool replayOnly) noexcept {
  try {
    if (finished_) {
      diagnostic = "Replay input recording has already finished";
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
  } catch (const std::exception &error) {
    return rejectCapture(
        diagnostic, std::string("Replay input clock failed: ") + error.what());
  } catch (...) {
    return rejectCapture(diagnostic, "Replay input clock failed");
  }
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
    if (!validControl(control)) {
      return rejectCapture(diagnostic, "Replay input control is invalid");
    }
    if (songTimeMicros < limits_.minimumSongTimeMicros) {
      return rejectCapture(diagnostic,
                           "Replay input is outside the supported pre-roll");
    }
    if (lastSongTimeMicros_.has_value() &&
        songTimeMicros < *lastSongTimeMicros_) {
      return rejectCapture(diagnostic,
                           "Replay input timestamps must not decrease");
    }
    const auto state =
        std::ranges::find(states_, control, &ControlState::control);
    const bool current = state != states_.end() && state->pressed;
    if (current == pressed) {
      diagnostic = pressed ? "Duplicate replay input press"
                           : "Unmatched replay input release";
      return false;
    }
    if (transitions_.size() >= limits_.maximumTransitions) {
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
ReplayInputRecorder::finish(std::string &diagnostic) noexcept {
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
  if (!validateReplayOnlyScratchHandoffs(transitions_, diagnostic)) {
    transitions_.clear();
    return std::nullopt;
  }
  diagnostic.clear();
  return std::move(transitions_);
}

ReplayInputCaptureBuffer::ReplayInputCaptureBuffer(
    ReplayInputRecorderLimits limits)
    : limits_(limits) {
  pending_.reserve(std::min<std::size_t>(limits.maximumTransitions, 4096));
}

void ReplayInputCaptureBuffer::fail(std::string diagnostic) noexcept {
  if (!failureDiagnostic_.empty()) {
    return;
  }
  try {
    failureDiagnostic_ = diagnostic.empty()
                             ? "Replay input capture failed"
                             : std::move(diagnostic);
  } catch (...) {
    failureDiagnostic_ = "Replay input capture failed";
  }
}

bool ReplayInputCaptureBuffer::capture(InputTransition transition,
                                       std::string &diagnostic) noexcept {
  try {
    if (finished_) {
      diagnostic = "Replay input capture has already finished";
      return false;
    }
    if (!failureDiagnostic_.empty()) {
      diagnostic = failureDiagnostic_;
      return false;
    }
    if (!validControl(transition.control)) {
      diagnostic = "Replay input control is invalid";
      fail(diagnostic);
      return false;
    }
    if (transition.songTimeMicros < limits_.minimumSongTimeMicros) {
      diagnostic = "Replay input is outside the supported pre-roll";
      fail(diagnostic);
      return false;
    }
    const auto timestamp = std::ranges::find(
        controlTimestamps_, transition.control, &ControlTimestamp::control);
    if (timestamp != controlTimestamps_.end() &&
        transition.songTimeMicros < timestamp->songTimeMicros) {
      diagnostic = "Replay input timestamp decreased for one control";
      fail(diagnostic);
      return false;
    }
    if (pending_.size() >= limits_.maximumTransitions) {
      diagnostic = "Replay input pending sample limit exceeded";
      fail(diagnostic);
      return false;
    }
    pending_.push_back(std::move(transition));
    if (timestamp == controlTimestamps_.end()) {
      controlTimestamps_.push_back(
          {.control = pending_.back().control,
           .songTimeMicros = pending_.back().songTimeMicros});
    } else {
      timestamp->songTimeMicros = pending_.back().songTimeMicros;
    }
    diagnostic.clear();
    return true;
  } catch (const std::exception &error) {
    diagnostic = std::string("Could not buffer replay input: ") + error.what();
    fail(diagnostic);
    return false;
  } catch (...) {
    diagnostic = "Could not buffer replay input";
    fail(diagnostic);
    return false;
  }
}

std::optional<std::vector<InputTransition>>
ReplayInputCaptureBuffer::finish(std::string &diagnostic) noexcept {
  try {
    if (finished_) {
      diagnostic = "Replay input capture has already finished";
      return std::nullopt;
    }
    finished_ = true;
    if (!failureDiagnostic_.empty()) {
      diagnostic = failureDiagnostic_;
      pending_.clear();
      return std::nullopt;
    }
    std::stable_sort(pending_.begin(), pending_.end(),
                     [](const auto &left, const auto &right) {
                       return left.songTimeMicros < right.songTimeMicros;
                     });
    ReplayInputRecorder recorder({}, limits_);
    for (const auto &transition : pending_) {
      // Redundant device samples are benign false returns. Fatal recorder
      // failures remain sticky and are reported by finish().
      (void)recorder.recordSongTime(transition.songTimeMicros,
                                    transition.control, transition.pressed,
                                    diagnostic, transition.replayOnly);
    }
    auto result = recorder.finish(diagnostic);
    if (!result) {
      pending_.clear();
    }
    return result;
  } catch (const std::exception &error) {
    diagnostic =
        std::string("Could not finish buffered replay input: ") + error.what();
  } catch (...) {
    diagnostic = "Could not finish buffered replay input";
  }
  pending_.clear();
  return std::nullopt;
}

} // namespace replay
