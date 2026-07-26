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

replay::LogicalControl scratch(replay::LogicalControlKind kind,
                               int player = 1) {
  return {.kind = kind, .player = player, .lane = -1};
}

void testRecordsMappedOrderedEdgesAndFinishesOnce() {
  ClockFixture clock{.offset = -1'000};
  replay::ReplayInputRecorder recorder(
      {.context = &clock, .mapSteadyToSong = &ClockFixture::map});
  std::string diagnostic;

  require(recorder.record(1'100, lane(), true, diagnostic),
          "first mapped press is accepted");
  require(recorder.record(1'200, lane(), false, diagnostic),
          "matching mapped release is accepted");
  require(recorder.recordSongTime(
              300,
              {.kind = replay::LogicalControlKind::ScratchCounterClockwise,
               .player = 1,
               .lane = -1},
              true, diagnostic),
          "direct song-time scratch input is accepted");

  const auto result = recorder.finish(diagnostic);
  require(diagnostic.empty(), "successful finish has no diagnostic");
  require(
      result.has_value() && *result ==
          std::vector<replay::InputTransition>{
              {.songTimeMicros = 100, .control = lane(), .pressed = true},
              {.songTimeMicros = 200, .control = lane(), .pressed = false},
              {.songTimeMicros = 300,
               .control =
                   {.kind = replay::LogicalControlKind::ScratchCounterClockwise,
                    .player = 1,
                    .lane = -1},
               .pressed = true}},
      "recorder returns the accepted input stream in song-time order");

  require(!recorder.recordSongTime(400, lane(1), true, diagnostic),
          "recording after finish is rejected");
  require(!recorder.finish(diagnostic).has_value(),
          "a second finish cannot expose mutable data");
}

void testRejectsUnavailableClockInvalidOrderAndPreRoll() {
  std::string diagnostic;
  replay::ReplayInputRecorder noClock({});
  require(!noClock.record(1, lane(), true, diagnostic) &&
              diagnostic.find("clock") != std::string::npos,
          "missing clock is rejected");

  ClockFixture clock;
  replay::ReplayInputRecorder unavailable(
      {.context = &clock, .mapSteadyToSong = &ClockFixture::map});
  clock.available = false;
  require(!unavailable.record(1, lane(), true, diagnostic),
          "an unavailable clock sample is rejected");

  replay::ReplayInputRecorder recorder(
      {.context = &clock, .mapSteadyToSong = &ClockFixture::map},
      {.minimumSongTimeMicros = -100, .maximumTransitions = 2});
  clock.available = true;
  require(!recorder.recordSongTime(-101, lane(), true, diagnostic),
          "input before the configured pre-roll is rejected");
  require(recorder.recordSongTime(20, lane(), true, diagnostic),
          "valid input after a rejected sample is accepted");
  require(!recorder.recordSongTime(19, lane(), false, diagnostic),
          "decreasing song time is rejected");
  require(recorder.recordSongTime(20, lane(), false, diagnostic),
          "equal song times preserve source sequence");
  require(!recorder.recordSongTime(21, lane(1), true, diagnostic),
          "transition capacity is enforced");
  require(!recorder.finish(diagnostic).has_value(),
          "nonredundant rejection invalidates the accepted replay prefix");
}

void testCapacityOverflowInvalidatesAcceptedPrefix() {
  ClockFixture clock;
  replay::ReplayInputRecorder recorder(
      {.context = &clock, .mapSteadyToSong = &ClockFixture::map},
      {.minimumSongTimeMicros = -100, .maximumTransitions = 2});
  std::string diagnostic;

  require(recorder.recordSongTime(0, lane(), true, diagnostic),
          "capacity fixture accepts the first edge");
  require(recorder.recordSongTime(1, lane(), false, diagnostic),
          "capacity fixture accepts the second edge");
  require(!recorder.recordSongTime(2, lane(1), true, diagnostic),
          "capacity fixture rejects the excess effective edge");
  require(!recorder.finish(diagnostic).has_value() &&
              diagnostic.find("limit") != std::string::npos,
          "capacity overflow cannot expose a truncated replay prefix");
}

void testRejectsInvalidControlsAndDuplicateState() {
  ClockFixture clock;
  replay::ReplayInputRecorder recorder(
      {.context = &clock, .mapSteadyToSong = &ClockFixture::map});
  std::string diagnostic;

  require(
      !recorder.recordSongTime(
          0, {.kind = replay::LogicalControlKind::Lane, .player = 3, .lane = 0},
          true, diagnostic),
      "unknown players are rejected");
  require(!recorder.recordSongTime(0, lane(-1), true, diagnostic),
          "negative lane controls are rejected");
  require(!recorder.recordSongTime(
              0,
              {.kind = replay::LogicalControlKind::ScratchClockwise,
               .player = 1,
               .lane = 0},
              true, diagnostic),
          "scratch controls cannot carry a lane index");
  require(!recorder.recordSongTime(0, lane(), false, diagnostic),
          "an unmatched release is rejected");
  require(recorder.recordSongTime(0, lane(), true, diagnostic),
          "initial press is accepted");
  require(!recorder.recordSongTime(1, lane(), true, diagnostic),
          "duplicate state is rejected");
  require(recorder.recordSongTime(2, lane(), false, diagnostic),
          "state-changing release is accepted");
  require(!recorder.finish(diagnostic).has_value(),
          "invalid controls make the completed capture unusable");
}

void testRedundantStateDoesNotInvalidateCapture() {
  ClockFixture clock;
  replay::ReplayInputRecorder recorder(
      {.context = &clock, .mapSteadyToSong = &ClockFixture::map},
      {.minimumSongTimeMicros = -100, .maximumTransitions = 2});
  std::string diagnostic;

  require(!recorder.recordSongTime(0, lane(), false, diagnostic),
          "redundant unmatched release is ignored");
  require(recorder.recordSongTime(1, lane(), true, diagnostic),
          "effective press is accepted");
  require(!recorder.recordSongTime(2, lane(), true, diagnostic),
          "redundant duplicate press is ignored");
  require(recorder.recordSongTime(3, lane(), false, diagnostic),
          "effective release is accepted");
  require(!recorder.recordSongTime(4, lane(), false, diagnostic),
          "redundant release remains redundant at transition capacity");

  const auto result = recorder.finish(diagnostic);
  require(result.has_value() && result->size() == 2 && diagnostic.empty(),
          "redundant state samples preserve a valid replay capture");
}

void testCaptureBufferSortsAcrossControlsButRejectsPerControlReversal() {
  std::string diagnostic;
  replay::ReplayInputCaptureBuffer accepted(
      {.minimumSongTimeMicros = -100, .maximumTransitions = 8});
  require(accepted.capture(
              {.songTimeMicros = 200, .control = lane(0), .pressed = true},
              diagnostic) &&
              accepted.capture({.songTimeMicros = 100,
                                .control = lane(1),
                                .pressed = true},
                               diagnostic),
          "different controls may arrive out of global song-time order");
  const auto sorted = accepted.finish(diagnostic);
  require(sorted && sorted->size() == 2 &&
              (*sorted)[0].control == lane(1) &&
              (*sorted)[1].control == lane(0),
          "capture buffer produces a stable global song-time order");

  replay::ReplayInputCaptureBuffer reversed(
      {.minimumSongTimeMicros = -100, .maximumTransitions = 8});
  require(reversed.capture(
              {.songTimeMicros = 200, .control = lane(), .pressed = true},
              diagnostic),
          "per-control reversal fixture accepts its first edge");
  require(!reversed.capture(
              {.songTimeMicros = 100, .control = lane(), .pressed = false},
              diagnostic) &&
              !reversed.finish(diagnostic).has_value() &&
              diagnostic.find("control") != std::string::npos,
          "sorting cannot hide a decreasing timestamp for one control");
}

void testCaptureBufferBoundsPendingSamplesAndPropagatesClockFailure() {
  std::string diagnostic;
  replay::ReplayInputCaptureBuffer bounded(
      {.minimumSongTimeMicros = -100, .maximumTransitions = 2});
  require(bounded.capture(
              {.songTimeMicros = 0, .control = lane(), .pressed = false},
              diagnostic) &&
              bounded.capture(
                  {.songTimeMicros = 1, .control = lane(), .pressed = false},
                  diagnostic),
          "pending capture counts even redundant source samples");
  require(!bounded.capture(
              {.songTimeMicros = 2, .control = lane(), .pressed = false},
              diagnostic) &&
              !bounded.finish(diagnostic).has_value() &&
              diagnostic.find("limit") != std::string::npos,
          "pending replay input is bounded before finish-time materialization");

  replay::ReplayInputCaptureBuffer failed;
  failed.fail("Replay input clock could not map the timestamp");
  require(!failed.finish(diagnostic).has_value() &&
              diagnostic.find("clock") != std::string::npos,
          "an unmappable gameplay timestamp invalidates replay capture");
}

void testCaptureBufferFiltersRedundantStateAtFinish() {
  std::string diagnostic;
  replay::ReplayInputCaptureBuffer buffered;
  require(buffered.capture(
              {.songTimeMicros = 0, .control = lane(), .pressed = false},
              diagnostic) &&
              buffered.capture(
                  {.songTimeMicros = 1, .control = lane(), .pressed = true},
                  diagnostic) &&
              buffered.capture(
                  {.songTimeMicros = 2, .control = lane(), .pressed = true},
                  diagnostic) &&
              buffered.capture(
                  {.songTimeMicros = 3, .control = lane(), .pressed = false},
                  diagnostic),
          "capture buffer accepts noisy ordered device samples");

  const auto result = buffered.finish(diagnostic);
  require(result.has_value() && result->size() == 2 && (*result)[0].pressed &&
              !(*result)[1].pressed && diagnostic.empty(),
          "capture buffer filters redundant state without invalidating replay");
}

void testCaptureBufferPreservesOnlyValidReplayOnlyScratchHandoffs() {
  std::string diagnostic;
  replay::ReplayInputCaptureBuffer valid;
  require(
      valid.capture({.songTimeMicros = 100,
                     .control = scratch(
                         replay::LogicalControlKind::ScratchCounterClockwise),
                     .pressed = true},
                    diagnostic) &&
          valid.capture(
              {.songTimeMicros = 200,
               .control =
                   scratch(replay::LogicalControlKind::ScratchCounterClockwise),
               .pressed = false,
               .replayOnly = true},
              diagnostic) &&
          valid.capture(
              {.songTimeMicros = 200, .control = lane(2), .pressed = true},
              diagnostic) &&
          valid.capture(
              {.songTimeMicros = 200,
               .control = scratch(replay::LogicalControlKind::ScratchClockwise),
               .pressed = true,
               .replayOnly = true},
              diagnostic) &&
          valid.capture(
              {.songTimeMicros = 250, .control = lane(2), .pressed = false},
              diagnostic) &&
          valid.capture(
              {.songTimeMicros = 300,
               .control = scratch(replay::LogicalControlKind::ScratchClockwise),
               .pressed = false},
              diagnostic),
      "capture accepts a complete logical-only held-scratch handoff");
  const auto result = valid.finish(diagnostic);
  require(result && result->size() == 6 && (*result)[1].replayOnly &&
              (*result)[3].replayOnly && !(*result)[0].replayOnly &&
              !(*result)[2].replayOnly && !(*result)[4].replayOnly &&
              !(*result)[5].replayOnly,
          "finish preserves only the two logical-only handoff markers");

  replay::ReplayInputCaptureBuffer arbitraryLane;
  require(arbitraryLane.capture(
              {.songTimeMicros = 100, .control = lane(), .pressed = true},
              diagnostic) &&
              arbitraryLane.capture({.songTimeMicros = 200,
                                     .control = lane(),
                                     .pressed = false,
                                     .replayOnly = true},
                                    diagnostic) &&
              arbitraryLane.capture({.songTimeMicros = 200,
                                     .control = lane(),
                                     .pressed = true,
                                     .replayOnly = true},
                                    diagnostic),
          "buffering does not need to guess at an incomplete marker pair");
  require(!arbitraryLane.finish(diagnostic).has_value() &&
              diagnostic.find("scratch") != std::string::npos,
          "finish rejects replay-only markers on arbitrary lane input");
}

} // namespace

int main() {
  testRecordsMappedOrderedEdgesAndFinishesOnce();
  testRejectsUnavailableClockInvalidOrderAndPreRoll();
  testCapacityOverflowInvalidatesAcceptedPrefix();
  testRejectsInvalidControlsAndDuplicateState();
  testRedundantStateDoesNotInvalidateCapture();
  testCaptureBufferSortsAcrossControlsButRejectsPerControlReversal();
  testCaptureBufferBoundsPendingSamplesAndPropagatesClockFailure();
  testCaptureBufferFiltersRedundantStateAtFinish();
  testCaptureBufferPreservesOnlyValidReplayOnlyScratchHandoffs();
  return 0;
}
