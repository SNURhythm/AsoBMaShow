#include "music_select/MusicSelectFavorites.h"

#include <array>
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

void testThreeStateCycleAndPrecedenceAreSourceExact() {
  constexpr MusicSelectFavoriteBits chart{.favorite = 2, .invisible = 8};
  require(musicSelectNextFavoriteState(0, chart, 1) ==
              MusicSelectFavoriteState::Favorite &&
              musicSelectNextFavoriteState(2, chart, 1) ==
                  MusicSelectFavoriteState::Invisible &&
              musicSelectNextFavoriteState(8, chart, 1) ==
                  MusicSelectFavoriteState::None,
          "forward favorite cycle is none, favorite, invisible, none");
  require(musicSelectNextFavoriteState(0, chart, -1) ==
              MusicSelectFavoriteState::Invisible &&
              musicSelectNextFavoriteState(2, chart, -1) ==
                  MusicSelectFavoriteState::None &&
              musicSelectNextFavoriteState(8, chart, -1) ==
                  MusicSelectFavoriteState::Favorite,
          "reverse favorite cycle follows the source modulo step");
  require(musicSelectNextFavoriteState(10, chart, 1) ==
              MusicSelectFavoriteState::None,
          "invisible has precedence when both source bits are present");
}

void testSongStateAppliesAcrossFolderWithoutChangingChartBits() {
  constexpr MusicSelectFavoriteBits song{.favorite = 1, .invisible = 4};
  const auto state =
      musicSelectNextFavoriteState(1 | 8, song, 1);
  require(state == MusicSelectFavoriteState::Invisible,
          "selected song determines the folder-wide next state");
  const std::array source{0, 1 | 2, 4 | 8, 1 | 4 | 2 | 8};
  const std::array expected{4, 4 | 2, 4 | 8, 4 | 2 | 8};
  std::array<int, 4> actual{};
  for (std::size_t index = 0; index < source.size(); ++index) {
    actual[index] = musicSelectApplyFavoriteState(source[index], song, state);
  }
  require(actual == expected,
          "folder-wide song state preserves every non-song review bit");
}

} // namespace

int main() {
  testThreeStateCycleAndPrecedenceAreSourceExact();
  testSongStateAppliesAcrossFolderWithoutChangingChartBits();
  if (failures != 0) return 1;
  std::cout << "music-select favorite tests passed\n";
  return 0;
}
