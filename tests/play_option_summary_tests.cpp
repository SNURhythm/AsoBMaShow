#include "ResultReplayLanePattern.h"
#include "PlayOptionUtils.h"
#include "support/AllocationLifetimeProbe.h"

#include <cassert>
#include <iostream>

void testLaneOrders() {
  bms_parser::ChartMeta single;
  single.KeyMode = 7;
  const auto mirrored = play_options::laneOrderForPlayOption(single, "MIRROR", {}, 0);
  assert(mirrored == std::vector<int>({7, 6, 5, 4, 3, 2, 1, 0}));
  assert(play_options::laneOrderForPlayOption(single, "NORMAL", {}, 0) ==
         std::vector<int>({7, 0, 1, 2, 3, 4, 5, 6}));
  assert(play_options::laneOrderForPlayOption(single, "unknown", {}, 0) ==
         std::vector<int>({7, 0, 1, 2, 3, 4, 5, 6}));
  assert(!play_options::laneOrderForPlayOption(single, "RANDOM", {}, 0));
  const auto random = play_options::laneOrderForPlayOption(single, "RANDOM", 123, 0);
  assert(random && random->front() == 7);
  assert(random == play_options::laneOrderForPlayOption(single, "RANDOM", 123, 0));

  bms_parser::ChartMeta doublePlay;
  doublePlay.KeyMode = 14;
  doublePlay.IsDP = true;
  assert(play_options::laneOrderForPlayOption(doublePlay, "MIRROR", {}, 0) ==
         std::vector<int>({7, 6, 5, 4, 3, 2, 1, 0, 8, 9, 10, 11, 12, 13, 14, 15}));
  assert(play_options::laneOrderForPlayOption(doublePlay, "MIRROR", {}, 1) ==
         std::vector<int>({7, 0, 1, 2, 3, 4, 5, 6, 14, 13, 12, 11, 10, 9, 8, 15}));

  bms_parser::ChartMeta empty;
  empty.KeyMode = 0;
  assert(!play_options::laneOrderForPlayOption(empty, "MIRROR", {}, 0));
}

void testSyntheticChartOwnership() {
  for (int player : {0, 1}) {
    bms_parser::ChartMeta meta;
    meta.KeyMode = 14;
    meta.IsDP = true;
    const std::optional<std::string> option = "MIRROR";
    const auto allocations = test_support::checkAllocationFailures([&] {
      const auto order = play_options::laneOrderForPlayOption(meta, option, {}, player);
      assert(order && order->size() == 16);
    });
    std::cout << "Lane order player " << player << ": " << allocations
              << " allocation failures passed\n";
  }
}

int main() {
  for (int keyMode : {7, 14}) {
    bms_parser::ChartMeta meta;
    meta.KeyMode = keyMode;
    meta.IsDP = keyMode == 14;
    for (int player = 0; player < (meta.IsDP ? 2 : 1); ++player) {
      for (const std::string option : {"RANDOM", "R-RANDOM", "RANDOM-EX"}) {
        const auto pattern = resultReplayLanePattern(meta, option, 123, player);
        assert(pattern && pattern->size() == 8);
        assert(pattern == resultReplayLanePattern(meta, option, 123, player));
        auto sorted = *pattern;
        std::ranges::sort(sorted);
        for (int index = 0; index < 8; ++index)
          assert(sorted[index] == index + player * 7);
      }
    }
  }

  testLaneOrders();
  testSyntheticChartOwnership();
  testLaneOrders();
}
