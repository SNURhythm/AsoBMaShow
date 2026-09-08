#include "music_select/MusicSelectBarManager.h"
#include "music_select/MusicSelectFolderStatusLoader.h"
#include "scene/MusicSelectDirectoryRestore.h"

#include <future>
#include <chrono>

#include "music_select_runtime_ledger_assertions.h"

#include <iostream>
#include <stdexcept>
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
  require(manager.snapshot().selectedIndex == 1 &&
              manager.snapshot().rows[1].id.value == "folder:b",
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

void testSourceSortingKeepsDifficultyTableFolderOrder() {
  ChartMetaRecord alpha;
  alpha.meta.Title = "Alpha Song";
  ChartMetaRecord zulu;
  zulu.meta.Title = "Zulu Song";
  MusicSelectProjection projection;
  projection.root = {{"table:first"}, {"folder:zulu"}, {"table:second"},
                     {"folder:alpha"}};
  projection.bars = {
      {.id = {"table:first"},
       .kind = skin::MusicSelectBarKind::Table,
       .title = "First table",
       .children = {{"hash:12"}, {"hash:1"}},
       .selectable = true,
       .sortable = true},
      {.id = {"folder:zulu"},
       .kind = skin::MusicSelectBarKind::Folder,
       .title = "Zulu folder",
       .selectable = true,
       .sortable = true},
      {.id = {"table:second"},
       .kind = skin::MusicSelectBarKind::Table,
       .title = "Second table",
       .selectable = true,
       .sortable = true},
      {.id = {"folder:alpha"},
       .kind = skin::MusicSelectBarKind::Folder,
       .title = "Alpha folder",
       .selectable = true,
       .sortable = true},
      {.id = {"hash:12"},
       .kind = skin::MusicSelectBarKind::Hash,
       .title = "sl12",
       .children = {{"song:zulu"}, {"song:alpha"}},
       .selectable = true,
       .sortable = true},
      {.id = {"hash:1"},
       .kind = skin::MusicSelectBarKind::Hash,
       .title = "sl1",
       .selectable = true,
       .sortable = true},
      {.id = {"song:zulu"},
       .kind = skin::MusicSelectBarKind::Song,
       .title = "Zulu Song",
       .chart = zulu,
       .selectable = true},
      {.id = {"song:alpha"},
       .kind = skin::MusicSelectBarKind::Song,
       .title = "Alpha Song",
       .chart = alpha,
       .selectable = true},
  };
  MusicSelectBarManager manager(std::move(projection), {.sortId = "TITLE"});
  auto snapshot = manager.snapshot();
  require(snapshot.rows.size() == 4 &&
              snapshot.rows[0].id.value == "folder:alpha" &&
              snapshot.rows[1].id.value == "folder:zulu" &&
              snapshot.rows[2].id.value == "table:first" &&
              snapshot.rows[3].id.value == "table:second",
          "the root applies Beatoraja's selected BarSorter while preserving "
          "non-Folder table order");

  require(manager.select({"table:first"}) && manager.openSelected(),
          "difficulty-table sorting fixture opens its first table");
  snapshot = manager.snapshot();
  require(snapshot.rows.size() == 2 &&
              snapshot.rows[0].id.value == "hash:12" &&
              snapshot.rows[1].id.value == "hash:1",
          "TableBar keeps difficulty-folder metadata order under the selected "
          "sorter");

  require(manager.openSelected(), "difficulty-table sorting fixture opens its level");
  snapshot = manager.snapshot();
  require(snapshot.rows.size() == 2 &&
              snapshot.rows[0].id.value == "song:alpha" &&
              snapshot.rows[1].id.value == "song:zulu",
          "HashBar still applies the selected song sorter");
}

void testPinnedUnavailableSongOrdering() {
  for (const std::string sort : {"ARTIST", "BPM", "LENGTH", "LEVEL"}) {
    for (bool reverse : {false, true}) {
      MusicSelectBar installed{.id = {"installed"}, .title = "Zulu",
                               .chart = ChartMetaRecord{},
                               .presentation = {.exists = true}};
      installed.chart->meta.Title = "Zulu";
      installed.chart->meta.Artist = "Zulu";
      installed.chart->meta.MaxBpm = 200;
      installed.chart->meta.PlayLength = 200;
      installed.chart->meta.PlayLevel = 12;
      MusicSelectBar missing{.id = {"missing"}, .title = "Alpha",
                             .chart = ChartMetaRecord{},
                             .presentation = {.exists = false}};
      missing.chart->unavailable = true;
      missing.chart->meta.Title = "Alpha";
      missing.chart->meta.Artist = "Alpha";
      missing.chart->meta.MaxBpm = 0;
      missing.chart->meta.PlayLength = 0;
      missing.chart->meta.PlayLevel = 0;
      MusicSelectProjection projection;
      projection.bars = {installed, missing};
      projection.root = reverse ? std::vector{installed.id, missing.id}
                                : std::vector{missing.id, installed.id};
      MusicSelectBarManager manager(projection, {.sortId = sort});
      require(manager.readView().rowAt(0).id == installed.id &&
                  manager.readView().rowAt(1).id == missing.id,
              sort + " must place installed songs before unavailable songs in either input order");
      projection.bars.front().chart->unavailable = true;
      projection.bars.front().presentation.exists = false;
      MusicSelectBarManager unavailable(projection, {.sortId = sort});
      require(unavailable.readView().rowAt(0).id == projection.root[0] &&
                  unavailable.readView().rowAt(1).id == projection.root[1],
              sort + " must treat two unavailable songs as equal and preserve authored order");
      manager.configure({.sortId = "TITLE"});
      require(manager.readView().rowAt(0).id == missing.id,
              "TITLE must retain title ordering regardless of availability");
    }
  }
}

} // namespace

void testBackgroundStatusReachesUnopenedBarsAndRejectsSupersededLoads() {
  MusicSelectProjection projection;
  projection.root = {{"folder:root"}};
  projection.bars = {{.id = {"folder:root"},
                     .kind = skin::MusicSelectBarKind::Folder,
                     .title = "Root",
                     .presentation = {.kind = skin::MusicSelectBarKind::Folder,
                                      .title = "Root"},
                     .selectable = true,
                     .childrenLoaded = false}};
  MusicSelectBarManager manager(projection);
  MusicSelectFolderStatusLoader loader;
  std::promise<void> started;
  std::promise<void> release;
  auto released = release.get_future().share();
  loader.request(projection.bars, "ALL", 1, [&](const MusicSelectBar &bar) {
    started.set_value();
    released.wait();
    auto frame = bar.presentation;
    frame.folderLampCounts[1] = 99;
    return frame;
  });
  require(started.get_future().wait_for(std::chrono::seconds(5)) ==
              std::future_status::ready,
          "status loading runs off the caller thread");
  require(!loader.request(projection.bars, "ALL", 1,
                          MusicSelectFolderStatusLoader::Processor{}),
          "selection movement does not restart the same directory status load");
  const bool changedLn = loader.request(projection.bars, "ALL", 2,
                                        [](const MusicSelectBar &bar) {
    auto frame = bar.presentation;
    frame.folderLampCounts[6] = 3;
    frame.folderRankCounts[24] = 3;
    frame.lamp = 6;
    return frame;
  });
  require(changedLn, "LN changes supersede status even with identical rows and mode");
  release.set_value();
  std::vector<MusicSelectFolderStatusLoader::Result> results;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (results.empty() && std::chrono::steady_clock::now() < deadline) {
    results = loader.takeResults();
    std::this_thread::yield();
  }
  require(results.size() == 1 && results.front().frame.folderLampCounts[1] == 0,
          "a newer status request discards the previous in-flight result");
  for (const auto &result : results) {
    manager.installFolderStatus(result.id, result.frame);
  }
  const auto snapshot = manager.snapshot();
  require(snapshot.rows.front().presentation.folderLampCounts[6] == 3 &&
              snapshot.rows.front().presentation.folderRankCounts[24] == 3 &&
              snapshot.rows.front().presentation.lamp == 6 &&
              snapshot.rows.front().title == "Root" &&
              !snapshot.rows.front().childrenLoaded && snapshot.directory.empty(),
          "background folder status updates the live unopened row without "
          "loading children or changing navigation");
}

void testLargeIndexedListsAndRetainedFrames() {
  for (const int count : {10'000, 50'000}) {
    MusicSelectProjection projection;
    projection.root = {{"large"}};
    MusicSelectBar parent{.id = {"large"},
                          .kind = skin::MusicSelectBarKind::Folder,
                          .title = "Large",
                          .presentation = {.kind = skin::MusicSelectBarKind::Folder},
                          .selectable = true};
    parent.children.reserve(count);
    projection.bars.reserve(count + 1);
    projection.bars.push_back(parent);
    for (int index = 0; index < count; ++index) {
      const MusicSelectBarId id{"song:" + std::to_string(index)};
      projection.bars.front().children.push_back(id);
      projection.bars.push_back(
          {.id = id, .title = "Song " + std::to_string(index),
           .presentation = {.title = "Song " + std::to_string(index),
                            .exists = true},
           .selectable = true});
    }
    MusicSelectBarManager manager({.bars = {parent}, .root = {{"large"}}});
    const auto started = std::chrono::steady_clock::now();
    auto children = musicSelectProjectionChildren(projection, {"large"});
    require(children.size() == static_cast<std::size_t>(count),
            "indexed extraction retains every child in authored order");
    require(manager.installChildren({"large"}, std::move(children)) &&
                manager.openSelected(),
            "large flat directory installs and opens");
    const auto elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    std::cout << "indexed extract/install/open " << count << " rows: "
              << elapsed << " ms\n";
    const auto initial = manager.readView();
    const auto frame = manager.songListFrame();
    const auto copiedFrame = frame;
    require(initial.rows.size() == static_cast<std::size_t>(count) &&
                frame.size() == static_cast<std::size_t>(count) &&
                frame.bars.empty() && copiedFrame.bars.empty() &&
                &frame.at(count - 1) == &initial.rows.back().presentation &&
                &copiedFrame.at(count - 1) == &frame.at(count - 1),
            "frame construction and copies retain indexed rows without copying titles");
    manager.move(false, -100, 1050);
    const auto wrapped = manager.songListFrame();
    require(wrapped.selectedIndex == static_cast<std::size_t>(count - 1) &&
                wrapped.movementDirection == -100 &&
                wrapped.movementEndMillis == 1050 &&
                manager.readView().rows.data() == initial.rows.data() &&
                manager.readView().rowsRevision == initial.rowsRevision,
            "wrapping changes absolute selection, not row storage or status generation");
    manager.setSelectedPosition(0.5F);
    require(manager.songListFrame().selectedIndex ==
                static_cast<std::size_t>(count / 2),
            "position writer addresses the entire large list");
    const auto owned = manager.snapshot();
    require(manager.close(), "large list closes");
    manager.refresh({});
    require(frame.at(count - 1).title == "Song " + std::to_string(count - 1) &&
                initial.rows.back().id.value == "song:" + std::to_string(count - 1) &&
                owned.rows.back().title == "Song " + std::to_string(count - 1),
            "frames, read views, and owning snapshots survive manager rebuilds");
    require(manager.songListFrame().size() == 0,
            "empty manager publishes an empty indexed list");
  }
}

void testReadViewsKeepFolderStatusAndOwningSnapshotsIndependent() {
  auto projection = fixture();
  projection.bars.front().presentation.kind = skin::MusicSelectBarKind::Folder;
  MusicSelectBarManager manager(std::move(projection));
  const auto owned = manager.snapshot();
  const auto view = manager.readView();
  const auto frame = manager.songListFrame();
  skin::MusicSelectBarFrame status;
  status.folderLampCounts[6] = 12'345;
  status.folderRankCounts[24] = 12'345;
  manager.installFolderStatus({"folder:a"}, status);
  require(manager.songListFrame().at(0).folderLampCounts[6] == 12'345 &&
              manager.songListFrame().at(0).folderRankCounts[24] == 12'345 &&
              manager.readView().rowsRevision == view.rowsRevision,
          "large folder statistics update without changing list membership");
  require(owned.rows[0].presentation.folderLampCounts[6] == 0 &&
              view.rows[0].presentation.folderLampCounts[6] == 0 &&
              frame.at(0).folderLampCounts[6] == 0,
          "published views and owning snapshots are immutable across status patches");
  manager.refresh({});
  require(view.rows[0].children.size() == 2 &&
              owned.rows[0].children.size() == 2,
          "retained views preserve complete folder children after manager mutation");
}

void testAfterOpenFolderStatusKeepsDirectoryViewsConsistent() {
  auto projection = fixture();
  projection.bars[0].children = {{"folder:b"}};
  MusicSelectBarManager manager(std::move(projection));
  const auto statusWithCount = [](int count, int lamp, int rivalLamp) {
    skin::MusicSelectBarFrame status;
    status.folderLampCounts[6] = count;
    status.folderRankCounts[24] = count;
    status.lamp = lamp;
    status.rivalLamp = rivalLamp;
    return status;
  };
  const auto sameStatus = [](const skin::MusicSelectBarFrame &actual,
                             const skin::MusicSelectBarFrame &expected) {
    return actual.folderLampCounts == expected.folderLampCounts &&
           actual.folderRankCounts == expected.folderRankCounts &&
           actual.lamp == expected.lamp && actual.rivalLamp == expected.rivalLamp;
  };
  const auto requireFreshDirectories = [&] {
    const auto view = manager.readView();
    const auto snapshot = manager.snapshot();
    require(view.directoryBars.size() == snapshot.directoryBars.size(),
            "fresh directory views and snapshots have matching depth");
    for (std::size_t index = 0; index < view.directoryBars.size(); ++index) {
      require(view.directoryBars[index].id == snapshot.directoryBars[index].id &&
                  sameStatus(view.directoryBars[index].presentation,
                             snapshot.directoryBars[index].presentation),
              "fresh read view directory stats match fresh snapshot after late install");
    }
  };
  const auto parentInitial = statusWithCount(3, 2, 1);
  const auto parentUpdated = statusWithCount(5, 6, 4);
  const auto childUpdated = statusWithCount(7, 5, 3);
  manager.installFolderStatus({"folder:a"}, parentInitial);
  require(manager.open({"folder:a"}), "after-open stats fixture opens parent");
  const auto parentView = manager.readView();
  const auto parentSnapshot = manager.snapshot();
  const auto parentFrame = manager.songListFrame();
  manager.installFolderStatus({"folder:a"}, parentUpdated);
  manager.installFolderStatus({"folder:b"}, childUpdated);
  requireFreshDirectories();
  const auto updatedParentView = manager.readView();
  require(sameStatus(updatedParentView.directoryBars[0].presentation, parentUpdated) &&
              sameStatus(updatedParentView.rowAt(0).presentation, childUpdated) &&
              sameStatus(manager.songListFrame().at(0), childUpdated) &&
              updatedParentView.rowsRevision == parentView.rowsRevision &&
              updatedParentView.selectedIndex == parentView.selectedIndex &&
              updatedParentView.directoryText == parentView.directoryText,
          "late parent and visible child stats update without changing list revision or navigation");
  require(sameStatus(parentView.directoryBars[0].presentation, parentInitial) &&
              sameStatus(parentSnapshot.directoryBars[0].presentation, parentInitial) &&
              parentView.rowAt(0).presentation.folderLampCounts[6] == 0 &&
              parentSnapshot.rowAt(0).presentation.folderLampCounts[6] == 0 &&
              parentFrame.at(0).folderLampCounts[6] == 0,
          "retained parent views, snapshots and frames keep their pre-install stats");

  require(manager.open({"folder:b"}), "after-open stats fixture opens nested child");
  requireFreshDirectories();
  const auto nestedView = manager.readView();
  const auto nestedSnapshot = manager.snapshot();
  const auto nestedFrame = manager.songListFrame();
  const auto parentNested = statusWithCount(9, 7, 5);
  const auto childNested = statusWithCount(11, 8, 6);
  manager.installFolderStatus({"folder:a"}, parentNested);
  manager.installFolderStatus({"folder:b"}, childNested);
  requireFreshDirectories();
  const auto updatedNestedView = manager.readView();
  require(sameStatus(updatedNestedView.directoryBars[0].presentation, parentNested) &&
              sameStatus(updatedNestedView.directoryBars[1].presentation, childNested) &&
              updatedNestedView.rowsRevision == nestedView.rowsRevision &&
              updatedNestedView.rows.data() == nestedView.rows.data() &&
              updatedNestedView.selectedIndex == nestedView.selectedIndex &&
              updatedNestedView.rowAt(0).id.value == "song:3" &&
              updatedNestedView.rowAt(0).presentation.folderLampCounts[6] == 0,
          "nested late installs update each matching ancestor without rebuilding or changing song rows");
  require(sameStatus(nestedView.directoryBars[0].presentation, parentUpdated) &&
              sameStatus(nestedView.directoryBars[1].presentation, childUpdated) &&
              sameStatus(nestedSnapshot.directoryBars[0].presentation, parentUpdated) &&
              sameStatus(nestedSnapshot.directoryBars[1].presentation, childUpdated) &&
              sameStatus(updatedParentView.directoryBars[0].presentation, parentUpdated) &&
              sameStatus(updatedParentView.rowAt(0).presentation, childUpdated) &&
              nestedFrame.at(0).folderLampCounts[6] == 0,
          "nested late installs preserve every previously published directory view and frame");

  require(manager.close(), "back navigation returns to parent after late stats");
  requireFreshDirectories();
  require(sameStatus(manager.readView().directoryBars[0].presentation, parentNested) &&
              sameStatus(manager.readView().rowAt(0).presentation, childNested),
          "back navigation keeps updated parent directory and child row stats");
  require(manager.openSelected(), "updated child reopens after back navigation");
  requireFreshDirectories();
  require(sameStatus(manager.readView().directoryBars[1].presentation, childNested),
          "reopening child keeps its latest installed stats");
  require(manager.close() && manager.close(), "back navigation returns to root");
  require(sameStatus(manager.readView().rowAt(0).presentation, parentNested) &&
              manager.readView().directoryBars.empty(),
          "root row retains the latest parent stats after closing nested directories");
}

class CountingRowProvider final : public MusicSelectRowProvider {
public:
  explicit CountingRowProvider(std::size_t count) : count_(count) {}

  std::size_t size() const noexcept override { return empty_ ? 0 : count_; }

  const MusicSelectBar &at(std::size_t index) const override {
    if (index >= size()) throw std::out_of_range("provider row");
    ++atCalls;
    const auto number = reversed_ ? count_ - index - 1 : index;
    cached_ = {.id = {"paged:" + std::to_string(number)},
               .title = "Paged " + std::to_string(number),
               .presentation = {.title = "Paged " + std::to_string(number),
                                .exists = true},
               .selectable = true};
    return cached_;
  }

  std::optional<std::size_t> indexOf(const MusicSelectBarId &id) const override {
    ++indexCalls;
    if (!id.value.starts_with("paged:")) return std::nullopt;
    const auto number = std::stoull(id.value.substr(6));
    if (number >= size()) return std::nullopt;
    return reversed_ ? count_ - number - 1 : number;
  }

  std::pair<std::string, std::string> configure(
      const std::string &modeFilter, const std::string &difficultyFilter,
      const std::string &sortId) override {
    ++configureCalls;
    lastConfig = {modeFilter, difficultyFilter, sortId};
    reversed_ = sortId == "LEVEL";
    empty_ = modeFilter == "EMPTY";
    cached_ = {};
    return {modeFilter == "MISSING" ? "7KEY" : modeFilter,
            difficultyFilter == "MISSING" ? "NORMAL" : difficultyFilter};
  }

  std::shared_ptr<MusicSelectRowProvider> clone() const override {
    ++cloneCalls;
    return std::make_shared<CountingRowProvider>(*this);
  }

  mutable std::size_t atCalls = 0;
  mutable std::size_t indexCalls = 0;
  mutable std::size_t cloneCalls = 0;
  std::size_t configureCalls = 0;
  MusicSelectBarManagerConfig lastConfig;

private:
  std::size_t count_;
  bool reversed_ = false;
  bool empty_ = false;
  mutable MusicSelectBar cached_;
};

void testPagedRowsStayLazyAcrossNavigationAndConfiguration() {
  auto projection = fixture();
  projection.bars[0].children.clear();
  projection.bars[0].childrenLoaded = false;
  MusicSelectBarManager manager(std::move(projection));
  auto provider = std::make_shared<CountingRowProvider>(100'000);
  require(!manager.installRowProvider({"missing"}, provider) &&
              !manager.installRowProvider({"song:1"}, provider) &&
              !manager.installRowProvider({"folder:a"}, nullptr),
          "providers require an existing directory and non-null ownership");
  require(manager.installRowProvider({"folder:a"}, provider) &&
              manager.select({"folder:b"}) && manager.open({"folder:a"}),
          "a provider opens a directory without eager child ids");
  const auto initial = manager.readView();
  const auto frame = manager.songListFrame();
  const auto snapshot = manager.snapshot();
  require(initial.rowCount() == 100'000 && !initial.rowsEmpty() &&
              initial.rows.empty() && snapshot.rows.empty() &&
              snapshot.rowCount() == 100'000 && frame.size() == 100'000 &&
              frame.bars.empty() && initial.rowProvider == provider &&
              snapshot.rowProvider == provider && frame.rowProvider == provider &&
              provider->atCalls == 0 && provider->configureCalls == 1 &&
              provider->cloneCalls == 0,
          "open, read view, snapshot and frame retain providers without enumeration");
  manager.move(false, -100, 1050);
  require(manager.readView().selectedIndex == 99'999 &&
              manager.songListFrame().movementDirection == -100 &&
              manager.songListFrame().movementEndMillis == 1050,
          "provider navigation wraps with movement metadata");
  manager.move(true, 100, 1100);
  require(manager.readView().selectedIndex == 0,
          "provider forward navigation wraps to the first row");
  manager.setSelectedPosition(0.5F);
  manager.setSelectedPosition(1.0F);
  manager.setSelectedPosition(-1.0F);
  require(manager.readView().selectedIndex == 50'000 && provider->atCalls == 0,
          "absolute jumps and invalid positions do not fetch rows");
  require(manager.select({"paged:98765"}) && !manager.select({"missing"}) &&
              manager.readView().selectedIndex == 98'765 && provider->atCalls == 0 &&
              provider->indexCalls == 2,
          "stable-id selection uses provider lookup, not enumeration");
  require(initial.rowAt(99'999).id.value == "paged:99999" &&
              snapshot.rowAt(12'345).title == "Paged 12345" &&
              frame.at(98'765).title == "Paged 98765" && provider->atCalls == 3,
          "published access fetches only explicitly requested rows");
  const auto beforeConfigure = provider->atCalls;
  manager.configure({"MISSING", "MISSING", "LEVEL"});
  const auto configured = manager.readView();
  const auto configuredProvider =
      std::static_pointer_cast<CountingRowProvider>(configured.rowProvider);
  require(configured.selectedIndex == 1234 &&
              configured.resolvedModeFilter == "7KEY" &&
              configured.resolvedDifficultyFilter == "NORMAL" &&
              configured.rowsRevision > initial.rowsRevision &&
              configuredProvider->lastConfig.modeFilter == "MISSING" &&
              configuredProvider->lastConfig.difficultyFilter == "MISSING" &&
              configuredProvider->lastConfig.sortId == "LEVEL" &&
              configuredProvider->configureCalls == 2 &&
              provider->cloneCalls == 1 &&
              configuredProvider->atCalls <= beforeConfigure + 1 &&
              provider->atCalls <= beforeConfigure + 1,
          "global provider configuration resolves filters and preserves id across reorder");
  require(initial.rowAt(0).id.value == "paged:0" &&
              snapshot.rowAt(99'999).id.value == "paged:99999" &&
              frame.at(0).title == "Paged 0" &&
              configured.rowAt(0).id.value == "paged:99999",
          "configuration leaves previously published views, snapshots and frames unchanged");
  const auto beforeNoop = configuredProvider->cloneCalls;
  manager.configure({"7KEY", "NORMAL", "LEVEL"});
  require(manager.readView().rowProvider == configuredProvider &&
              configuredProvider->cloneCalls == beforeNoop,
          "unchanged effective configuration does not clone the provider index");
  require(manager.close() && manager.readView().rowAt(1).id.value == "folder:b" &&
              manager.readView().selectedIndex == 1 && !manager.readView().rowProvider,
          "closing a clicked provider directory restores the parent center selection");
  require(!manager.open({"folder:a"}) && !manager.readView().rowAt(0).childrenLoaded,
          "closing releases the directory binding and marks it for asynchronous reload");
  require(manager.installRowProvider({"folder:a"},
                                    std::make_shared<CountingRowProvider>(100'000)) &&
              manager.open({"folder:a"}) && manager.readView().rowCount() == 100'000,
          "a closed provider directory reopens after explicit reload");
  manager.configure({"EMPTY", "ALL", "TITLE"});
  require(manager.readView().rowsEmpty() && manager.songListFrame().size() == 0,
          "configuration can empty the provider without eager fallback");
  manager.move(true, 1, 1);
  manager.setSelectedPosition(0.5F);
  require(manager.readView().selectedIndex == 0 && !manager.openSelected() &&
              !manager.select({"paged:0"}),
          "empty providers reject row actions without fetching an invalid row");
}

void testPagedProviderLifetimeAndExplicitEnumeration() {
  MusicSelectBarManager manager(fixture());
  auto provider = std::make_shared<CountingRowProvider>(257);
  require(manager.installRowProvider({"folder:a"}, provider),
          "provider overrides eager directory children");
  const auto children = manager.childrenOf({"folder:a"});
  require(children.size() == 257 && provider->atCalls == 257 &&
              children.front().id.value == "paged:0" &&
              children.back().id.value == "paged:256" &&
              children[128].title == "Paged 128",
          "explicit enumeration copies every row before a provider cache eviction");
  require(manager.open({"folder:a"}), "provider lifetime fixture opens");
  std::weak_ptr<MusicSelectRowProvider> weak = provider;
  {
    const auto view = manager.readView();
    const auto snapshot = manager.snapshot();
    const auto frame = manager.songListFrame();
    provider.reset();
    manager.refresh(fixture(2));
    require(!weak.expired() && !manager.readView().rowProvider &&
                manager.readView().rowCount() == 2 &&
                view.rowAt(256).id.value == "paged:256" &&
                snapshot.rowAt(0).id.value == "paged:0" &&
                frame.at(128).title == "Paged 128",
            "refresh releases installed providers while retained publications stay readable");
  }
  require(weak.expired(), "last retained publication releases its provider");
  auto empty = std::make_shared<CountingRowProvider>(0);
  require(manager.close() && manager.installRowProvider({"folder:a"}, empty) &&
              !manager.open({"folder:a"}) && manager.readView().directory.empty() &&
              empty->atCalls == 0,
          "empty providers do not open or fall back to eager children");
  auto unopened = std::make_shared<CountingRowProvider>(100'000);
  std::weak_ptr<MusicSelectRowProvider> unopenedWeak = unopened;
  require(manager.installRowProvider({"folder:b"}, unopened),
          "unopened provider is installed");
  unopened.reset();
  manager.refresh(fixture(3));
  require(unopenedWeak.expired(), "refresh releases unopened directory providers too");
  auto closing = std::make_shared<CountingRowProvider>(100'000);
  std::weak_ptr<MusicSelectRowProvider> closingWeak = closing;
  require(manager.installRowProvider({"folder:a"}, closing) &&
              manager.open({"folder:a"}), "close ownership fixture opens");
  closing.reset();
  require(manager.close() && closingWeak.expired() &&
              !manager.readView().rowAt(0).childrenLoaded,
          "close releases the exiting provider and invalidates its directory load flag");
}

void testProviderReplacementAndNestedBackNavigation() {
  MusicSelectBarManager manager(fixture());
  auto provider = std::make_shared<CountingRowProvider>(100'000);
  require(manager.open({"folder:a"}) &&
              manager.installRowProvider({"folder:a"}, provider) &&
              manager.select({"paged:99999"}),
          "installing a provider on the active directory replaces eager rows");
  MusicSelectBar child{.id = {"song:transient"}, .title = "Transient song"};
  MusicSelectBar nested{.id = {"same-folder:paged"},
                         .kind = skin::MusicSelectBarKind::SameFolder,
                         .title = "Same Folder",
                         .children = {child.id}};
  require(manager.openTransient(nested, {child}) &&
              !manager.readView().rowProvider && manager.readView().rowCount() == 1 &&
              manager.close() && manager.readView().selectedIndex == 99'999 &&
              manager.readView().rowAt(99'999).id.value == "paged:99999",
          "closing an eager nested directory restores its cached provider source id");
  require(provider->cloneCalls == 0,
          "unchanged ancestor provider reopening does not clone the compact index");
  auto replacement = std::make_shared<CountingRowProvider>(100'000);
  std::weak_ptr<MusicSelectRowProvider> oldProvider = provider;
  provider.reset();
  require(manager.installRowProvider({"folder:a"}, replacement) &&
              manager.readView().selectedIndex == 99'999 && oldProvider.expired(),
          "replacing the active provider retains selection and releases the old owner");
  auto smaller = std::make_shared<CountingRowProvider>(1);
  require(manager.installRowProvider({"folder:a"}, smaller) &&
              manager.readView().selectedIndex == 0 && manager.readView().rowCount() == 1,
          "provider replacement resets selection when the old id is absent");
  child.id = {"paged:0"};
  require(manager.installChildren({"folder:a"}, {child}) &&
              !manager.readView().rowProvider &&
              manager.readView().rowAt(0).title == "Transient song",
          "explicit eager children replace an active provider without stale rows");
  require(manager.close() && manager.installRowProvider({"folder:b"}, smaller),
          "transient replacement fixture installs an inactive provider");
  MusicSelectBar transient{.id = {"folder:b"},
                            .kind = skin::MusicSelectBarKind::Folder,
                            .title = "Replacement",
                            .children = {child.id}};
  require(manager.openTransient(transient, {child}) &&
              !manager.readView().rowProvider &&
              manager.readView().rowAt(0).title == "Transient song",
          "transient directory installation replaces a provider with explicit eager rows");
}

void testOpeningProviderBackedIdSurvivesSelectedRowCacheEviction() {
  auto projection = fixture();
  projection.bars.push_back({.id = {"paged:42"},
                             .kind = skin::MusicSelectBarKind::Folder,
                             .title = "Nested",
                             .children = {{"song:3"}}});
  MusicSelectBarManager manager(std::move(projection));
  auto provider = std::make_shared<CountingRowProvider>(100'000);
  require(manager.installRowProvider({"folder:a"}, provider) &&
              manager.open({"folder:a"}) && manager.select({"paged:17"}),
          "cache eviction fixture selects a different row from the clicked id");
  const auto view = manager.readView();
  require(manager.open(view.rowAt(42).id) &&
              manager.readView().directory.back().value == "paged:42" &&
              manager.readView().rowCount() == 1 &&
              manager.readView().rowAt(0).id.value == "song:3" &&
              manager.close() && manager.readView().selectedIndex == 17,
          "opening copies the clicked id before fetching a cache-evicting selected row");
}

int main(int argc, char **argv) {
  testPinnedUnavailableSongOrdering();
  testPagedRowsStayLazyAcrossNavigationAndConfiguration();
  testPagedProviderLifetimeAndExplicitEnumeration();
  testProviderReplacementAndNestedBackNavigation();
  testOpeningProviderBackedIdSurvivesSelectedRowCacheEviction();
  {
    MusicSelectBarManager manager(fixture());
    require(manager.open({"folder:a"}) && manager.select({"song:2"}),
            "score refresh fixture selects the Same Folder source");
    MusicSelectBar sibling{.id = {"song:sibling"},
                           .kind = skin::MusicSelectBarKind::Song,
                           .title = "Sibling"};
    MusicSelectBar sameFolder{.id = {"same-folder:song:2"},
                              .kind = skin::MusicSelectBarKind::SameFolder,
                              .title = "Same Folder",
                              .children = {{"song:2"}, sibling.id}};
    require(manager.openTransient(sameFolder, {sibling}) &&
                manager.select(sibling.id),
            "score refresh fixture opens and selects a physical-folder sibling");
    const auto previous = manager.readView();
    manager.refresh({});
    manager.refresh(fixture(2));
    int reloaded = 0;
    restoreMusicSelectDirectory(
        manager, previous, [](const MusicSelectBar &) { return false; },
        [&](const MusicSelectBarId &source) {
          ++reloaded;
          sibling.score = ScoreBestSnapshot{.score = 1900};
          return manager.select(source) && manager.openTransient(sameFolder, {sibling});
        });
    const auto refreshed = manager.readView();
    require(reloaded == 1 && refreshed.directory == previous.directory &&
                refreshed.rows[refreshed.selectedIndex].id == sibling.id &&
                refreshed.rows[refreshed.selectedIndex].score->score == 1900 &&
                refreshed.rowsRevision > previous.rowsRevision,
            "score refresh rebuilds Same Folder rows while retaining directory and selection");
    require(manager.close() &&
                manager.readView().rows[manager.readView().selectedIndex].id.value == "song:2",
            "closing a score-refreshed Same Folder restores its original source chart");
  }
  testLargeIndexedListsAndRetainedFrames();
  testReadViewsKeepFolderStatusAndOwningSnapshotsIndependent();
  testAfterOpenFolderStatusKeepsDirectoryViewsConsistent();
  testBackgroundStatusReachesUnopenedBarsAndRejectsSupersededLoads();
  testWrapOpenCloseAndPositionSemantics();
  testClickedDirectoryOpensWithoutMovingTheCenterSelection();
  testBarClassPredicatesDoNotDependOnChildren();
  testRefreshRebindsStableSelection();
  testInstallsDeferredDirectoryChildrenBeforeOpening();
  testReplayAndHashCommandsUseSelectableSongState();
  testTableContextUsesOpenedTableAndHashTitles();
  testTransientDirectoryRestoresItsSourceBar();
  testPinnedFilterFallbackAndSort();
  testSourceSortingKeepsDifficultyTableFolderOrder();
  return music_select_runtime_ledger_assertions::finish(
      argc, argv, "music_select_bar_manager_tests", failures,
      "music-select bar manager assertion(s) failed",
      "music-select bar manager tests passed");
}
