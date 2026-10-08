#include "scene/play/StartSelectControl.h"

#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

void require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

using Action = gameplay::StartSelectControlAction;
using ActionKind = gameplay::StartSelectControlActionKind;
using Control = replay::LogicalControl;
using ControlKind = replay::LogicalControlKind;

Control start() {
  return {.kind = ControlKind::Start, .player = 1, .lane = -1};
}

Control select() {
  return {.kind = ControlKind::Select, .player = 1, .lane = -1};
}

Control lane(int lane) {
  return {.kind = ControlKind::Lane, .player = 1, .lane = lane};
}

Control scratchClockwise() {
  return {.kind = ControlKind::ScratchClockwise, .player = 1, .lane = 7};
}

Control scratchCounterClockwise() {
  return {.kind = ControlKind::ScratchCounterClockwise, .player = 1, .lane = 7};
}

void testStartAndSelectUseBeatorajaKeyBindings() {
  gameplay::StartSelectControl control({.keyMode = 7});

  require(control.apply(start(), true, 1'000).empty(),
          "Start's first edge only arms its held-key control mode");
  require(control.apply(lane(1), true, 1'001) ==
              std::vector<Action>{{.kind = ActionKind::AdjustHispeed,
                                   .delta = 1}},
          "Start plus the second 7-key input raises hi-speed");
  require(control.apply(lane(1), false, 1'002).empty(),
          "a key release does not change hi-speed");
  require(control.apply(lane(0), true, 1'003) ==
              std::vector<Action>{{.kind = ActionKind::AdjustHispeed,
                                   .delta = -1}},
          "Start plus the first 7-key input lowers hi-speed");
  require(control.apply(start(), false, 1'004).empty(),
          "releasing Start ends hi-speed control");

  require(control.apply(select(), true, 2'000).empty(),
          "Select's first edge only arms its held-key control mode");
  require(control.apply(lane(1), true, 2'001) ==
              std::vector<Action>{{.kind = ActionKind::AdjustDuration,
                                   .delta = 1}},
          "Select plus the second 7-key input raises green number");
  require(control.apply(lane(0), true, 2'002) ==
              std::vector<Action>{{.kind = ActionKind::AdjustDuration,
                                   .delta = -1}},
          "Select plus the first 7-key input lowers green number");
}

void testEvenKeyModesUseSkinIndependentLaneRoles() {
  struct Case {
    int mode;
    std::vector<int> lanes;
    std::vector<int> deltas;
  };
  for (const auto &example : {
           Case{4, {0, 1, 3, 4}, {-1, 1, 1, -1}},
           Case{6, {0, 1, 2, 4, 5, 6}, {-1, 1, -1, -1, 1, -1}},
           Case{8, {7, 0, 1, 2, 3, 4, 5, 6}, {-1, 1, -1, 1, 1, -1, 1, -1}}}) {
    for (const auto modifier : {start(), select()}) {
      gameplay::StartSelectControl control({.keyMode = example.mode});
      (void)control.apply(modifier, true, 1'000);
      for (std::size_t index = 0; index < example.lanes.size(); ++index) {
        const auto key = lane(example.lanes[index]);
        require(control.apply(key, true, 1'001) == std::vector<Action>{
                    {.kind = modifier.kind == ControlKind::Start
                         ? ActionKind::AdjustHispeed : ActionKind::AdjustDuration,
                     .delta = example.deltas[index]}},
                "4K/6K/8K canonical white-role keys decrease and blue-role keys increase");
        require(control.apply(key, false, 1'002).empty(),
                "key release does not repeat a speed adjustment");
      }
      require(control.apply(lane(-1), true, 1'003).empty() &&
                  control.apply(lane(8), true, 1'003).empty(),
              "invalid lanes never adjust speed");
      if (example.mode != 8) {
        require(control.apply(lane(example.mode == 4 ? 2 : 3), true, 1'004).empty(),
                "omitted sparse lanes have no speed command");
      }
      (void)control.apply(modifier, false, 1'005);
      require(control.apply(lane(example.lanes.front()), true, 1'006).empty(),
              "releasing the modifier ends speed control");
    }
  }
}

void testLegacyAndSingleKeyboardCommandMappingsRemainUnchanged() {
  for (const int mode : {5, 7, 10, 14, 24}) {
    const int players = mode == 10 || mode == 14 ? 2 : 1;
    const int keys = mode == 10 ? 5 : mode == 14 ? 7 : mode;
    constexpr std::array keyboardDeltas{
        -1, 1, -1, 1, -1, -1, 1, -1, 1, -1, 1, -1,
        -1, 1, -1, 1, -1, -1, 1, -1, 1, -1, 1, -1};
    for (int player = 1; player <= players; ++player) {
      for (const auto modifier : {start(), select()}) {
        gameplay::StartSelectControl control({.keyMode = mode});
        (void)control.apply(modifier, true, 1'000);
        for (int position = 0; position < keys; ++position) {
          const Control key{.kind = ControlKind::Lane, .player = player, .lane = position};
          require(control.apply(key, true, 1'001) == std::vector<Action>{{
                      .kind = modifier.kind == ControlKind::Start
                                  ? ActionKind::AdjustHispeed : ActionKind::AdjustDuration,
                      .delta = mode == 24 ? keyboardDeltas[position] : position % 2 == 0 ? -1 : 1}},
                  "legacy beat and 24K single command key roles remain unchanged");
          require(control.apply(key, false, 1'002).empty(),
                  "legacy command key releases emit no adjustment");
        }
      }
    }
  }
  for (const int specialLane : {24, 25}) {
    gameplay::StartSelectControl control({.keyMode = 24});
    (void)control.apply(start(), true, 100'000);
    require(control.apply(lane(specialLane), true, 100'001).empty(),
            "24K single extra controls retain their held behavior");
    require(control.tick(150'002) == std::vector<Action>{{
                .kind = ActionKind::AdjustLaneCover, .delta = specialLane == 24 ? -1 : 1}},
            "24K single extra controls retain their lane-cover direction");
  }
}

void testDenseDoublePlayReservedReplayLanesDoNotAdjustCommands() {
  for (const auto modifier : {start(), select()}) {
    gameplay::StartSelectControl control({.keyMode = 48});
    (void)control.apply(modifier, true, 100'000);
    // BRD's 26-wide player namespace includes channels beyond the 48 chart keys.
    for (int reserved = 22; reserved <= 25; ++reserved) {
      const Control key{.kind = ControlKind::Lane, .player = 2, .lane = reserved};
      require(control.apply(key, true, 100'001).empty() &&
                  control.tick(200'002).empty() &&
                  control.apply(key, false, 200'003).empty(),
              "48K reserved replay channels do not wrap into note or held commands");
    }
    for (const Control invalid : {
             Control{.kind = ControlKind::Lane, .player = 1, .lane = -1},
             Control{.kind = ControlKind::Lane, .player = 1, .lane = 26},
             Control{.kind = ControlKind::Lane, .player = 3, .lane = 0}}) {
      require(control.apply(invalid, true, 200'004).empty(),
              "invalid 48K replay lanes produce no command adjustment");
    }
  }
}

void testStartDoublePressAndConjunctionMatchBeatorajaEdges() {
  gameplay::StartSelectControl control({.keyMode = 7});
  require(control.apply(start(), true, 1'000).empty(),
          "the initial Start edge is not a double press");
  require(control.apply(start(), false, 1'100).empty(),
          "releasing Start keeps its double-press timer");
  require(control.apply(start(), true, 400'999) ==
              std::vector<Action>{{.kind = ActionKind::ToggleLaneCover}},
          "a second Start within 500 ms toggles lane cover");
  require(control.apply(select(), true, 401'000) ==
              std::vector<Action>{{.kind = ActionKind::ToggleLiftHiddenTarget}},
          "the first Start+Select conjunction switches the Lift/Hidden target");
  require(control.tick(1'400'999).empty(),
          "the conjunction waits for the configured exit hold duration");
  require(control.tick(1'401'001) ==
              std::vector<Action>{{.kind = ActionKind::Exit}},
          "holding Start+Select beyond the configured duration exits play");
  require(control.tick(1'500'000).empty(),
          "the same conjunction emits only one exit action");
}

void testStartAndSelectAtNoteEndExitImmediately() {
  gameplay::StartSelectControl control({.keyMode = 5});
  require(control.apply(start(), true, 1'000, {.noteEnd = true}) ==
              std::vector<Action>{{.kind = ActionKind::Exit}},
          "Start advances out of a completed chart immediately");
  require(control.apply(start(), false, 1'100).empty(),
          "releasing Start after note-end exit has no second effect");
  require(control.apply(select(), true, 1'200, {.noteEnd = true}) ==
              std::vector<Action>{{.kind = ActionKind::Exit}},
          "Select also advances out of a completed chart immediately");
  require(control.tick(1'300, {.noteEnd = true}).empty(),
          "a held post-chart control emits only one exit action");
}

void testHeldSpecialKeysRepeatLikeBeatorajaScratchBindings() {
  gameplay::StartSelectControl control({.keyMode = 9});
  require(control.apply(start(), true, 100'000).empty(),
          "Start arms the Pop'n held special-key controls");
  require(control.apply(lane(7), true, 100'001).empty(),
          "the held special key changes no value until the frame tick");
  require(control.tick(150'002) ==
              std::vector<Action>{{.kind = ActionKind::AdjustLaneCover,
                                   .delta = 1}},
          "START plus Pop'n's +2 key moves lane cover by the low repeat step");
  require(control.tick(651'003) ==
              std::vector<Action>{{.kind = ActionKind::AdjustLaneCover,
                                   .delta = 10}},
          "the same held control uses Beatoraja's fast repeat step after 500 ms");
}

void testAnalogScratchUsesTurntableTicksInsteadOfFrameRepeat() {
  gameplay::StartSelectControl control({.keyMode = 7});
  require(control.apply(start(), true, 100'000).empty(),
          "Start arms the analog scratch control path");
  require(control.apply(scratchClockwise(), true, 100'001, {}, true).empty(),
          "an analog scratch edge only records the physical scratch direction");
  require(control.tick(200'000).empty(),
          "a stationary analog scratch does not repeat from rendered frames");
  require(control.applyAnalogScratchTicks(scratchClockwise(), 3, 200'001) ==
              std::vector<Action>{{.kind = ActionKind::AdjustLaneCover,
                                   .delta = 3}},
          "Start plus analog scratch changes lane cover by completed turntable ticks");

  require(control.apply(start(), false, 200'002).empty() &&
              control.apply(select(), true, 200'003).empty(),
          "Select replaces Start without changing the analog scratch direction");
  require(control.applyAnalogScratchTicks(scratchClockwise(), 2, 200'004) ==
              std::vector<Action>{{.kind = ActionKind::AdjustDuration,
                                   .delta = 2}},
          "Select plus analog scratch changes green number by completed turntable ticks");
  require(control.applyAnalogScratchTicks(scratchCounterClockwise(), 2,
                                          200'005) ==
              std::vector<Action>{{.kind = ActionKind::AdjustDuration,
                                   .delta = -2}},
          "counter-clockwise angular ticks preserve Beatoraja's opposite adjustment direction");
}

void testResetDiscardsHeldAndTimedGestureState() {
  gameplay::StartSelectControl control({.keyMode = 7});
  require(control.apply(start(), true, 1'000).empty(),
          "an initial Start press arms the control before reset");
  require(control.apply(start(), false, 1'100).empty(),
          "the pre-reset Start release preserves the old double-press timer");
  control.reset();
  require(control.apply(start(), true, 400'000).empty(),
          "reset discards a stale Start double-press edge");
  require(control.apply(select(), true, 400'001) ==
              std::vector<Action>{{.kind = ActionKind::ToggleLiftHiddenTarget}},
          "fresh Start+Select state still begins a new conjunction after reset");
  require(control.tick(400'002).empty(),
          "reset prevents stale held scratch or exit actions");
}

} // namespace

int main() {
  testStartAndSelectUseBeatorajaKeyBindings();
  testEvenKeyModesUseSkinIndependentLaneRoles();
  testLegacyAndSingleKeyboardCommandMappingsRemainUnchanged();
  testDenseDoublePlayReservedReplayLanesDoNotAdjustCommands();
  testStartDoublePressAndConjunctionMatchBeatorajaEdges();
  testStartAndSelectAtNoteEndExitImmediately();
  testHeldSpecialKeysRepeatLikeBeatorajaScratchBindings();
  testAnalogScratchUsesTurntableTicksInsteadOfFrameRepeat();
  testResetDiscardsHeldAndTimedGestureState();
  return 0;
}
