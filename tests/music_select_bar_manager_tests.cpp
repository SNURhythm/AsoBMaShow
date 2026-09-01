#include "music_select/MusicSelectBarManager.h"

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

MusicSelectProjection fixture(std::uint64_t revision = 1) {
  MusicSelectProjection value;
  value.repositoryRevision = revision;
  value.root = {{"folder:a"}, {"folder:b"}};
  value.bars = {
      {.id = {"folder:a"},
       .kind = skin::MusicSelectBarKind::Folder,
       .title = "A",
       .children = {{"song:1"}, {"song:2"}},
       .selectable = true,
       .sortable = true},
      {.id = {"folder:b"},
       .kind = skin::MusicSelectBarKind::Folder,
       .title = "B",
       .children = {{"song:3"}},
       .selectable = true,
       .sortable = true},
      {.id = {"song:1"}, .kind = skin::MusicSelectBarKind::Song,
       .title = "One", .selectable = true},
      {.id = {"song:2"}, .kind = skin::MusicSelectBarKind::Song,
       .title = "Two", .selectable = true},
      {.id = {"song:3"}, .kind = skin::MusicSelectBarKind::Song,
       .title = "Three", .selectable = true},
  };
  return value;
}

void testWrapOpenCloseAndPositionSemantics() {
  MusicSelectBarManager manager(fixture());
  require(manager.snapshot().rows.size() == 2 &&
              manager.snapshot().selectedIndex == 0,
          "manager starts on the root's first bar");
  manager.move(false, 100, 250);
  require(manager.snapshot().selectedIndex == 1 &&
              manager.snapshot().movementDirection == -1 &&
              manager.snapshot().movementEndMillis == 350,
          "decrement wraps and publishes the source scroll window");
  require(manager.openSelected() && manager.snapshot().rows.size() == 1 &&
              manager.snapshot().directoryText == "B > ",
          "opening a DirectoryBar replaces rows and appends directory text");
  require(manager.close() && manager.snapshot().selectedIndex == 1,
          "closing restores the directory bar that was opened");
  manager.setSelectedPosition(0.0F);
  require(manager.snapshot().selectedIndex == 0,
          "selected-position writer truncates rowCount * value");
  manager.setSelectedPosition(1.0F);
  require(manager.snapshot().selectedIndex == 0,
          "the source ignores a selected position of exactly one");
}

void testRefreshRebindsStableSelection() {
  MusicSelectBarManager manager(fixture());
  manager.move(true, 0, 0);
  auto next = fixture(2);
  std::swap(next.root[0], next.root[1]);
  manager.refresh(std::move(next));
  require(manager.snapshot().selectedIndex == 0 &&
              manager.snapshot().rows.front().id.value == "folder:b",
          "revision replacement rebinds selection by stable bar identity");
}

void testPinnedFilterFallbackAndSort() {
  auto projection = fixture();
  auto *first = const_cast<MusicSelectBar *>(projection.find({"song:1"}));
  auto *second = const_cast<MusicSelectBar *>(projection.find({"song:2"}));
  require(first && second, "filter fixture songs exist");
  if (!first || !second) return;
  first->chart.emplace();
  first->chart->meta.Title = "Zulu";
  first->chart->meta.KeyMode = 7;
  first->chart->meta.TotalNotes = 500;
  first->score = ScoreBestSnapshot{.score = 500, .maxScore = 1000};
  second->chart.emplace();
  second->chart->meta.Title = "Alpha";
  second->chart->meta.KeyMode = 14;
  second->chart->meta.TotalNotes = 1300;
  second->score = ScoreBestSnapshot{.score = 1800, .maxScore = 2600};

  MusicSelectBarManager manager(
      std::move(projection),
      {.modeFilter = "14KEY", .difficultyFilter = "ANOTHER",
       .sortId = "TITLE"});
  require(manager.openSelected(), "filter fixture opens the first folder");
  auto snapshot = manager.snapshot();
  require(snapshot.rows.size() == 1 &&
              snapshot.rows.front().id.value == "song:2" &&
              snapshot.resolvedModeFilter == "14KEY" &&
              snapshot.resolvedDifficultyFilter == "ANOTHER",
          "ModeFilter and DifficultyFilter retain a non-empty exact match");

  manager.configure({.modeFilter = "48KEY",
                     .difficultyFilter = "INSANE",
                     .sortId = "TITLE"});
  snapshot = manager.snapshot();
  require(snapshot.rows.size() == 1 &&
              snapshot.rows.front().id.value == "song:1" &&
              snapshot.resolvedModeFilter == "SINGLE" &&
              snapshot.resolvedDifficultyFilter == "ALL",
          "filter trials advance mode first and difficulty second");

  manager.configure({.modeFilter = "ALL",
                     .difficultyFilter = "ALL",
                     .sortId = "TITLE"});
  snapshot = manager.snapshot();
  require(snapshot.rows.size() == 2 &&
              snapshot.rows.front().id.value == "song:2" &&
              snapshot.rows.back().id.value == "song:1",
          "sortable directories use the selected BarSorter");

  auto invisible = fixture();
  auto *hiddenOne = const_cast<MusicSelectBar *>(invisible.find({"song:1"}));
  auto *hiddenTwo = const_cast<MusicSelectBar *>(invisible.find({"song:2"}));
  hiddenOne->chart.emplace();
  hiddenTwo->chart.emplace();
  hiddenOne->chart->songReviewFavorite = 4;
  hiddenTwo->chart->songReviewFavorite = 8;
  MusicSelectBarManager hidden(std::move(invisible));
  require(hidden.openSelected(), "invisible fallback fixture opens");
  require(hidden.snapshot().rows.size() == 2,
          "if every trial removes every SongBar, source leaves rows unfiltered");
}

} // namespace

int main() {
  testWrapOpenCloseAndPositionSemantics();
  testRefreshRebindsStableSelection();
  testPinnedFilterFallbackAndSort();
  if (failures != 0) return 1;
  std::cout << "music-select bar manager tests passed\n";
  return 0;
}
