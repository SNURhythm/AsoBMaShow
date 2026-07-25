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
      result ==
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
  require(recorder.finish(diagnostic).empty(),
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
}

} // namespace

int main() {
  testRecordsMappedOrderedEdgesAndFinishesOnce();
  testRejectsUnavailableClockInvalidOrderAndPreRoll();
  testRejectsInvalidControlsAndDuplicateState();
  return 0;
}
