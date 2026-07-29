#include "../src/ArchiveRAII.h"
#include "../src/ArchiveFile.h"
#include "../src/ChartLibraryScanner.h"
#include "../src/Utils.h"
#include "../src/repositories/ChartRepository.h"
#include "../src/sqlite3.h"

#include <archive_entry.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

class TempDirectory {
public:
  TempDirectory() {
    static std::atomic<unsigned long long> sequence{0};
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("asobmashow-chart-scanner-" + std::to_string(nonce) + "-" +
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

using ArchiveEntryHandle =
    std::unique_ptr<archive_entry, decltype(&archive_entry_free)>;

std::string chartText(const std::string &title) {
  return "#PLAYER 1\n"
         "#GENRE Test\n"
         "#TITLE " +
         title +
         "\n#ARTIST AsoBMaShow Test\n"
         "#BPM 120\n"
         "#PLAYLEVEL 1\n"
         "#RANK 2\n"
         "#TOTAL 100\n"
         "#WAV01 sample.wav\n"
         "#00111:01\n";
}

std::filesystem::path writeChart(const std::filesystem::path &root,
                                 const std::string &name,
                                 const std::string &title) {
  std::filesystem::create_directories(root);
  const auto chartPath = root / (name + ".bms");
  {
    std::ofstream chart(chartPath);
    chart << chartText(title);
  }
  {
    std::ofstream audio(root / "sample.wav", std::ios::binary);
  }
  return chartPath;
}

std::filesystem::path
writeZip(const std::filesystem::path &path,
         const std::vector<std::pair<std::string, std::string>> &files) {
  std::filesystem::create_directories(path.parent_path());
  auto writer = makeArchiveWriteHandle();
  assert(writer);
  assert(archive_write_set_format_zip(writer.get()) == ARCHIVE_OK);
  assert(archive_write_set_options(writer.get(), "zip:compression=store") ==
         ARCHIVE_OK);
  assert(archive_write_open_filename(writer.get(), path.string().c_str()) ==
         ARCHIVE_OK);

  for (const auto &[entryPath, contents] : files) {
    ArchiveEntryHandle entry(archive_entry_new(), archive_entry_free);
    assert(entry);
    archive_entry_set_pathname(entry.get(), entryPath.c_str());
    archive_entry_set_filetype(entry.get(), AE_IFREG);
    archive_entry_set_perm(entry.get(), 0644);
    archive_entry_set_size(entry.get(),
                           static_cast<la_int64_t>(contents.size()));
    assert(archive_write_header(writer.get(), entry.get()) == ARCHIVE_OK);
    assert(archive_write_data(writer.get(), contents.data(), contents.size()) ==
           static_cast<la_ssize_t>(contents.size()));
    assert(archive_write_finish_entry(writer.get()) == ARCHIVE_OK);
  }
  assert(archive_write_close(writer.get()) == ARCHIVE_OK);
  return path;
}

void testBasicNoOpAndDeleteScan() {
  TempDirectory temporary;
  const auto root = temporary.path() / "library";
  const auto chartPath = writeChart(root, "sample", "Repository Scanner");

  ChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());

  ChartLibraryScanner scanner;
  std::vector<ChartScanProgress> progress;
  const std::uint64_t beforeRevision = repository.GetLibraryRevision();
  const int changed = scanner.Scan(
      *session, {root}, nullptr,
      [&](const ChartScanProgress &value) { progress.push_back(value); });
  assert(changed == 1);
  assert(session->CountAllChartMeta() == 1);
  assert(repository.GetLibraryRevision() > beforeRevision);
  assert(!progress.empty());
  assert(progress.front().stage == ChartScanProgressStage::Preparing);

  const std::uint64_t stableRevision = repository.GetLibraryRevision();
  assert(scanner.Scan(*session, {root}) == 0);
  assert(repository.GetLibraryRevision() == stableRevision);

  std::filesystem::remove(chartPath);
  assert(scanner.Scan(*session, {root}) == 1);
  assert(session->CountAllChartMeta() == 0);
  assert(repository.GetLibraryRevision() > stableRevision);
}

void testStopAndPauseBeforeWork() {
  TempDirectory temporary;
  const auto root = temporary.path() / "library";
  writeChart(root, "sample", "Stopped Scanner");

  ChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());
  ChartLibraryScanner scanner;

  std::stop_source stopped;
  stopped.request_stop();
  const auto stoppedToken = stopped.get_token();
  assert(scanner.Scan(*session, {root}, &stoppedToken) == 0);
  assert(session->CountAllChartMeta() == 0);

  assert(scanner.Scan(*session, {root}, nullptr, nullptr,
                      []() { return false; }) == 0);
  assert(session->CountAllChartMeta() == 0);
}

void testCheckpointResume() {
  TempDirectory temporary;
  const auto root = temporary.path() / "library";
  writeChart(root, "first", "First Scanner Chart");
  writeChart(root, "second", "Second Scanner Chart");

  ChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());
  ChartLibraryScanner scanner;

  std::stop_source stop;
  const auto stopToken = stop.get_token();
  const int firstChanged = scanner.Scan(
      *session, {root}, &stopToken, nullptr, nullptr,
      []() -> std::uint64_t { return 1; },
      [&](std::uint64_t request) {
        assert(request == 1);
        stop.request_stop();
      });
  assert(firstChanged == 1);
  assert(session->CountAllChartMeta() == 1);
  assert(session->LoadScanSnapshot().checkpoint.has_value());

  assert(scanner.Scan(*session, {root}) == 1);
  assert(session->CountAllChartMeta() == 2);
  assert(!session->LoadScanSnapshot().checkpoint.has_value());
}

std::atomic_bool denyChartInsert{false};

int denyChartInsertAuthorizer(void *, int action, const char *first,
                              const char *, const char *, const char *) {
  if (denyChartInsert.load(std::memory_order_relaxed) &&
      action == SQLITE_INSERT && first != nullptr &&
      std::string_view(first) == "chart_meta") {
    return SQLITE_DENY;
  }
  return SQLITE_OK;
}

int installDenyChartInsert(sqlite3 *database, char **,
                           const sqlite3_api_routines *) {
  return sqlite3_set_authorizer(database, denyChartInsertAuthorizer, nullptr);
}

class ScopedInsertDenial {
public:
  ScopedInsertDenial() {
    sqlite3_reset_auto_extension();
    assert(sqlite3_auto_extension(reinterpret_cast<void (*)()>(
               installDenyChartInsert)) == SQLITE_OK);
  }

  ~ScopedInsertDenial() {
    denyChartInsert.store(false, std::memory_order_relaxed);
    sqlite3_reset_auto_extension();
  }
};

std::atomic_bool countPostInsertChartPathReads{false};
std::atomic_bool observedChartInsert{false};
std::atomic_int postInsertChartPathReads{0};

int countPostInsertChartPathReadAuthorizer(void *, int action,
                                           const char *first,
                                           const char *second, const char *,
                                           const char *) {
  if (!countPostInsertChartPathReads.load(std::memory_order_relaxed)) {
    return SQLITE_OK;
  }
  if (action == SQLITE_INSERT && first != nullptr &&
      std::string_view(first) == "chart_meta") {
    observedChartInsert.store(true, std::memory_order_relaxed);
  } else if (action == SQLITE_READ && first != nullptr && second != nullptr &&
             std::string_view(first) == "chart_meta" &&
             std::string_view(second) == "path" &&
             observedChartInsert.load(std::memory_order_relaxed)) {
    postInsertChartPathReads.fetch_add(1, std::memory_order_relaxed);
  }
  return SQLITE_OK;
}

int installPostInsertChartPathReadCounter(sqlite3 *database, char **,
                                          const sqlite3_api_routines *) {
  return sqlite3_set_authorizer(database, countPostInsertChartPathReadAuthorizer,
                                nullptr);
}

class ScopedPostInsertChartPathReadCounter {
public:
  ScopedPostInsertChartPathReadCounter() {
    sqlite3_reset_auto_extension();
    assert(sqlite3_auto_extension(reinterpret_cast<void (*)()>(
               installPostInsertChartPathReadCounter)) == SQLITE_OK);
  }

  ~ScopedPostInsertChartPathReadCounter() {
    countPostInsertChartPathReads.store(false, std::memory_order_relaxed);
    observedChartInsert.store(false, std::memory_order_relaxed);
    postInsertChartPathReads.store(0, std::memory_order_relaxed);
    sqlite3_reset_auto_extension();
  }

  void start() {
    observedChartInsert.store(false, std::memory_order_relaxed);
    postInsertChartPathReads.store(0, std::memory_order_relaxed);
    countPostInsertChartPathReads.store(true, std::memory_order_relaxed);
  }

  int stop() {
    countPostInsertChartPathReads.store(false, std::memory_order_relaxed);
    assert(observedChartInsert.load(std::memory_order_relaxed));
    return postInsertChartPathReads.load(std::memory_order_relaxed);
  }
};

void testStorageFailureLeavesNoChart() {
  TempDirectory temporary;
  const auto root = temporary.path() / "library";
  writeChart(root, "sample", "Denied Scanner");
  ScopedInsertDenial denial;

  ChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());
  denyChartInsert.store(true, std::memory_order_relaxed);

  ChartLibraryScanner scanner;
  assert(scanner.Scan(*session, {root}) == 0);
  assert(session->CountAllChartMeta() == 0);
}

void testMixedOrdinaryAndArchiveEntitiesIndexExactlyOnce() {
  TempDirectory temporary;
  const auto root = temporary.path() / "library";
  const auto archiveA = writeZip(root / "00-archive.zip",
                                 {{"inside-a.bms", chartText("Archive A")}});
  const auto ordinaryA = writeChart(root, "10-ordinary-a", "Ordinary A");
  const auto ordinaryB = writeChart(root, "20-ordinary-b", "Ordinary B");
  const auto archiveB = writeZip(root / "30-archive.zip",
                                 {{"inside-b.bms", chartText("Archive B")}});

  ChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());
  ChartLibraryScanner scanner;

  const std::vector<std::filesystem::path> roots{archiveA, ordinaryA, ordinaryB,
                                                 archiveB};
  assert(scanner.Scan(*session, roots) == 6);
  assert(session->CountAllChartMeta() == 4);
  const ChartScanSnapshot snapshot = session->LoadScanSnapshot();
  assert(snapshot.archiveCache.size() == 2);
  assert(scanner.Scan(*session, roots) == 0);
  assert(session->CountAllChartMeta() == 4);
}

void testManySmallArchivesPreserveDiscoveryOrderAndCache() {
  constexpr int kArchiveCount = 6;
  TempDirectory temporary;
  const auto root = temporary.path() / "library";
  std::vector<std::filesystem::path> roots;
  std::map<std::string, std::string> expectedArchiveByTitle;
  for (int index = 0; index < kArchiveCount; ++index) {
    const std::string stem = "archive-" + std::to_string(index);
    const std::string title = "Archive Chart " + std::to_string(index);
    const auto archivePath = writeZip(
        root / (stem + ".zip"),
        {{"inside-" + std::to_string(index) + ".bms", chartText(title)}});
    roots.push_back(archivePath);
    expectedArchiveByTitle.emplace(title, archivePath.filename().string());
  }

  ChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());
  ChartLibraryScanner scanner;

  assert(scanner.Scan(*session, roots) == kArchiveCount * 2);
  assert(session->CountAllChartMeta() == kArchiveCount);
  const ChartScanSnapshot snapshot = session->LoadScanSnapshot();
  assert(snapshot.archiveCache.size() == kArchiveCount);
  assert(snapshot.charts.size() == kArchiveCount);
  for (const auto &chart : snapshot.charts) {
    const auto expected = expectedArchiveByTitle.find(chart.Title);
    assert(expected != expectedArchiveByTitle.end());
    assert(chart.BmsPath.generic_string().find(expected->second) !=
           std::string::npos);
  }
  assert(scanner.Scan(*session, roots) == 0);
}

void testNormalArchiveScanDoesNotRecountStoredRows() {
  constexpr int kArchiveCount = 6;
  TempDirectory temporary;
  const auto root = temporary.path() / "library";
  std::vector<std::filesystem::path> roots;
  for (int index = 0; index < kArchiveCount; ++index) {
    roots.push_back(writeZip(
        root / ("count-reuse-" + std::to_string(index) + ".zip"),
        {{"chart.bms", chartText("Count Reuse " + std::to_string(index))}}));
  }

  ScopedPostInsertChartPathReadCounter readCounter;
  ChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());
  ChartLibraryScanner scanner;

  readCounter.start();
  assert(scanner.Scan(*session, roots) == kArchiveCount * 2);
  assert(readCounter.stop() == 0);

  const ChartScanSnapshot snapshot = session->LoadScanSnapshot();
  assert(snapshot.charts.size() == kArchiveCount);
  assert(snapshot.archiveCache.size() == kArchiveCount);
  for (const auto &cache : snapshot.archiveCache) {
    assert(cache.chartCount == 1);
  }
}

void testMultiEntryArchivePreservesPreparedResultOrderAndCache() {
  TempDirectory temporary;
  const auto root = temporary.path() / "library";
  const auto archivePath =
      writeZip(root / "multi-chart.zip",
               {{"alpha/first.bms", chartText("First Archive Entry")},
                {"beta/second.bms", chartText("Second Archive Entry")},
                {"gamma/third.bms", chartText("Third Archive Entry")},
                {"readme.txt", "not a chart"}});

  ChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());
  ChartLibraryScanner scanner;

  assert(scanner.Scan(*session, {archivePath}) == 4);
  const ChartScanSnapshot snapshot = session->LoadScanSnapshot();
  assert(snapshot.charts.size() == 3);
  assert(snapshot.archiveCache.size() == 1);
  assert(snapshot.archiveCache.front().chartCount == 3);

  const std::set<std::string> expectedInnerPaths{
      "alpha/first.bms",
      "beta/second.bms",
      "gamma/third.bms",
  };
  std::set<std::string> storedInnerPaths;
  for (const auto &chart : snapshot.charts) {
    const std::string storedPath = chart.BmsPath.generic_string();
    for (const auto &expected : expectedInnerPaths) {
      if (storedPath.find(expected) != std::string::npos) {
        storedInnerPaths.insert(expected);
      }
    }
  }
  assert(storedInnerPaths == expectedInnerPaths);
  assert(scanner.Scan(*session, {archivePath}) == 0);
}

void testArchiveCheckpointResumeUsesOrderedFallbackPipeline() {
  TempDirectory temporary;
  const auto root = temporary.path() / "library";
  const auto archivePath =
      writeZip(root / "resume-archive.zip",
               {{"first.bms", chartText("Resume Archive First")},
                {"second.bms", chartText("Resume Archive Second")},
                {"third.bms", chartText("Resume Archive Third")}});

  ChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());
  ChartLibraryScanner scanner;

  std::stop_source stop;
  const auto stopToken = stop.get_token();
  (void)scanner.Scan(
      *session, {archivePath}, &stopToken, nullptr, nullptr,
      []() -> std::uint64_t { return 1; },
      [&](std::uint64_t request) {
        assert(request == 1);
        stop.request_stop();
      });
  assert(session->CountAllChartMeta() == 0);
  assert(session->LoadScanSnapshot().checkpoint.has_value());

  assert(scanner.Scan(*session, {archivePath}) == 4);
  assert(session->CountAllChartMeta() == 3);
  const ChartScanSnapshot snapshot = session->LoadScanSnapshot();
  assert(snapshot.archiveCache.size() == 1);
  assert(snapshot.archiveCache.front().chartCount == 3);
  assert(!snapshot.checkpoint.has_value());
}

void testMidArchiveCheckpointResumePreservesValidCacheCount() {
  constexpr int kCandidateCount = 105;
  constexpr int kValidCount = 104;
  TempDirectory temporary;
  const auto root = temporary.path() / "library";
  std::vector<std::pair<std::string, std::string>> files;
  files.reserve(kCandidateCount);
  files.emplace_back("chart-0.bms",
                     "#PLAYER 1\n#TITLE Invalid Candidate\n#BPM 120\n");
  for (int index = 1; index < kCandidateCount; ++index) {
    files.emplace_back("chart-" + std::to_string(index) + ".bms",
                       chartText("Resume Valid " + std::to_string(index)));
  }
  const auto archivePath =
      writeZip(root / "mid-archive-resume.zip", files);

  ChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());
  ChartLibraryScanner scanner;

  std::stop_source stop;
  const auto stopToken = stop.get_token();
  int parsingCurrent = 0;
  (void)scanner.Scan(
      *session, {archivePath}, &stopToken,
      [&](const ChartScanProgress &progress) {
        if (progress.stage == ChartScanProgressStage::ParsingCharts) {
          parsingCurrent = progress.current;
        }
      },
      nullptr,
      [&]() -> std::uint64_t { return parsingCurrent >= 99 ? 1 : 0; },
      [&](std::uint64_t request) {
        assert(request == 1);
        stop.request_stop();
      });
  assert(session->CountAllChartMeta() == 99);
  const ChartScanSnapshot interrupted = session->LoadScanSnapshot();
  assert(interrupted.checkpoint.has_value());
  assert(interrupted.checkpoint->subIndex == 100);

  (void)scanner.Scan(*session, {archivePath});
  assert(session->CountAllChartMeta() == kValidCount);
  const ChartScanSnapshot resumed = session->LoadScanSnapshot();
  assert(resumed.archiveCache.size() == 1);
  assert(resumed.archiveCache.front().chartCount == kValidCount);
  assert(!resumed.checkpoint.has_value());
}

void testLargeSingleArchivePreservesAllChartResults() {
  constexpr int kChartCount = 24;
  TempDirectory temporary;
  const auto root = temporary.path() / "library";
  std::vector<std::pair<std::string, std::string>> files;
  files.reserve(kChartCount);
  for (int index = 0; index < kChartCount; ++index) {
    files.emplace_back("charts/chart-" + std::to_string(index) + ".bms",
                       chartText("Large Archive " + std::to_string(index)));
  }
  const auto archivePath = writeZip(root / "large-single-archive.zip", files);

  ChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());
  ChartLibraryScanner scanner;

  assert(scanner.Scan(*session, {archivePath}) == kChartCount + 1);
  const ChartScanSnapshot snapshot = session->LoadScanSnapshot();
  assert(snapshot.charts.size() == kChartCount);
  assert(snapshot.archiveCache.size() == 1);
  assert(snapshot.archiveCache.front().chartCount == kChartCount);
  assert(scanner.Scan(*session, {archivePath}) == 0);
}

void testArchiveResultApplicationOverlapsLaterArchiveStreaming() {
  constexpr int kFirstChartCount = 16;
  constexpr int kSecondChartCount = 128;
  TempDirectory temporary;
  const auto root = temporary.path() / "library";

  std::vector<std::pair<std::string, std::string>> firstFiles;
  firstFiles.reserve(kFirstChartCount);
  for (int index = 0; index < kFirstChartCount; ++index) {
    firstFiles.emplace_back("first/chart-" + std::to_string(index) + ".bms",
                            chartText("First Archive " +
                                      std::to_string(index)));
  }
  const auto firstArchive =
      writeZip(root / "ordered-drain-first.zip", firstFiles);

  std::vector<std::pair<std::string, std::string>> secondFiles;
  secondFiles.reserve(kSecondChartCount);
  const std::string padding = "#COMMENT " + std::string(128 * 1024, 'x') + "\n";
  for (int index = 0; index < kSecondChartCount; ++index) {
    secondFiles.emplace_back(
        "second/chart-" + std::to_string(index) + ".bms",
        chartText("Second Archive " + std::to_string(index)) + padding);
  }
  const auto secondArchive =
      writeZip(root / "ordered-drain-second.zip", secondFiles);

  ChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());
  ChartLibraryScanner scanner;

  assert(scanner.Scan(*session, {firstArchive, secondArchive}) ==
         kFirstChartCount + kSecondChartCount + 2);
  assert(session->CountAllChartMeta() ==
         kFirstChartCount + kSecondChartCount);

  const auto logLines = archive_file::debugLogLines();
  const auto findLogIndex = [&](const std::string &event,
                                const std::filesystem::path &archive) {
    const std::string archiveName = archive.filename().string();
    const auto found = std::find_if(
        logLines.begin(), logLines.end(), [&](const std::string &line) {
          return line.find(event) != std::string::npos &&
                 line.find(archiveName) != std::string::npos;
        });
    assert(found != logLines.end());
    return static_cast<std::size_t>(
        std::distance(logLines.begin(), found));
  };
  const std::size_t firstInsert = findLogIndex(
      "Inserting streamed DB chart batch:", firstArchive);
  const std::size_t secondStream =
      findLogIndex("Streamed archive batch via miniz ZIP:", secondArchive);
  assert(firstInsert < secondStream);
}

void testArchiveResultApplicationOverlapsItsOwnStreaming() {
  constexpr int kFirstChartCount = 128;
  constexpr int kSecondChartCount = 16;
  if (parallel_worker_count(kFirstChartCount + kSecondChartCount) <= 1) {
    return;
  }
  TempDirectory temporary;
  const auto root = temporary.path() / "library";

  std::vector<std::pair<std::string, std::string>> firstFiles;
  firstFiles.reserve(kFirstChartCount);
  const std::string padding = "#COMMENT " + std::string(128 * 1024, 'x') + "\n";
  for (int index = 0; index < kFirstChartCount; ++index) {
    firstFiles.emplace_back(
        "first/chart-" + std::to_string(index) + ".bms",
        chartText("Same Archive Overlap " + std::to_string(index)) + padding);
  }
  const auto firstArchive =
      writeZip(root / "same-archive-overlap-first.zip", firstFiles);

  std::vector<std::pair<std::string, std::string>> secondFiles;
  secondFiles.reserve(kSecondChartCount);
  for (int index = 0; index < kSecondChartCount; ++index) {
    secondFiles.emplace_back(
        "second/chart-" + std::to_string(index) + ".bms",
        chartText("Second Deferred Archive " + std::to_string(index)));
  }
  const auto secondArchive =
      writeZip(root / "same-archive-overlap-second.zip", secondFiles);

  ChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());
  ChartLibraryScanner scanner;

  assert(scanner.Scan(*session, {firstArchive, secondArchive}) ==
         kFirstChartCount + kSecondChartCount + 2);
  assert(session->CountAllChartMeta() == kFirstChartCount + kSecondChartCount);
  const ChartScanSnapshot snapshot = session->LoadScanSnapshot();
  assert(snapshot.archiveCache.size() == 2);
  std::multiset<int> cachedChartCounts;
  for (const auto &cache : snapshot.archiveCache) {
    cachedChartCounts.insert(cache.chartCount);
  }
  assert((cachedChartCounts ==
          std::multiset<int>{kFirstChartCount, kSecondChartCount}));

  const auto logLines = archive_file::debugLogLines();
  const auto findLogIndex = [&](const std::string &event) {
    const std::string archiveName = firstArchive.filename().string();
    const auto found = std::find_if(
        logLines.begin(), logLines.end(), [&](const std::string &line) {
          return line.find(event) != std::string::npos &&
                 line.find(archiveName) != std::string::npos;
        });
    assert(found != logLines.end());
    return static_cast<std::size_t>(std::distance(logLines.begin(), found));
  };
  const std::size_t firstInsert =
      findLogIndex("Inserting streamed DB chart batch:");
  const std::size_t streamFinished =
      findLogIndex("Streamed archive batch via miniz ZIP:");
  assert(firstInsert < streamFinished);
}

void testArchiveInspectionUsesMultipleEntityWorkers() {
  constexpr int kFixtureCount = 6;
  if (parallel_worker_count(kFixtureCount) <= 1) {
    return;
  }

  TempDirectory temporary;
  const auto root = temporary.path() / "library";
  std::vector<std::filesystem::path> roots;
  for (int index = 0; index < kFixtureCount; ++index) {
    roots.push_back(writeZip(root / ("empty-" + std::to_string(index) + ".zip"),
                             {{"readme.txt", "not a chart"}}));
  }

  ChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());
  ChartLibraryScanner scanner;

  const std::thread::id callerThread = std::this_thread::get_id();
  std::mutex rendezvousMutex;
  std::condition_variable rendezvousCv;
  std::set<std::thread::id> workerThreads;
  bool timedOut = false;
  const auto pauseCallback = [&]() {
    const std::thread::id currentThread = std::this_thread::get_id();
    if (currentThread == callerThread) {
      return true;
    }
    std::unique_lock lock(rendezvousMutex);
    workerThreads.insert(currentThread);
    rendezvousCv.notify_all();
    if (!rendezvousCv.wait_for(lock, std::chrono::seconds(2),
                               [&] { return workerThreads.size() >= 2; })) {
      timedOut = true;
      rendezvousCv.notify_all();
    }
    return true;
  };

  (void)scanner.Scan(*session, roots, nullptr, nullptr, pauseCallback);
  assert(session->CountAllChartMeta() == 0);
  assert(workerThreads.size() >= 2);
  assert(!timedOut);
  assert(session->LoadScanSnapshot().archiveCache.size() == kFixtureCount);
}

void testBlockedArchiveDoesNotDelayLaterOrdinaryEntities() {
  constexpr int kEntityCount = 4;
  if (parallel_worker_count(kEntityCount) <= 1) {
    return;
  }

  TempDirectory temporary;
  const auto root = temporary.path() / "library";
  const auto archive =
      writeZip(root / "00-archive.zip", {{"readme.txt", "not a chart"}});
  const auto ordinaryA = writeChart(root, "10-ordinary-a", "Ordinary A");
  const auto ordinaryB = writeChart(root, "20-ordinary-b", "Ordinary B");
  const auto ordinaryC = writeChart(root, "30-ordinary-c", "Ordinary C");

  ChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());
  ChartLibraryScanner scanner;

  const std::thread::id callerThread = std::this_thread::get_id();
  std::atomic_bool scanningRoots{false};
  std::mutex rendezvousMutex;
  std::condition_variable rendezvousCv;
  std::set<std::thread::id> entityWorkers;
  bool timedOut = false;
  const auto progressCallback = [&](const ChartScanProgress &progress) {
    if (progress.stage == ChartScanProgressStage::ScanningRoots) {
      scanningRoots.store(true, std::memory_order_release);
    } else if (progress.stage == ChartScanProgressStage::PreparingUpdates) {
      scanningRoots.store(false, std::memory_order_release);
    }
  };
  const auto pauseCallback = [&]() {
    const std::thread::id currentThread = std::this_thread::get_id();
    if (currentThread == callerThread ||
        !scanningRoots.load(std::memory_order_acquire)) {
      return true;
    }
    std::unique_lock lock(rendezvousMutex);
    entityWorkers.insert(currentThread);
    rendezvousCv.notify_all();
    if (!rendezvousCv.wait_for(lock, std::chrono::seconds(2),
                               [&] { return entityWorkers.size() >= 2; })) {
      timedOut = true;
      rendezvousCv.notify_all();
    }
    return true;
  };

  const std::vector<std::filesystem::path> roots{archive, ordinaryA, ordinaryB,
                                                 ordinaryC};
  (void)scanner.Scan(*session, roots, nullptr, progressCallback, pauseCallback);
  assert(entityWorkers.size() >= 2);
  assert(!timedOut);
  assert(session->CountAllChartMeta() == 3);
  assert(session->LoadScanSnapshot().archiveCache.size() == 1);
}

} // namespace

int main() {
  testBasicNoOpAndDeleteScan();
  testStopAndPauseBeforeWork();
  testCheckpointResume();
  testStorageFailureLeavesNoChart();
  testMixedOrdinaryAndArchiveEntitiesIndexExactlyOnce();
  testManySmallArchivesPreserveDiscoveryOrderAndCache();
  testNormalArchiveScanDoesNotRecountStoredRows();
  testMultiEntryArchivePreservesPreparedResultOrderAndCache();
  testArchiveCheckpointResumeUsesOrderedFallbackPipeline();
  testMidArchiveCheckpointResumePreservesValidCacheCount();
  testLargeSingleArchivePreservesAllChartResults();
  testArchiveResultApplicationOverlapsLaterArchiveStreaming();
  testArchiveResultApplicationOverlapsItsOwnStreaming();
  testArchiveInspectionUsesMultipleEntityWorkers();
  testBlockedArchiveDoesNotDelayLaterOrdinaryEntities();
  return 0;
}
