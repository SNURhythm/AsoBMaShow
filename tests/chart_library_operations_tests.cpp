#include "library/ChartLibraryOperations.h"
#include "library/ChartLibraryPlatform.h"
#include "bms_parser.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    ++failures;
  }
}

class TempDirectory {
public:
  TempDirectory() {
    static std::atomic<unsigned long long> sequence{0};
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("asobmashow-library-operations-" + std::to_string(nonce) + "-" +
             std::to_string(sequence.fetch_add(1)));
    std::filesystem::create_directories(path_);
  }

  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

std::filesystem::path writeChart(
    const std::filesystem::path &root,
    const std::filesystem::path &filename = "operation.bms") {
  std::filesystem::create_directories(root);
  const auto chartPath = root / filename;
  std::ofstream chart(chartPath);
  chart << "#PLAYER 1\n"
           "#GENRE Test\n"
           "#TITLE Shared Operations\n"
           "#ARTIST AsoBMaShow Test\n"
           "#BPM 120\n"
           "#PLAYLEVEL 1\n"
           "#RANK 2\n"
           "#TOTAL 100\n"
           "#WAV01 sample.wav\n"
           "#00111:01\n";
  chart.close();
  std::ofstream(root / "sample.wav", std::ios::binary).close();
  return chartPath;
}

main_menu_library::FindBmsChartIdentity
readChartIdentity(const std::filesystem::path &chartPath) {
  bms_parser::Parser parser;
  bms_parser::Chart *parsed = nullptr;
  std::atomic_bool cancelled = false;
  parser.Parse(chartPath, &parsed, false, true, cancelled);
  std::unique_ptr<bms_parser::Chart> chart(parsed);
  return chart ? main_menu_library::findBmsChartIdentity(chart->Meta)
               : main_menu_library::FindBmsChartIdentity{};
}

chart_library_tasks::ChartLibraryOperationsDependencies
dependencies(ChartRepository &repository, const std::filesystem::path &root,
             bool &reloadRequested) {
  return {
      .repository = repository,
      .tablesDirectory = root / "tables",
      .defaultDifficultyTablesSeeded = [] { return true; },
      .setDefaultDifficultyTablesSeeded = [](bool) {},
      .saveSettings = [] { return true; },
      .requestReload = [&](bool includeFolders) {
        reloadRequested = includeFolders;
      },
  };
}

void testRefreshStopsAtTheExistingPauseCheckpoint() {
  TempDirectory temporary;
  const auto libraryRoot = temporary.path() / "library";
  writeChart(libraryRoot);
  ChartRepository repository(temporary.path() / "chart.db");
  expect(repository.EnsureReady(), "temporary chart repository is ready");
  bool reloadRequested = false;
  chart_library_tasks::ChartLibraryOperations operations(
      dependencies(repository, temporary.path(), reloadRequested));
  std::stop_source stop;
  int pauseCalls = 0;

  const auto result = operations.run(
      {.kind = chart_library_tasks::TaskKind::RefreshLibrary,
       .title = "Refresh Library",
       .folderToAdd = libraryRoot},
      stop.get_token(), [](const ChartScanProgress &, std::string_view) {},
      [&] {
        ++pauseCalls;
        return false;
      });

  expect(result.disposition ==
             chart_library_tasks::TaskRunDisposition::Paused,
         "false pause checkpoint returns a paused task");
  expect(pauseCalls == 1, "refresh consults the first pause checkpoint once");
  auto session = repository.OpenSession();
  expect(session.has_value() && session->CountAllChartMeta() == 0,
         "paused refresh does not mutate the chart library");
  expect(!reloadRequested, "paused refresh does not publish a reload");
}

void testRefreshScansThroughTheRealRepository() {
  TempDirectory temporary;
  const auto libraryRoot = temporary.path() / "library";
  writeChart(libraryRoot);
  ChartRepository repository(temporary.path() / "chart.db");
  expect(repository.EnsureReady(), "scan repository is ready");
  bool reloadRequested = false;
  chart_library_tasks::ChartLibraryOperations operations(
      dependencies(repository, temporary.path(), reloadRequested));
  std::vector<ChartScanProgress> progress;
  const auto before = repository.GetLibraryRevision();

  const auto result = operations.run(
      {.kind = chart_library_tasks::TaskKind::RefreshLibrary,
       .title = "Refresh Library",
       .folderToAdd = libraryRoot},
      std::stop_token{},
      [&](const ChartScanProgress &value, std::string_view) {
        progress.push_back(value);
      },
      [] { return true; });

  expect(result.disposition ==
             chart_library_tasks::TaskRunDisposition::Complete,
         "refresh completes through the shared operation");
  auto session = repository.OpenSession();
  expect(session.has_value() && session->CountAllChartMeta() == 1,
         "refresh indexes the real BMS fixture");
  std::vector<ChartMetaRecord> records;
  if (session.has_value()) session->QueryChartMeta({}, records);
  expect(records.size() == 1 && records.front().addDateSeconds > 0,
         "first scan persists Beatoraja SongData adddate seconds");
  expect(repository.GetLibraryRevision() > before,
         "refresh advances the repository library revision");
  expect(!progress.empty(), "refresh publishes scanner progress");
  expect(reloadRequested, "completed refresh requests selector reload");
}

void testAddingFolderRefreshesAccessForEveryEffectiveEntry() {
  TempDirectory temporary;
  const auto existingRoot = temporary.path() / "existing";
  const auto addedRoot = temporary.path() / "added";
  std::filesystem::create_directories(existingRoot);
  writeChart(addedRoot);
  ChartRepository repository(temporary.path() / "chart.db");
  expect(repository.EnsureReady(), "folder access repository is ready");
  auto session = repository.OpenSession();
  expect(session.has_value() && session->InsertEntry(existingRoot, "old-bookmark"),
         "existing external folder is registered");
  bool reloadRequested = false;
  std::vector<ChartEntry> refreshedEntries;
  auto deps = dependencies(repository, temporary.path(), reloadRequested);
  deps.refreshFolderAccess = [&](const std::vector<ChartEntry> &entries) {
    refreshedEntries = entries;
  };
  chart_library_tasks::ChartLibraryOperations operations(std::move(deps));

  const auto result = operations.run(
      {.kind = chart_library_tasks::TaskKind::RefreshLibrary,
       .title = "Add Folder",
       .folderToAdd = addedRoot,
       .iosBookmark = "new-bookmark"},
      std::stop_token{}, [](const ChartScanProgress &, std::string_view) {},
      [] { return true; });

  expect(result.disposition ==
             chart_library_tasks::TaskRunDisposition::Complete,
         "adding another folder completes");
  expect(refreshedEntries.size() == 2,
         "security access refresh receives every effective folder");
  expect(std::ranges::any_of(refreshedEntries, [&](const ChartEntry &entry) {
           return std::filesystem::path(entry.path).lexically_normal() ==
                      existingRoot.lexically_normal() &&
                  entry.iosBookmark == "old-bookmark";
         }),
         "security access refresh retains the existing folder bookmark");
  expect(std::ranges::any_of(refreshedEntries, [&](const ChartEntry &entry) {
           return std::filesystem::path(entry.path).lexically_normal() ==
                      addedRoot.lexically_normal() &&
                  entry.iosBookmark == "new-bookmark";
         }),
         "security access refresh includes the newly added folder bookmark");
}

void testPathRefreshReconcilesOnlyTheRequestedSubtree() {
  TempDirectory temporary;
  const auto libraryRoot = temporary.path() / "library";
  const auto targetRoot = libraryRoot / "target";
  const auto untouchedRoot = libraryRoot / "untouched";
  const auto staleTargetChart = writeChart(targetRoot);
  const auto untouchedChart = writeChart(untouchedRoot);
  ChartRepository repository(temporary.path() / "chart.db");
  expect(repository.EnsureReady(), "targeted refresh repository is ready");
  bool reloadRequested = false;
  chart_library_tasks::ChartLibraryOperations operations(
      dependencies(repository, temporary.path(), reloadRequested));

  const auto initial = operations.run(
      {.kind = chart_library_tasks::TaskKind::RefreshLibrary,
       .title = "Refresh Library",
       .folderToAdd = libraryRoot},
      std::stop_token{},
      [](const ChartScanProgress &, std::string_view) {}, [] { return true; });
  expect(initial.disposition ==
             chart_library_tasks::TaskRunDisposition::Complete,
         "initial full refresh completes");

  std::filesystem::remove(staleTargetChart);
  std::filesystem::remove(untouchedChart);
  const auto replacementChart = writeChart(targetRoot, "replacement.bms");
  reloadRequested = false;
  const auto targeted = operations.run(
      {.kind = chart_library_tasks::TaskKind::RefreshPath,
       .title = "Update Folder",
       .refreshPath = targetRoot},
      std::stop_token{},
      [](const ChartScanProgress &, std::string_view) {}, [] { return true; });

  expect(targeted.disposition ==
             chart_library_tasks::TaskRunDisposition::Complete,
         "targeted path refresh completes");
  auto session = repository.OpenSession();
  std::vector<bms_parser::ChartMeta> charts;
  if (session.has_value()) {
    session->SelectAllChartMeta(charts);
  }
  const auto hasPath = [&](const std::filesystem::path &path) {
    return std::ranges::any_of(charts, [&](const auto &chart) {
      return chart.BmsPath.lexically_normal() == path.lexically_normal();
    });
  };
  expect(charts.size() == 2,
         "targeted refresh keeps records outside the requested subtree");
  expect(!hasPath(staleTargetChart),
         "targeted refresh removes a stale chart inside its subtree");
  expect(hasPath(replacementChart),
         "targeted refresh indexes a replacement chart inside its subtree");
  expect(hasPath(untouchedChart),
         "targeted refresh does not reconcile an unvisited sibling subtree");
  const auto entries =
      session.has_value() ? session->SelectAllEntries()
                          : std::vector<ChartEntry>{};
  expect(entries.size() == 1 &&
             std::filesystem::path(entries.front().path).lexically_normal() ==
                 libraryRoot.lexically_normal(),
         "targeted refresh does not register its subtree as a library root");
  expect(reloadRequested, "targeted refresh requests selector reload");
}

void testDownloadedPathIndexesAndReturnsTheSelectionHandoff() {
  TempDirectory temporary;
  const auto downloadedRoot = temporary.path() / "downloaded";
  const auto chartPath = writeChart(downloadedRoot);
  const auto identity = readChartIdentity(chartPath);
  expect(identity.valid(), "download fixture has a stable chart identity");
  ChartRepository repository(temporary.path() / "chart.db");
  expect(repository.EnsureReady(), "download repository is ready");
  bool reloadRequested = false;
  chart_library_tasks::ChartLibraryOperations operations(
      dependencies(repository, temporary.path(), reloadRequested));

  chart_library_tasks::TaskRunResult result;
  try {
    result = operations.run(
        {.kind = chart_library_tasks::TaskKind::IndexDownloadedPath,
         .title = "Index downloaded charts",
         .downloadedPath = downloadedRoot,
         .downloadedTargetIdentity = identity,
         .downloadedSelectionGeneration = 9},
        std::stop_token{},
        [](const ChartScanProgress &, std::string_view) {}, [] { return true; });
  } catch (const std::exception &error) {
    expect(false, std::string("download indexing threw: ") + error.what());
    return;
  }

  expect(result.disposition ==
             chart_library_tasks::TaskRunDisposition::Complete,
         "download indexing completes through the shared operation");
  expect(result.downloadedIndex.has_value(),
         "download indexing returns a selection handoff");
  if (result.downloadedIndex.has_value()) {
    expect(result.downloadedIndex->chartPath.lexically_normal() ==
               chartPath.lexically_normal(),
           "selection handoff identifies the indexed chart path");
    expect(result.downloadedIndex->targetIdentity.sha256 == identity.sha256,
           "selection handoff retains the requested identity");
    expect(result.downloadedIndex->selectionGeneration == 9,
           "selection handoff retains the captured selection generation");
  }
  auto session = repository.OpenSession();
  expect(session.has_value() && session->CountAllChartMeta() == 1,
         "download indexing commits the BMS fixture");
  expect(reloadRequested, "download indexing requests selector reload");
}

void testRefreshSeedsTheExactDefaultTablesOnce() {
  TempDirectory temporary;
  ChartRepository repository(temporary.path() / "chart.db");
  expect(repository.EnsureReady(), "table seed repository is ready");
  bool reloadRequested = false;
  bool seeded = false;
  int saveCalls = 0;
  std::vector<std::string> importedUrls;
  auto deps = dependencies(repository, temporary.path(), reloadRequested);
  deps.defaultDifficultyTablesSeeded = [&] { return seeded; };
  deps.setDefaultDifficultyTablesSeeded = [&](bool value) { seeded = value; };
  deps.saveSettings = [&] {
    ++saveCalls;
    return true;
  };
  deps.importDifficultyTableFromUrl =
      [&](ChartRepository::Session &, const std::string &url,
          std::string *, DifficultyTableImportProgressCallback) {
        importedUrls.push_back(url);
        return true;
      };
  deps.importDifficultyTablesFromDirectory =
      [](ChartRepository::Session &, const std::filesystem::path &) {
        return 0;
      };
  chart_library_tasks::ChartLibraryOperations operations(std::move(deps));

  const auto result = operations.run(
      {.kind = chart_library_tasks::TaskKind::RefreshLibrary,
       .title = "Refresh Library"},
      std::stop_token{}, [](const ChartScanProgress &, std::string_view) {},
      [] { return true; });

  const std::vector<std::string> expectedUrls = {
      "https://rattoto10.jounin.jp/table.html",
      "https://rattoto10.jounin.jp/table_insane.html",
      "https://stellabms.xyz/sl/table.html",
      "https://stellabms.xyz/st/table.html",
  };
  expect(result.disposition ==
             chart_library_tasks::TaskRunDisposition::Complete,
         "refresh completes after default table seeding");
  expect(importedUrls == expectedUrls,
         "refresh imports the exact default table URLs in order");
  expect(seeded, "all successful default imports persist the seeded flag");
  expect(saveCalls == 1, "successful default imports save settings once");
  expect(reloadRequested,
         "successful default table imports request selector reload");
}

void testDifficultyTableUpdateUsesTheSharedTaskOperation() {
  TempDirectory temporary;
  ChartRepository repository(temporary.path() / "chart.db");
  expect(repository.EnsureReady(), "table update repository is ready");
  bool reloadRequested = false;
  int reloadCalls = 0;
  int updatedTableId = 0;
  auto deps = dependencies(repository, temporary.path(), reloadRequested);
  deps.requestReload = [&](bool includeFolders) {
    ++reloadCalls;
    reloadRequested = includeFolders;
  };
  deps.updateDifficultyTableFromSourceUrl =
      [&](ChartRepository::Session &, int tableId, std::string *) {
        updatedTableId = tableId;
        return true;
      };
  chart_library_tasks::ChartLibraryOperations operations(std::move(deps));

  const auto result = operations.run(
      {.kind = chart_library_tasks::TaskKind::UpdateDifficultyTable,
       .title = "Update Difficulty Table",
       .tableId = 47},
      std::stop_token{}, [](const ChartScanProgress &, std::string_view) {},
      [] { return true; });

  expect(result.disposition ==
             chart_library_tasks::TaskRunDisposition::Complete,
         "difficulty table update completes through the shared operation");
  expect(updatedTableId == 47,
         "difficulty table update retains the selected TableBar identity");
  expect(reloadCalls == 1 && !reloadRequested,
         "difficulty table update requests a list-only selector reload");
}

void testDesktopLibraryEntryResolutionPreservesTheStoredPath() {
  const ChartEntry entry{.path = fspath_to_path_t("/tmp/library-entry")};
  expect(chart_library_platform::resolveFolderEntryPath(entry) ==
             std::filesystem::path("/tmp/library-entry"),
         "desktop scanning uses the stored library entry path");
}

} // namespace

int main() {
  testRefreshStopsAtTheExistingPauseCheckpoint();
  testRefreshScansThroughTheRealRepository();
  testAddingFolderRefreshesAccessForEveryEffectiveEntry();
  testPathRefreshReconcilesOnlyTheRequestedSubtree();
  testDownloadedPathIndexesAndReturnsTheSelectionHandoff();
  testRefreshSeedsTheExactDefaultTablesOnce();
  testDifficultyTableUpdateUsesTheSharedTaskOperation();
  testDesktopLibraryEntryResolutionPreservesTheStoredPath();
  if (failures != 0) {
    std::cerr << failures << " chart library operation test(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "chart library operation tests passed\n";
  return EXIT_SUCCESS;
}
