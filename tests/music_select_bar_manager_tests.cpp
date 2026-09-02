#include "music_select/MusicSelectBarManager.h"

#include "music_select_runtime_ledger_assertions.h"

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
  manager.move(false, -250, 350);
  require(manager.snapshot().selectedIndex == 1 &&
              manager.snapshot().movementDirection == -250 &&
              manager.snapshot().movementEndMillis == 350,
          "decrement wraps and publishes the signed source scroll window");
  require(manager.openSelected() && manager.snapshot().rows.size() == 1 &&
              manager.snapshot().directoryText == "B > " &&
              manager.snapshot().directoryBars.size() == 1 &&
              manager.snapshot().directoryBars.front().id.value == "folder:b",
          "opening a DirectoryBar replaces rows and appends directory text");
  const auto children = manager.childrenOf({"folder:b"});
  require(children.size() == 1 && children.front().id.value == "song:3",
          "directory children are returned as value-owned bars");
  require(manager.close() && manager.snapshot().selectedIndex == 1,
          "closing restores the directory bar that was opened");
  manager.setSelectedPosition(0.0F);
  require(manager.snapshot().selectedIndex == 0,
          "selected-position writer truncates rowCount * value");
  manager.setSelectedPosition(1.0F);
  require(manager.snapshot().selectedIndex == 0,
          "the source ignores a selected position of exactly one");
  require(manager.select({"folder:b"}) &&
              manager.snapshot().selectedIndex == 1 &&
              !manager.select({"missing"}),
          "direct source selection finds only a current row");
}

void testClickedDirectoryOpensWithoutMovingTheCenterSelection() {
  MusicSelectBarManager manager(fixture());
  manager.move(true, 0, 0);
  require(manager.snapshot().rows[manager.snapshot().selectedIndex].id.value ==
              "folder:b",
          "pointer fixture centers the second root directory");
  require(manager.open({"folder:a"}) &&
              manager.snapshot().rows.size() == 2 &&
              manager.snapshot().directoryBars.back().id.value == "folder:a",
          "a clicked DirectoryBar opens independently of the center bar");
  require(manager.close() && manager.snapshot().selectedIndex == 1 &&
              manager.snapshot().rows[1].id.value == "folder:b",
          "closing a clicked directory restores the previous center bar");
}

void testBarClassPredicatesDoNotDependOnChildren() {
  require(skin::musicSelectIsDirectoryBarKind(
              skin::MusicSelectBarKind::Folder) &&
              skin::musicSelectIsDirectoryBarKind(
                  skin::MusicSelectBarKind::Table) &&
              skin::musicSelectIsDirectoryBarKind(
                  skin::MusicSelectBarKind::Hash) &&
              skin::musicSelectIsDirectoryBarKind(
                  skin::MusicSelectBarKind::Command) &&
              skin::musicSelectIsDirectoryBarKind(
                  skin::MusicSelectBarKind::Container) &&
              skin::musicSelectIsDirectoryBarKind(
                  skin::MusicSelectBarKind::SearchWord) &&
              skin::musicSelectIsDirectoryBarKind(
                  skin::MusicSelectBarKind::SameFolder),
          "all seven Beatoraja DirectoryBar classes remain directories even "
          "when empty");
  require(skin::musicSelectIsSelectableBarKind(
              skin::MusicSelectBarKind::Song) &&
              skin::musicSelectIsSelectableBarKind(
                  skin::MusicSelectBarKind::Executable) &&
              skin::musicSelectIsSelectableBarKind(
                  skin::MusicSelectBarKind::Grade) &&
              skin::musicSelectIsSelectableBarKind(
                  skin::MusicSelectBarKind::RandomCourse),
          "all four Beatoraja SelectableBar classes are classified by type");
  require(!skin::musicSelectIsDirectoryBarKind(
              skin::MusicSelectBarKind::Song) &&
              !skin::musicSelectIsSelectableBarKind(
                  skin::MusicSelectBarKind::Folder),
          "directory and selectable class families remain distinct");
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

void testInstallsDeferredDirectoryChildrenBeforeOpening() {
  MusicSelectProjection projection;
  projection.root = {{"folder:root"}};
  projection.bars = {{.id = {"folder:root"},
                     .kind = skin::MusicSelectBarKind::Folder,
                     .title = "Root",
                     .selectable = true,
                     .sortable = true}};
  MusicSelectBarManager manager(std::move(projection));
  const MusicSelectBar song{.id = {"song:deferred"},
                            .kind = skin::MusicSelectBarKind::Song,
                            .title = "Deferred",
                            .selectable = true};
  require(manager.installChildren({"folder:root"}, {song}) &&
              manager.openSelected() && manager.snapshot().rows.size() == 1 &&
              manager.snapshot().rows.front().id == song.id,
          "deferred DirectoryBar children install before the source opens it");
}

void testReplayAndHashCommandsUseSelectableSongState() {
  MusicSelectBar song{
      .kind = skin::MusicSelectBarKind::Song,
      .chart = ChartMetaRecord{},
      .replayExists = {false, true, false, true},
      .selectable = true,
  };
  song.chart->meta.MD5 = "md5";
  song.chart->meta.SHA256 = "sha256";
  require(musicSelectFirstExistingReplay(&song) == 1 &&
              musicSelectNextExistingReplay(&song, 1) == 3 &&
              musicSelectNextExistingReplay(&song, 3) == 1,
          "replay commands scan only the four existing source slots");
  require(musicSelectSelectedHash(&song, false) == "md5" &&
              musicSelectSelectedHash(&song, true) == "sha256",
          "hash commands read only SongBar chart hashes");
  song.replayExists = {};
  require(musicSelectFirstExistingReplay(&song) == -1 &&
              musicSelectNextExistingReplay(&song, -1) == -1,
          "missing replay slots retain the source -1 selection");
  song.kind = skin::MusicSelectBarKind::Folder;
  require(musicSelectSelectedHash(&song, true).empty(),
          "non-SongBar hashes are inert");
}

void testTableContextUsesOpenedTableAndHashTitles() {
  MusicSelectBarManagerSnapshot snapshot;
  snapshot.directoryBars = {
      {.kind = skin::MusicSelectBarKind::Table,
       .title = "Satellite",
       .tableId = 42},
      {.kind = skin::MusicSelectBarKind::Hash,
       .title = "sl12",
       .tableId = 42,
       .tableLevel = "12"},
  };
  const auto context = musicSelectTableContextForLaunch(snapshot);
  require(context.name == "Satellite" && context.level == "sl12" &&
              context.fullName == "sl12Satellite",
          "table properties use the opened TableBar and HashBar titles in "
          "PlayerResource level-before-name order");

  snapshot.directoryBars.front().tableId = 0;
  const auto local = musicSelectTableContextForLaunch(snapshot);
  require(local.name.empty() && local.level.empty() &&
              local.fullName.empty(),
          "the local COURSE table does not publish imported-table context");

  MusicSelectProjection projection;
  projection.root = {{"table:42"}};
  projection.bars = {
      {.id = {"table:42"},
       .kind = skin::MusicSelectBarKind::Table,
       .title = "Satellite",
       .children = {{"hash:42:12"}},
       .tableId = 42,
       .selectable = true},
      {.id = {"hash:42:12"},
       .kind = skin::MusicSelectBarKind::Hash,
       .title = "sl12",
       .children = {{"song:table"}},
       .tableId = 42,
       .selectable = true},
      {.id = {"song:table"},
       .kind = skin::MusicSelectBarKind::Song,
       .title = "Song",
       .selectable = true},
  };
  MusicSelectBarManager manager(std::move(projection));
  require(musicSelectTableContextForLaunch(manager.snapshot()).name.empty(),
          "table properties are empty before navigating into a table");
  require(manager.openSelected() &&
              musicSelectTableContextForLaunch(manager.snapshot()).name ==
                  "Satellite",
          "table properties follow the currently opened Table bar");
  require(manager.openSelected() &&
              musicSelectTableContextForLaunch(manager.snapshot()).level ==
                  "sl12",
          "table properties follow the currently opened Hash bar");
  require(manager.close() &&
              musicSelectTableContextForLaunch(manager.snapshot()).level.empty(),
          "closing the Hash bar clears the current table level");
}

void testTransientDirectoryRestoresItsSourceBar() {
  MusicSelectBarManager manager(fixture());
  require(manager.openSelected(), "transient fixture opens its root folder");
  manager.move(true, 0, 0);
  require(manager.snapshot().rows[manager.snapshot().selectedIndex].id.value ==
              "song:2",
          "transient fixture selects its source song");

  MusicSelectBar sameFolder{
      .id = {"same-folder:song:2"},
      .kind = skin::MusicSelectBarKind::SameFolder,
      .title = "Two",
      .children = {{"song:1"}, {"song:2"}},
      .presentation = {.kind = skin::MusicSelectBarKind::SameFolder,
                       .title = "Two",
                       .exists = true},
  };
  require(manager.openTransient(std::move(sameFolder), {}),
          "SameFolderBar opens from the current song");
  require(manager.snapshot().rows.size() == 2 &&
              manager.snapshot().directoryText == "A > Two > ",
          "temporary directory participates in the source directory text");
  require(manager.close() && manager.snapshot().selectedIndex == 1 &&
              manager.snapshot().rows[1].id.value == "song:2",
          "closing a temporary directory restores its source song");
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
  first->chart->meta.Difficulty = 4;
  first->chart->meta.MinBpm = first->chart->meta.MaxBpm = 120;
  first->score = ScoreBestSnapshot{.score = 500, .maxScore = 1000};
  second->chart.emplace();
  second->chart->meta.Title = "Alpha";
  second->chart->meta.KeyMode = 14;
  second->chart->meta.TotalNotes = 1300;
  second->chart->meta.Difficulty = 1;
  second->chart->meta.MinBpm = second->chart->meta.MaxBpm = 120;
  second->score = ScoreBestSnapshot{.score = 1800, .maxScore = 2600};

  MusicSelectBarManager manager(
      projection,
      {.modeFilter = "14KEY", .difficultyFilter = "ANOTHER",
       .sortId = "TITLE"});
  require(manager.openSelected(), "filter fixture opens the first folder");
  auto snapshot = manager.snapshot();
  require(snapshot.rows.size() == 1 &&
              snapshot.rows.front().id.value == "song:2" &&
              snapshot.resolvedModeFilter == "14KEY" &&
              snapshot.resolvedDifficultyFilter == "ANOTHER",
          "named difficulty uses the pinned nearest-notes profile rather than "
          "the chart difficulty field");

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

  first->score->averageJudgeMicros = 200;
  second->score->averageJudgeMicros = 100;
  manager.refresh(projection);
  manager.configure({.modeFilter = "ALL",
                     .difficultyFilter = "ALL",
                     .sortId = "DURATION"});
  snapshot = manager.snapshot();
  require(snapshot.rows.front().id.value == "song:2" &&
              snapshot.rows.back().id.value == "song:1",
          "DURATION uses the source average-judge ordering");
  first->score->averageJudgeMicros.reset();
  second->score->averageJudgeMicros.reset();
  manager.refresh(projection);
  manager.configure({.modeFilter = "ALL",
                     .difficultyFilter = "ALL",
                     .sortId = "DURATION"});
  snapshot = manager.snapshot();
  require(snapshot.rows.front().id.value == "song:1" &&
              snapshot.rows.back().id.value == "song:2",
          "missing DURATION values compare equal and retain source order");

  first->score->comboBreak = 99;
  second->score->comboBreak = 1;
  manager.refresh(projection);
  manager.configure({.modeFilter = "ALL",
                     .difficultyFilter = "ALL",
                     .sortId = "MISSCOUNT"});
  snapshot = manager.snapshot();
  require(snapshot.rows.front().id.value == "song:1" &&
              snapshot.rows.back().id.value == "song:2",
          "missing BP compares as zero without substituting combo breaks");
  first->score->badPoints = 2;
  second->score->badPoints = 1;
  manager.refresh(projection);
  snapshot = manager.snapshot();
  require(snapshot.rows.front().id.value == "song:2" &&
              snapshot.rows.back().id.value == "song:1",
          "MISSCOUNT uses the source minimum-BP ordering");

  first->chart->hasBpmStop = true;
  second->chart->hasScrollChange = true;
  MusicSelectBarManager speedManager(
      std::move(projection),
      {.modeFilter = "ALL", .difficultyFilter = "SPEED CHANGE CHART"});
  require(speedManager.openSelected(), "speed filter fixture opens");
  snapshot = speedManager.snapshot();
  require(snapshot.rows.size() == 2,
          "speed-change filter uses stop and scroll sequence flags");

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

  auto visible = fixture();
  auto *visibleFolder =
      const_cast<MusicSelectBar *>(visible.find({"folder:a"}));
  auto *visibleHidden =
      const_cast<MusicSelectBar *>(visible.find({"song:1"}));
  auto *visibleNormal =
      const_cast<MusicSelectBar *>(visible.find({"song:2"}));
  visibleFolder->showInvisibleCharts = true;
  visibleHidden->chart.emplace();
  visibleNormal->chart.emplace();
  visibleHidden->chart->songReviewFavorite = 4;
  MusicSelectBarManager showInvisible(std::move(visible));
  require(showInvisible.openSelected() &&
              showInvisible.snapshot().rows.size() == 2,
          "DirectoryBar showInvisibleChart bypasses only invisible-bit removal");
}

} // namespace

int main(int argc, char **argv) {
  testWrapOpenCloseAndPositionSemantics();
  testClickedDirectoryOpensWithoutMovingTheCenterSelection();
  testBarClassPredicatesDoNotDependOnChildren();
  testRefreshRebindsStableSelection();
  testInstallsDeferredDirectoryChildrenBeforeOpening();
  testReplayAndHashCommandsUseSelectableSongState();
  testTableContextUsesOpenedTableAndHashTitles();
  testTransientDirectoryRestoresItsSourceBar();
  testPinnedFilterFallbackAndSort();
  return music_select_runtime_ledger_assertions::finish(
      argc, argv, "music_select_bar_manager_tests", failures,
      "music-select bar manager assertion(s) failed",
      "music-select bar manager tests passed");
}
