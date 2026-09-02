#include "music_select/MusicSelectExternalActions.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <fstream>
#include <set>

namespace {

struct TempDirectory {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("asobmashow-selector-actions-" +
       std::to_string(std::chrono::steady_clock::now()
                          .time_since_epoch()
                          .count()));

  TempDirectory() { std::filesystem::create_directories(path); }
  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

MusicSelectBar song(std::filesystem::path path, bool exists) {
  ChartMetaRecord record;
  record.meta.BmsPath = std::move(path);
  return {.kind = skin::MusicSelectBarKind::Song,
          .title = "Song",
          .chart = std::move(record),
          .presentation = {.kind = skin::MusicSelectBarKind::Song,
                           .exists = exists}};
}

void testDocumentSelectionMatchesFilesListFilter() {
  TempDirectory temporary;
  std::ofstream(temporary.path / "chart.bms") << "#TITLE test";
  std::ofstream(temporary.path / "readme.TXT") << "readme";
  std::ofstream(temporary.path / "details.txt") << "details";
  std::ofstream(temporary.path / "ignore.md") << "ignore";
  std::filesystem::create_directory(temporary.path / "folder.txt");

  auto bar = song(temporary.path / "chart.bms", true);
  const auto paths = musicSelectDocumentPaths(bar);
  std::set<std::filesystem::path> actual(paths.begin(), paths.end());
  const std::set<std::filesystem::path> expected{
      temporary.path / "details.txt", temporary.path / "readme.TXT"};
  assert(actual == expected);
  bar.presentation.exists = false;
  assert(musicSelectDocumentPaths(bar).empty());
}

void testArchivedDocumentsUseMaterializedResolverPaths() {
  const auto bar = song("/charts/package.zip/folder/chart.bms", true);
  int calls = 0;
  const auto paths = musicSelectDocumentPaths(
      bar, [&](const std::filesystem::path &path)
               -> std::optional<std::vector<std::filesystem::path>> {
        ++calls;
        assert(path == bar.chart->meta.BmsPath);
        return std::vector<std::filesystem::path>{
            "/cache/package/readme.txt", "/cache/package/details.TXT"};
      });
  assert(calls == 1);
  assert(paths == std::vector<std::filesystem::path>(
                      {"/cache/package/readme.txt",
                       "/cache/package/details.TXT"}));
}

void testExplorerBranchPriority() {
  TempDirectory temporary;
  const auto existingPath = temporary.path / "installed" / "chart.bms";
  auto bar = song(existingPath, true);
  int hashLookups = 0;
  int textLookups = 0;
  MusicSelectExplorerLookups lookups{
      .originalMd5Paths = [&](std::span<const std::string>) {
        ++hashLookups;
        return std::vector<std::filesystem::path>{};
      },
      .textPaths = [&](std::string_view) {
        ++textLookups;
        return std::vector<std::filesystem::path>{};
      }};
  assert(musicSelectExplorerPath(bar, lookups) == existingPath.parent_path());
  assert(hashLookups == 0 && textLookups == 0);

  bar.presentation.exists = false;
  bar.chart->originalMd5s = std::vector<std::string>{"parent-md5"};
  lookups.originalMd5Paths = [&](std::span<const std::string> hashes) {
    ++hashLookups;
    assert(std::ranges::equal(hashes, *bar.chart->originalMd5s));
    if (hashes.empty()) return std::vector<std::filesystem::path>{};
    return std::vector<std::filesystem::path>{
        {}, temporary.path / "parent" / "original.bms"};
  };
  assert(musicSelectExplorerPath(bar, lookups) == temporary.path / "parent");
  assert(hashLookups == 1 && textLookups == 0);

  bar.chart->originalMd5s = std::vector<std::string>{};
  assert(!musicSelectExplorerPath(bar, lookups));
  assert(hashLookups == 2 && textLookups == 0);

  bar.chart->originalMd5s.reset();
  bar.title = "Title[SP ANOTHER]";
  lookups.textPaths = [&](std::string_view query) {
    ++textLookups;
    assert(query == "Title");
    return std::vector<std::filesystem::path>{
        temporary.path / "search" / "match.bms"};
  };
  assert(musicSelectExplorerPath(bar, lookups) == temporary.path / "search");
  assert(textLookups == 1);

  bar.title = "~leading";
  assert(musicSelectExplorerTitleQuery(bar.title) == "~leading");

  const auto archived = song("/charts/package.zip/nested/chart.bms", true);
  const auto splitArchive = [](const std::filesystem::path &path,
                               std::filesystem::path &archive,
                               std::filesystem::path &inner) {
    if (path != "/charts/package.zip/nested/chart.bms") return false;
    archive = "/charts/package.zip";
    inner = "nested/chart.bms";
    return true;
  };
  assert(musicSelectExplorerPath(archived, {}, splitArchive) ==
         std::filesystem::path("/charts"));
}

void testFolderAndDownloadSiteBranches() {
  MusicSelectBar folder{.kind = skin::MusicSelectBarKind::Folder,
                        .directoryPath = "/charts/folder"};
  assert(musicSelectExplorerPath(folder, {}) ==
         std::filesystem::path("/charts/folder"));

  MusicSelectBar archiveFolder{.kind = skin::MusicSelectBarKind::Folder,
                               .directoryPath = "/charts/package.zip"};
  const auto isArchive = [](const std::filesystem::path &path) {
    return path == "/charts/package.zip";
  };
  assert(musicSelectExplorerPath(archiveFolder, {}, {}, isArchive) ==
         std::filesystem::path("/charts"));

  MusicSelectBar localCourses{.kind = skin::MusicSelectBarKind::Table,
                              .title = "COURSE"};
  assert(!musicSelectDifficultyTableUpdateId(localCourses));
  MusicSelectBar importedTable{.kind = skin::MusicSelectBarKind::Table,
                               .tableId = 42,
                               .tableUrl = "https://example.test/table"};
  assert(musicSelectDifficultyTableUpdateId(importedTable) == 42);
  importedTable.tableUrl.clear();
  assert(!musicSelectDifficultyTableUpdateId(importedTable));

  auto bar = song("missing.bms", false);
  bar.chart->downloadUrl = "https://example.test/archive.zip";
  bar.chart->appendDownloadUrl = "https://example.test/patch.zip";
  assert(musicSelectDownloadUrls(bar) ==
         std::vector<std::string>({"https://example.test/archive.zip",
                                   "https://example.test/patch.zip"}));
  bar.chart->appendDownloadUrl = bar.chart->downloadUrl;
  assert(musicSelectDownloadUrls(bar) ==
         std::vector<std::string>({"https://example.test/archive.zip"}));
}

void testRefreshPathUsesThePhysicalArchive() {
  const auto splitArchive = [](const std::filesystem::path &path,
                               std::filesystem::path &archive,
                               std::filesystem::path &inner) {
    if (path != "/charts/package.zip/nested/chart.bms") return false;
    archive = "/charts/package.zip";
    inner = "nested/chart.bms";
    return true;
  };
  MusicSelectBar folder{.kind = skin::MusicSelectBarKind::Folder,
                        .directoryPath = "/charts/folder"};
  assert(musicSelectRefreshPath(folder, splitArchive) ==
         std::filesystem::path("/charts/folder"));

  MusicSelectBar archivedFolder{
      .kind = skin::MusicSelectBarKind::Folder,
      .directoryPath = "/charts/package.zip/nested"};
  const auto splitFolderArchive = [](const std::filesystem::path &path,
                                     std::filesystem::path &archive,
                                     std::filesystem::path &inner) {
    if (path != "/charts/package.zip/nested") return false;
    archive = "/charts/package.zip";
    inner = "nested";
    return true;
  };
  assert(musicSelectRefreshPath(archivedFolder, splitFolderArchive) ==
         std::filesystem::path("/charts/package.zip"));

  auto ordinary = song("/charts/folder/chart.bms", true);
  assert(musicSelectRefreshPath(ordinary, splitArchive) ==
         std::filesystem::path("/charts/folder"));

  auto archived = song("/charts/package.zip/nested/chart.bms", true);
  assert(musicSelectRefreshPath(archived, splitArchive) ==
         std::filesystem::path("/charts/package.zip"));
}

void testTouchRowsUseTheSameActivationAsDesktopPointers() {
  assert(musicSelectPointerActivatesRow(MusicSelectPointerOrigin::Mouse));
  assert(musicSelectPointerActivatesRow(MusicSelectPointerOrigin::Touch));
  assert(!musicSelectPointerKeepsCenteredBar(
      skin::MusicSelectBarKind::Folder));
  assert(musicSelectPointerKeepsCenteredBar(
      skin::MusicSelectBarKind::Song));
  assert(musicSelectPointerKeepsCenteredBar(
      skin::MusicSelectBarKind::Grade));
}

void testTouchGestureDefersBarTapAndConvertsVerticalDragToRows() {
  MusicSelectTouchGesture gesture;
  assert(gesture.begin(7, 0.50F, 0.50F, MusicSelectTouchTarget::Bar));
  const auto belowThreshold = gesture.move(7, 0.50F, 0.48F);
  assert(belowThreshold.accepted && belowThreshold.rowDelta == 0 &&
         !belowThreshold.sliderDrag);
  const auto upwards = gesture.move(7, 0.50F, 0.39F);
  assert(upwards.accepted && upwards.rowDelta == 2 &&
         !upwards.sliderDrag);
  const auto release = gesture.end(7);
  assert(release.accepted && !release.tap);

  assert(gesture.begin(8, 0.50F, 0.25F, MusicSelectTouchTarget::Bar));
  const auto tap = gesture.end(8);
  assert(tap.accepted && tap.tap);
}

void testTouchGestureCapturesOnlyItsFingerAndRoutesSliderMotion() {
  MusicSelectTouchGesture gesture;
  assert(gesture.begin(11, 0.50F, 0.20F, MusicSelectTouchTarget::Slider));
  assert(!gesture.begin(12, 0.50F, 0.20F, MusicSelectTouchTarget::Bar));
  assert(!gesture.move(12, 0.50F, 0.30F).accepted);
  const auto slider = gesture.move(11, 0.50F, 0.30F);
  assert(slider.accepted && slider.sliderDrag && slider.rowDelta == 0);
  assert(!gesture.end(12).accepted);
  assert(gesture.end(11).accepted);
}

void testTouchGestureUsesAHorizontalLeftEdgeSwipeToGoBack() {
  MusicSelectTouchGesture gesture;
  assert(!gesture.begin(21, 0.25F, 0.50F,
                        MusicSelectTouchTarget::Navigation));
  assert(gesture.begin(21, 0.05F, 0.50F,
                       MusicSelectTouchTarget::Navigation));
  const auto moved = gesture.move(21, 0.20F, 0.52F);
  assert(moved.accepted && !moved.sliderDrag && moved.rowDelta == 0);
  const auto release = gesture.end(21);
  assert(release.accepted && !release.tap && release.goBack);

  assert(gesture.begin(22, 0.05F, 0.50F,
                       MusicSelectTouchTarget::Navigation));
  (void)gesture.move(22, 0.20F, 0.75F);
  assert(!gesture.end(22).goBack);
}

} // namespace

int main() {
  testDocumentSelectionMatchesFilesListFilter();
  testArchivedDocumentsUseMaterializedResolverPaths();
  testExplorerBranchPriority();
  testFolderAndDownloadSiteBranches();
  testRefreshPathUsesThePhysicalArchive();
  testTouchRowsUseTheSameActivationAsDesktopPointers();
  testTouchGestureDefersBarTapAndConvertsVerticalDragToRows();
  testTouchGestureCapturesOnlyItsFingerAndRoutesSliderMotion();
  testTouchGestureUsesAHorizontalLeftEdgeSwipeToGoBack();
  return 0;
}
