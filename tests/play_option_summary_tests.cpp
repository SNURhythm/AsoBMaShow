#include "ResultReplayLanePattern.h"
#include "PlayOptionUtils.h"
#include "support/AllocationLifetimeProbe.h"

#include <cassert>
#include <iostream>
#include <atomic>
#include <memory>
#include <tuple>

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

void testDemotedNotesFollowModifiedActiveLane() {
  for (const std::string option : {"MIRROR", "RANDOM"}) {
    for (const bool detachHead : {false, true}) {
      const std::string input = std::string("#BPM 120\n#00051:0101\n#00011:") +
                                (detachHead ? "02\n" : "0002\n");
      std::vector<unsigned char> bytes(input.begin(), input.end());
      bms_parser::Parser parser;
      bms_parser::Chart *raw = nullptr;
      std::atomic_bool cancelled{false};
      parser.Parse(bytes, &raw, false, false, cancelled);
      std::unique_ptr<bms_parser::Chart> chart(raw);
      assert(chart && chart->DetachedNotes.empty() && chart->Meta.TotalNotes == 2);
      std::optional<std::string> applied;
      std::optional<long long> seed;
      assert(play_options::applyPlayOptionModifier(*chart, option, 123, 0, applied, seed));
      int notes = 0;
      for (const auto *measure : chart->Measures)
        for (const auto *timeline : measure->TimeLines)
          for (const auto *note : timeline->Notes) if (note) {
            assert(dynamic_cast<const bms_parser::LongNote *>(note) == nullptr);
            if (option == "MIRROR") assert(note->Lane != 0);
            ++notes;
          }
      assert(notes == 2 && chart->Meta.TotalNotes == 2);

    }
  }
}

void testPreviewBranchesAndResourcesSurviveLaneModifiers() {
  const std::string input =
      "#BPM 120\n#RANDOM 2\n"
      "#IF 1\n#WAV01 first.wav\n#BMP01 first.bmp\n#00011:01\n#00004:01\n#ENDIF\n"
      "#IF 2\n#WAV02 second.wav\n#BMP02 second.bmp\n#00011:02\n#00019:0002\n#00004:02\n#ENDIF\n"
      "#ENDRANDOM\n";
  const std::vector<unsigned char> bytes(input.begin(), input.end());
  auto parsePreview = [&] {
    bms_parser::Parser parser;
    parser.SetRandomValues({2});
    bms_parser::Chart *raw = nullptr;
    std::atomic_bool cancelled = false;
    parser.Parse(bytes, &raw, false, false, cancelled);
    return std::unique_ptr<bms_parser::Chart>(raw);
  };
  auto pattern = [](const bms_parser::Chart &chart) {
    std::vector<std::tuple<long long, int, int>> notes;
    for (const auto *measure : chart.Measures)
      for (const auto *timeline : measure->TimeLines)
        for (const auto *note : timeline->Notes)
          if (note) notes.emplace_back(timeline->Timing, note->Lane, note->Wav);
    return notes;
  };
  for (const std::string option : {"MIRROR", "RANDOM", "R-RANDOM", "S-RANDOM"}) {
    auto preview = parsePreview();
    assert(preview && preview->Meta.RandomValues == std::vector<int>{2});
    assert(preview->ReferencedWavTable.size() == 1 && preview->ReferencedWavTable.at(2) == "second.wav");
    assert(preview->ReferencedBmpTable.size() == 1 && preview->ReferencedBmpTable.at(2) == "second.bmp");
    const auto sounds = preview->ReferencedWavTable;
    const auto visuals = preview->ReferencedBmpTable;
    const auto info = play_options::applySelectedPlayOptions(*preview, option);
    assert(info.option == option);
    assert(info.seed.has_value() == (option != "MIRROR"));
    assert(preview->Meta.RandomValues == std::vector<int>{2});
    assert(preview->ReferencedWavTable == sounds && preview->ReferencedBmpTable == visuals);
    assert(pattern(*preview).size() == 2);
    if (option == "MIRROR") assert(std::get<1>(pattern(*preview).front()) == 6);

    // Replay must reproduce the same lanes from the captured Play-time seed.
    auto replay = parsePreview();
    std::optional<std::string> replayOption;
    std::optional<long long> replaySeed;
    assert(play_options::applyPlayOptionModifier(*replay, option, info.seed, 0,
                                                replayOption, replaySeed));
    assert(pattern(*replay) == pattern(*preview));
  }
}

int main() {
  testPreviewBranchesAndResourcesSurviveLaneModifiers();
  testDemotedNotesFollowModifiedActiveLane();
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
