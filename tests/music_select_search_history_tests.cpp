#include "music_select/MusicSelectSearchHistory.h"

#include <iostream>
#include <string_view>

namespace {
int failures = 0;

void require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void testBlankAndEmptyResultsDoNothing() {
  MusicSelectSearchHistory history;
  require(!MusicSelectSearchHistory::acceptsText("") &&
              !MusicSelectSearchHistory::acceptsText(" \t\n") &&
              !MusicSelectSearchHistory::acceptsText("\xe3\x80\x80") &&
              MusicSelectSearchHistory::acceptsText("\xc2\xa0") &&
              !history.remember("", true) &&
              !history.remember(" \t\n", true) &&
              !history.remember("\xe3\x80\x80", true) &&
              !history.remember("missing", false) &&
              history.entries().empty(),
          "Java-blank text and empty result sets do not create search bars");
  require(history.remember("\xc2\xa0", true),
          "Java non-breaking space is not Character whitespace");
}

void testDuplicateMovesToEndAndMaximumEvictsOldest() {
  MusicSelectSearchHistory history;
  require(history.remember("one", true, 3) &&
              history.remember("two", true, 3) &&
              history.remember("three", true, 3) &&
              history.remember("one", true, 3),
          "result-bearing searches are accepted");
  require(history.entries().size() == 3 &&
              history.entries()[0] == "two" &&
              history.entries()[1] == "three" &&
              history.entries()[2] == "one",
          "duplicate search title is removed and appended");
  require(history.remember("four", true, 3) &&
              history.entries()[0] == "three" &&
              history.entries()[2] == "four",
          "full search history removes index zero before appending");
}
} // namespace

int main() {
  testBlankAndEmptyResultsDoNothing();
  testDuplicateMovesToEndAndMaximumEvictsOldest();
  if (failures != 0) return 1;
  std::cout << "music-select search history tests passed\n";
  return 0;
}
