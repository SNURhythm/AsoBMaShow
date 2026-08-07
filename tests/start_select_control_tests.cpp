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

} // namespace

int main() {
  testStartAndSelectUseBeatorajaKeyBindings();
  testStartDoublePressAndConjunctionMatchBeatorajaEdges();
  testStartAndSelectAtNoteEndExitImmediately();
  testHeldSpecialKeysRepeatLikeBeatorajaScratchBindings();
  return 0;
}
