#include "replay/ReplayInputRecorder.h"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

struct ClockFixture {
  std::int64_t offset = 0;
  bool available = true;

  static std::optional<std::int64_t> map(void *context,
                                         std::int64_t steadyMicros) {
    auto &clock = *static_cast<ClockFixture *>(context);
    if (!clock.available) {
      return std::nullopt;
    }
    return steadyMicros + clock.offset;
  }
};

replay::LogicalControl lane(int laneIndex = 0, int player = 1) {
  return {.kind = replay::LogicalControlKind::Lane,
          .player = player,
          .lane = laneIndex};
}

void testRecordsSignedMonotonicSongTimeWithoutSorting() {
  ClockFixture clock{.offset = -2'000};
  replay::ReplayInputRecorder recorder(
      {.context = &clock, .mapSteadyToSong = &ClockFixture::map});
  std::string diagnostic;

  require(recorder.record(1'000, lane(), true, diagnostic),
          "mapped pre-roll press is accepted");
  require(recorder.recordSongTime(-500, lane(), false, diagnostic),
          "direct signed-time release is accepted");
  require(recorder.recordSongTime(
              0,
              {.kind = replay::LogicalControlKind::Start,
               .player = 1,
               .lane = -1},
              true, diagnostic),
          "stock Start input is accepted");

  const auto result = recorder.finish(
      {.completionSongTimeMicros = 5'000}, diagnostic);
  require(result.has_value() && diagnostic.empty(),
          "valid capture finishes exactly once");
  require(*result ==
              std::vector<replay::InputTransition>{
                  {.songTimeMicros = -1'000,
                   .control = lane(),
                   .pressed = true},
                  {.songTimeMicros = -500,
                   .control = lane(),
                   .pressed = false},
                  {.songTimeMicros = 0,
                   .control = {.kind = replay::LogicalControlKind::Start,
                               .player = 1,
                               .lane = -1},
                   .pressed = true}},
          "capture preserves accepted source order and signed timestamps");
  require(!recorder.finish({.completionSongTimeMicros = 5'000}, diagnostic)
               .has_value(),
          "finished capture cannot be read twice");
}

void testOutOfOrderAndRedundantObserverEdgesAreNormalized() {
  replay::ReplayInputRecorder recorder;
  std::string diagnostic;

  require(recorder.recordSongTime(10, lane(), true, diagnostic),
          "initial press is observed");
  require(recorder.recordSongTime(9, lane(), true, diagnostic),
          "out-of-order duplicate press remains recoverable");
  require(recorder.recordSongTime(8, lane(1), false, diagnostic),
          "unmatched release remains recoverable");
  require(recorder.recordSongTime(11, lane(), false, diagnostic),
          "matching release is observed");

  const auto result = recorder.finish(
      {.completionSongTimeMicros = 20}, diagnostic);
  require(result.has_value() && diagnostic.empty(),
          "observer noise does not discard the attachment");
  require(*result ==
              std::vector<replay::InputTransition>{
                  {.songTimeMicros = 9,
                   .control = lane(),
                   .pressed = true},
                  {.songTimeMicros = 11,
                   .control = lane(),
                   .pressed = false}},
          "input is stable-ordered and redundant states are removed");
}

void testOverflowInvalidatesTheWholeAttachment() {
  replay::ReplayLimits bounded = replay::kReplayLimits;
  bounded.maxInputTransitions = 2;
  std::string diagnostic;

  replay::ReplayInputRecorder overflow({}, bounded);
  require(overflow.recordSongTime(0, lane(), true, diagnostic) &&
              overflow.recordSongTime(1, lane(), false, diagnostic),
          "overflow fixture fills its exact capacity");
  require(!overflow.recordSongTime(2, lane(1), true, diagnostic),
          "one excess transition is rejected");
  require(!overflow.finish({.completionSongTimeMicros = 10}, diagnostic)
               .has_value() &&
              diagnostic.find("limit") != std::string::npos,
          "capacity failure never saves a truncated replay");
}

void testClockFailureAndCompletionObservation() {
  std::string diagnostic;
  replay::ReplayInputRecorder noClock;
  require(!noClock.record(1, lane(), true, diagnostic) &&
              diagnostic.find("clock") != std::string::npos,
          "steady-time recording requires an explicit song clock");
  require(!noClock.finish({.completionSongTimeMicros = 10}, diagnostic)
               .has_value(),
          "clock failure permanently invalidates the attachment");

  replay::ReplayInputRecorder tooLate;
  require(tooLate.recordSongTime(11, lane(), true, diagnostic),
          "input after the observed cursor is accepted");
  const auto afterCursor =
      tooLate.finish({.completionSongTimeMicros = 10}, diagnostic);
  require(afterCursor.has_value() && afterCursor->size() == 1 &&
              afterCursor->front().songTimeMicros == 11 && diagnostic.empty(),
          "accepted input extends the capture completion observation");

  replay::ReplayInputRecorder missingCompletion;
  require(missingCompletion.recordSongTime(11, lane(), true, diagnostic),
          "missing-completion fixture records input");
  require(!missingCompletion.finish({}, diagnostic).has_value() &&
              diagnostic.find("bounds") != std::string::npos,
          "input cannot invent a missing completion observation");
}

void testUnsupportedControlsFailClosed() {
  std::string diagnostic;
  replay::ReplayInputRecorder unsupported;
  require(!unsupported.recordSongTime(
              0,
              {.kind = replay::LogicalControlKind::Lane,
               .player = 3,
               .lane = 0},
              true, diagnostic),
          "unknown replay players are rejected");
  require(!unsupported.finish({.completionSongTimeMicros = 10}, diagnostic)
               .has_value(),
          "unsupported controls invalidate capture");
}

} // namespace

int main() {
  testRecordsSignedMonotonicSongTimeWithoutSorting();
  testOutOfOrderAndRedundantObserverEdgesAreNormalized();
  testOverflowInvalidatesTheWholeAttachment();
  testClockFailureAndCompletionObservation();
  testUnsupportedControlsFailClosed();
  return 0;
}
