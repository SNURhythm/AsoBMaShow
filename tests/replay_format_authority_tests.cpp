#include "replay/ReplayFormat.h"
#include "replay/ReplayKeyMode.h"
#include "replay/ReplayOption.h"

#include <array>
#include <iostream>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void testOneKeyModeLayoutTable() {
  struct Case {
    int keyMode;
    int players;
    int lanes;
    int shuffleWidth;
    bool scratch;
    bool flip;
    std::string_view assignment;
  };
  constexpr std::array cases{
      Case{5, 1, 5, 6, true, false, "S12345"},
      Case{7, 1, 7, 8, true, false, "S1234567"},
      Case{9, 1, 9, 9, false, false, ""},
      Case{10, 2, 5, 6, true, true, "L123456789AR"},
      Case{14, 2, 7, 8, true, true, "L123456789ABCDER"},
      Case{24, 1, 26, 26, false, false, ""},
      Case{48, 2, 26, 26, false, false, ""},
  };
  for (const auto &test : cases) {
    const auto layout = replay::replayKeyModeLayout(test.keyMode);
    expect(layout && layout->players == test.players &&
               layout->logicalLanesPerPlayer == test.lanes &&
               layout->stockShuffleWidth == test.shuffleWidth &&
               layout->hasDirectionalScratch == test.scratch &&
               layout->supportsDoublePlayFlip == test.flip &&
               replay::manualAssignmentSymbols(test.keyMode) == test.assignment,
           "all replay branches consume one key-mode layout row");
  }
  expect(!replay::replayKeyModeLayout(8) &&
             replay::manualAssignmentSymbols(8).empty(),
         "unsupported key mode fails closed in the shared table");
}

void testOneStockOptionTable() {
  constexpr std::array<std::string_view, 10> options{
      "NORMAL", "MIRROR",   "RANDOM",  "R-RANDOM",  "S-RANDOM",
      "SPIRAL", "H-RANDOM", "ALL-SCR", "RANDOM-EX", "S-RANDOM-EX",
  };
  for (std::size_t index = 0; index < options.size(); ++index) {
    expect(replay::beatorajaReplayOptionIndex(options[index]) ==
                   static_cast<int>(index) &&
               replay::beatorajaReplayOptionName(static_cast<int>(index)) ==
                   options[index],
           "stock option name/index projection is one reversible authority");
  }
  expect(!replay::beatorajaReplayOptionIndex("random") &&
             !replay::beatorajaReplayOptionName(-1) &&
             replay::projectedBeatorajaReplayOptionIndex("ASSIGN:S1234567") ==
                 0 &&
             replay::validReplayPlayerOptionName("ASSIGN:S1234567", 7),
         "manual assignment validates once and projects to stock NORMAL");
  expect(!replay::validReplayPlayerOptionName("ASSIGN:S1234566", 7),
         "manual assignment must be a bijection");
}

void testOneCanonicalHexPredicate() {
  expect(replay::isCanonicalLowerHex(std::string(64, 'a'), 64),
         "lowercase SHA-256 is canonical");
  expect(!replay::isCanonicalLowerHex(std::string(64, 'A'), 64) &&
             !replay::isCanonicalLowerHex(std::string(63, 'a'), 64),
         "case and length are checked by one digest predicate");
}

} // namespace

int main() {
  testOneKeyModeLayoutTable();
  testOneStockOptionTable();
  testOneCanonicalHexPredicate();
  if (failures != 0) {
    std::cerr << failures << " replay format authority test(s) failed\n";
    return 1;
  }
  std::cout << "replay format authority tests passed\n";
  return 0;
}
