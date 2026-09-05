#include "../src/ArchiveRAII.h"
#include "../src/ArchiveFile.h"
#include "../src/ChartLibraryScanner.h"
#include "../src/Utils.h"
#include "../src/repositories/ChartRepository.h"
#include "../src/sqlite3.h"

#include <archive_entry.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iterator>
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

class TestChartRepository : public ChartRepository {
public:
  explicit TestChartRepository(std::filesystem::path databasePath)
      : ChartRepository(std::move(databasePath)) {
    static std::atomic<unsigned long long> sequence{0};
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    cacheDirectory_ = std::filesystem::temp_directory_path() /
                      ("asobmashow-chart-index-cache-" +
                       std::to_string(nonce) + "-" +
                       std::to_string(sequence.fetch_add(1)));
    std::error_code ignored;
    std::filesystem::create_directories(cacheDirectory_, ignored);
    archive_file::setArchiveIndexCacheDirectory(cacheDirectory_);
  }

  ~TestChartRepository() {
    std::error_code ignored;
    std::filesystem::remove_all(cacheDirectory_, ignored);
  }

private:
  std::filesystem::path cacheDirectory_;
};

void setMetadataRebuildRequired(const std::filesystem::path &databasePath,
                                bool required) {
  sqlite3 *database = nullptr;
  assert(sqlite3_open(databasePath.string().c_str(), &database) == SQLITE_OK);
  const std::string query =
      "INSERT INTO chart_meta_rebuild_state (id, required, updated_at) "
      "VALUES (1, " +
      std::string(required ? "1" : "0") +
      ", CURRENT_TIMESTAMP) ON CONFLICT(id) DO UPDATE SET required = " +
      std::string(required ? "1" : "0") +
      ", updated_at = CURRENT_TIMESTAMP";
  assert(sqlite3_exec(database, query.c_str(), nullptr, nullptr, nullptr) ==
         SQLITE_OK);
  assert(sqlite3_close(database) == SQLITE_OK);
}

bool metadataRebuildRequired(const std::filesystem::path &databasePath) {
  sqlite3 *database = nullptr;
  assert(sqlite3_open(databasePath.string().c_str(), &database) == SQLITE_OK);
  sqlite3_stmt *statement = nullptr;
  assert(sqlite3_prepare_v2(
             database,
             "SELECT required FROM chart_meta_rebuild_state WHERE id = 1", -1,
             &statement, nullptr) == SQLITE_OK);
  assert(sqlite3_step(statement) == SQLITE_ROW);
  const bool required = sqlite3_column_int(statement, 0) != 0;
  assert(sqlite3_finalize(statement) == SQLITE_OK);
  assert(sqlite3_close(database) == SQLITE_OK);
  return required;
}

void seedFolderRecord(const std::filesystem::path &databasePath,
                      const std::filesystem::path &folder,
                      std::int64_t dateSeconds,
                      std::int64_t addDateSeconds) {
  sqlite3 *database = nullptr;
  assert(sqlite3_open(databasePath.string().c_str(), &database) == SQLITE_OK);
  sqlite3_stmt *statement = nullptr;
  assert(sqlite3_prepare_v2(
             database,
             "INSERT INTO folder(path, date, adddate) VALUES(?, ?, ?)", -1,
             &statement, nullptr) == SQLITE_OK);
  const std::string path = folder.string();
  assert(sqlite3_bind_text(statement, 1, path.c_str(),
                           static_cast<int>(path.size()), SQLITE_TRANSIENT) ==
         SQLITE_OK);
  assert(sqlite3_bind_int64(statement, 2, dateSeconds) == SQLITE_OK);
  assert(sqlite3_bind_int64(statement, 3, addDateSeconds) == SQLITE_OK);
  assert(sqlite3_step(statement) == SQLITE_DONE);
  assert(sqlite3_finalize(statement) == SQLITE_OK);
  assert(sqlite3_close(database) == SQLITE_OK);
}

void setFolderAddDate(const std::filesystem::path &databasePath,
                      const std::filesystem::path &folder,
                      std::int64_t addDateSeconds) {
  sqlite3 *database = nullptr;
  assert(sqlite3_open(databasePath.string().c_str(), &database) == SQLITE_OK);
  sqlite3_stmt *statement = nullptr;
  assert(sqlite3_prepare_v2(
             database, "UPDATE folder SET adddate = ? WHERE path = ?", -1,
             &statement, nullptr) == SQLITE_OK);
  assert(sqlite3_bind_int64(statement, 1, addDateSeconds) == SQLITE_OK);
  const std::string path = folder.string();
  assert(sqlite3_bind_text(statement, 2, path.c_str(),
                           static_cast<int>(path.size()), SQLITE_TRANSIENT) ==
         SQLITE_OK);
  assert(sqlite3_step(statement) == SQLITE_DONE);
  assert(sqlite3_changes(database) == 1);
  assert(sqlite3_finalize(statement) == SQLITE_OK);
  assert(sqlite3_close(database) == SQLITE_OK);
}

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

bool corruptStoredZipPayload(const std::filesystem::path &path,
                             const std::string &payloadText) {
  const auto originalMtime = std::filesystem::last_write_time(path);
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return false;
  }
  std::string bytes((std::istreambuf_iterator<char>(input)),
                    std::istreambuf_iterator<char>());
  input.close();
  const std::size_t offset = bytes.find(payloadText);
  if (offset == std::string::npos) {
    return false;
  }
  bytes[offset] = bytes[offset] == 'X' ? 'Y' : 'X';
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    return false;
  }
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  output.close();
  std::error_code error;
  std::filesystem::last_write_time(path, originalMtime, error);
  return !error;
}

bool hasArchiveLog(const std::vector<std::string> &logLines,
                   const std::filesystem::path &archive,
                   std::string_view event) {
  const std::string archiveName = archive.filename().string();
  return std::any_of(logLines.begin(), logLines.end(), [&](const auto &line) {
    return line.find(event) != std::string::npos &&
           line.find(archiveName) != std::string::npos;
  });
}

bool waitForArchiveLog(const std::filesystem::path &archive,
                       std::string_view event) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    if (hasArchiveLog(archive_file::debugLogLines(), archive, event)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return hasArchiveLog(archive_file::debugLogLines(), archive, event);
}

class StreamingEntryGate {
public:
  StreamingEntryGate(std::filesystem::path archivePath,
                     std::filesystem::path entryPath)
      : archivePath_(std::move(archivePath)),
        entryPath_(std::move(entryPath)) {
    archive_file::setStreamingEntryObserverForTesting(
        [this](const std::filesystem::path &archivePath,
               const std::filesystem::path &entryPath) {
          if (archivePath != archivePath_ || entryPath != entryPath_) {
            return;
          }
          std::unique_lock<std::mutex> entryLock(mutex_);
          held_ = true;
          cv_.notify_all();
          while (!released_) {
            cv_.wait(entryLock);
          }
        });
  }

  ~StreamingEntryGate() {
    release();
    archive_file::setStreamingEntryObserverForTesting({});
  }

  bool waitUntilHeld() {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, std::chrono::seconds(2),
                        [this] { return held_; });
  }

  void release() {
    {
      std::lock_guard lock(mutex_);
      released_ = true;
    }
    cv_.notify_all();
  }

private:
  std::filesystem::path archivePath_;
  std::filesystem::path entryPath_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool held_ = false;
  bool released_ = false;
};

void testBasicNoOpAndDeleteScan() {
  TempDirectory temporary;
  const auto root = temporary.path() / "library";
  const auto chartPath = writeChart(root, "sample", "Repository Scanner");

  TestChartRepository repository(temporary.path() / "chart.db");
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

void testSequenceFeaturesMatchBeatorajaSongData() {
  TempDirectory temporary;
  const auto root = temporary.path() / "library";
  std::filesystem::create_directories(root);
  const auto chartPath = root / "declared-bmp.bms";
  {
    std::ofstream chart(chartPath);
    chart << chartText("Sequence Features")
          << "#STOP01 48\n"
          << "#SCROLL01 0.5\n"
          << "#BMP01 stage.png\n"
          << "#00209:01\n"
          << "#002SC:01\n";
  }
  const auto undeclaredBgaPath = root / "undeclared-bga.bms";
  {
    std::ofstream chart(undeclaredBgaPath);
    chart << chartText("Undeclared BGA")
          << "#00204:01\n";
  }

  TestChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());
  ChartLibraryScanner scanner;
  assert(scanner.Scan(*session, {root}) == 2);

  const std::array paths{chartPath, undeclaredBgaPath};
  const auto records = session->SelectChartMetaByPaths(paths);
  assert(records.status == ChartMetaPathBatchReadStatus::Loaded);
  assert(records.records.size() == 2);
  assert(records.records.front().hasBpmStop);
  assert(records.records.front().hasScrollChange);
  assert(records.records.front().hasBga);
  assert(!records.records.back().hasBga);
}

void testFolderRecordsMatchBeatorajaFolderTraversal() {
  TempDirectory temporary;
  const auto root = temporary.path() / "library";
  const auto directSongFolder = root / "songs";
  const auto hiddenChild = directSongFolder / "hidden";
  const auto nestedFolder = root / "collections" / "deep";
  writeChart(directSongFolder, "direct", "Direct Folder Chart");
  writeChart(hiddenChild, "hidden", "Hidden Child Chart");
  writeChart(nestedFolder, "nested", "Nested Folder Chart");

  TestChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  seedFolderRecord(repository.DatabasePath(), directSongFolder, 0, 1);
  auto session = repository.OpenSession();
  assert(session.has_value());
  ChartLibraryScanner scanner;
  assert(scanner.Scan(*session, {root}) == 3);

  const auto records = session->SelectFolderRecords();
  const auto find = [&](const std::filesystem::path &path) {
    return std::find_if(records.begin(), records.end(), [&](const auto &item) {
      return std::filesystem::path(item.path).lexically_normal() ==
             path.lexically_normal();
    });
  };
  const auto rootRecord = find(root);
  const auto directRecord = find(directSongFolder);
  const auto collectionsRecord = find(root / "collections");
  const auto nestedRecord = find(nestedFolder);
  assert(rootRecord != records.end());
  assert(directRecord != records.end());
  assert(collectionsRecord != records.end());
  assert(nestedRecord != records.end());
  assert(find(hiddenChild) == records.end());
  assert(directRecord->addDateSeconds > 1);
  const auto stableDirectAddDate = directRecord->addDateSeconds;
  constexpr std::int64_t stableRootAddDate = 1'234'567;
  setFolderAddDate(repository.DatabasePath(), root, stableRootAddDate);

  assert(scanner.Scan(*session, {root}) == 0);
  const auto stableRecords = session->SelectFolderRecords();
  const auto stableDirect = std::find_if(
      stableRecords.begin(), stableRecords.end(), [&](const auto &item) {
        return std::filesystem::path(item.path).lexically_normal() ==
               directSongFolder.lexically_normal();
      });
  assert(stableDirect != stableRecords.end());
  assert(stableDirect->addDateSeconds == stableDirectAddDate);
  const auto stableRoot = std::find_if(
      stableRecords.begin(), stableRecords.end(), [&](const auto &item) {
        return std::filesystem::path(item.path).lexically_normal() ==
               root.lexically_normal();
      });
  assert(stableRoot != stableRecords.end());
  assert(stableRoot->addDateSeconds == stableRootAddDate);
}

void testFolderTextDocumentFlagMatchesBeatorajaScanScope() {
  TempDirectory temporary;
  const auto root = temporary.path() / "library";
  const auto documented = root / "documented";
  const auto undocumented = root / "undocumented";
  const auto nested = undocumented / "nested";
  const auto documentedChart =
      writeChart(documented, "documented", "Documented Scanner Chart");
  const auto undocumentedChart =
      writeChart(undocumented, "undocumented", "Undocumented Scanner Chart");
  const auto nestedChart = writeChart(nested, "nested", "Nested Scanner Chart");
  {
    std::ofstream(documented / "README.TXT") << "chart documentation";
    std::ofstream(nested / "notes.txt") << "nested documentation";
  }

  TestChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());
  ChartLibraryScanner scanner;
  assert(scanner.Scan(*session, {root}) == 3);

  const std::array<std::filesystem::path, 3> paths{
      documentedChart, undocumentedChart, nestedChart};
  const auto records = session->SelectChartMetaByPaths(paths);
  assert(records.status == ChartMetaPathBatchReadStatus::Loaded);
  assert(records.records.size() == paths.size());
  assert(records.records[0].hasDocument);
  assert(!records.records[1].hasDocument);
  assert(records.records[2].hasDocument);
}

void testArchiveFolderTextDocumentFlagMatchesBeatorajaScanScope() {
  TempDirectory temporary;
  const auto archive = writeZip(
      temporary.path() / "documented.zip",
      {{"documented/chart.bms", chartText("Documented Archive Chart")},
       {"documented/README.TXT", "chart documentation"},
       {"undocumented/chart.bms", chartText("Undocumented Archive Chart")}});

  TestChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());
  ChartLibraryScanner scanner;
  assert(scanner.Scan(*session, {archive}) == 3);

  const auto snapshot = session->LoadScanSnapshot();
  assert(snapshot.charts.size() == 2);
  const std::array<std::filesystem::path, 2> paths{
      snapshot.charts[0].BmsPath, snapshot.charts[1].BmsPath};
  const auto records = session->SelectChartMetaByPaths(paths);
  assert(records.status == ChartMetaPathBatchReadStatus::Loaded);
  assert(records.records.size() == 2);
  const auto documented = std::find_if(
      records.records.begin(), records.records.end(), [](const auto &record) {
        return record.meta.Title == "Documented Archive Chart";
      });
  const auto undocumented = std::find_if(
      records.records.begin(), records.records.end(), [](const auto &record) {
        return record.meta.Title == "Undocumented Archive Chart";
      });
  assert(documented != records.records.end() && documented->hasDocument);
  assert(undocumented != records.records.end() && !undocumented->hasDocument);
}

void testKnownChartRefreshesFolderTextDocumentFlag() {
  TempDirectory temporary;
  const auto root = temporary.path() / "library";
  const auto chart = writeChart(root, "known", "Known Document Chart");

  TestChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());
  ChartLibraryScanner scanner;
  assert(scanner.Scan(*session, {root}) == 1);

  const auto record = [&] {
    const std::array<std::filesystem::path, 1> paths{chart};
    const auto loaded = session->SelectChartMetaByPaths(paths);
    assert(loaded.status == ChartMetaPathBatchReadStatus::Loaded);
    assert(loaded.records.size() == 1);
    return loaded.records.front();
  };
  assert(!record().hasDocument);

  std::ofstream(root / "README.TXT") << "chart documentation";
  assert(scanner.Scan(*session, {root}) == 1);
  assert(record().hasDocument);

  std::filesystem::remove(root / "README.TXT");
  assert(scanner.Scan(*session, {root}) == 1);
  assert(!record().hasDocument);
}

void testAddedDirectoryScanPreservesUnrelatedMissingChart() {
  TempDirectory temporary;
  const auto existingRoot = temporary.path() / "existing";
  const auto addedRoot = temporary.path() / "downloaded";
  const auto existing = writeChart(existingRoot, "existing", "Existing");
  writeChart(addedRoot, "added", "Added");

  TestChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());
  ChartLibraryScanner scanner;
  assert(scanner.Scan(*session, {existingRoot}) == 1);
  std::filesystem::remove(existing);

  const ChartScanResult addedResult =
      scanner.ScanAddedWithResult(*session, {addedRoot});
  assert(addedResult.changedCount == 1);
  assert(addedResult.completed);
  assert(addedResult.committed);
  const ChartScanSnapshot snapshot = session->LoadScanSnapshot();
  assert(snapshot.charts.size() == 2);
  assert(std::any_of(snapshot.charts.begin(), snapshot.charts.end(),
                     [](const auto &meta) { return meta.Title == "Existing"; }));
  assert(std::any_of(snapshot.charts.begin(), snapshot.charts.end(),
                     [](const auto &meta) { return meta.Title == "Added"; }));

  const ChartScanResult noWorkResult =
      scanner.ScanAddedWithResult(*session, {temporary.path() / "missing"});
  assert(noWorkResult.changedCount == 0);
  assert(noWorkResult.completed);
  assert(!noWorkResult.committed);
}

void testAddedArchivePathIsIndexed() {
  TempDirectory temporary;
  const auto archive = writeZip(
      temporary.path() / "downloaded.zip",
      {{"inside.bms", chartText("Downloaded Archive")}});

  TestChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());
  ChartLibraryScanner scanner;

  assert(scanner.ScanAdded(*session, {archive}) == 2);
  const ChartScanSnapshot snapshot = session->LoadScanSnapshot();
  assert(snapshot.charts.size() == 1);
  assert(snapshot.charts.front().Title == "Downloaded Archive");
  assert(snapshot.archiveCache.size() == 1);
  assert(snapshot.archiveCache.front().chartCount == 1);

  writeZip(archive, {{"replacement.bms", chartText("Updated Archive")}});
  assert(scanner.ScanAdded(*session, {archive}) > 0);
  const ChartScanSnapshot updatedSnapshot = session->LoadScanSnapshot();
  assert(updatedSnapshot.charts.size() == 1);
  assert(updatedSnapshot.charts.front().Title == "Updated Archive");
  assert(updatedSnapshot.archiveCache.size() == 1);
  assert(updatedSnapshot.archiveCache.front().chartCount == 1);
}

void testFullScanSkipsOnlyFindBmsPrivateStorageDirectory() {
  TempDirectory temporary;
  const auto libraryRoot = temporary.path() / "library";
  const auto downloadRoot = libraryRoot / "BMSSEARCH";
  writeChart(downloadRoot / "package", "final", "Final Download");
  writeChart(downloadRoot / ".asobmashow-transactions" / "active" / "commit",
             "commit", "Private Transaction");
  writeChart(downloadRoot / ".asobmashow-transactions-user", "legitimate",
             "Legitimate Similar Name");

  TestChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());
  ChartLibraryScanner scanner;

  assert(scanner.Scan(*session, {libraryRoot}) > 0);
  const ChartScanSnapshot snapshot = session->LoadScanSnapshot();
  assert(snapshot.charts.size() == 2);
  assert(std::any_of(snapshot.charts.begin(), snapshot.charts.end(),
                     [](const auto &meta) {
                       return meta.Title == "Final Download";
                     }));
  assert(std::any_of(snapshot.charts.begin(), snapshot.charts.end(),
                     [](const auto &meta) {
                       return meta.Title == "Legitimate Similar Name";
                     }));
}

void testStopAndPauseBeforeWork() {
  TempDirectory temporary;
  const auto root = temporary.path() / "library";
  writeChart(root, "sample", "Stopped Scanner");

  TestChartRepository repository(temporary.path() / "chart.db");
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

void testArchiveCheckpointResumeSurvivesArchiveOrderChanges() {
  constexpr int kChartsPerCompleteArchive = 20;
  constexpr int kChartsInPartialArchive = 105;
  TempDirectory temporary;
  const auto root = temporary.path() / "library";
  auto chartTextFor = [&](const std::string &prefix, int index) {
    return chartText(prefix + " " + std::to_string(index));
  };

  auto writeArchiveWith = [&](const std::string &name,
                              const std::string &prefix, int chartCount) {
    std::vector<std::pair<std::string, std::string>> files;
    for (int chartIndex = 0; chartIndex < chartCount; ++chartIndex) {
      files.emplace_back("chart-" + std::to_string(chartIndex) + ".bms",
                         chartTextFor(prefix, chartIndex));
    }
    writeZip(root / name, std::move(files));
  };

  writeArchiveWith("gamma-0.zip", "Archive0", kChartsPerCompleteArchive);
  writeArchiveWith("gamma-1.zip", "Archive1", kChartsPerCompleteArchive);
  writeArchiveWith("gamma-2.zip", "Archive2", kChartsInPartialArchive);

  TestChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());
  ChartLibraryScanner scanner;

  // Stop during the third archive so the first two are recorded as completed
  // by identity (their charts are durable) and the third records a
  // mid-archive resumed count.
  std::stop_source stop;
  const auto stopToken = stop.get_token();
  int parsingCurrent = 0;
  (void)scanner.Scan(
      *session, {root}, &stopToken,
      [&](const ChartScanProgress &progress) {
        if (progress.stage == ChartScanProgressStage::ParsingCharts) {
          parsingCurrent = progress.current;
        }
      },
      nullptr,
      [&]() -> std::uint64_t {
        return parsingCurrent >= (2 * kChartsPerCompleteArchive + 40) ? 1 : 0;
      },
      [&](std::uint64_t request) {
        assert(request == 1);
        stop.request_stop();
      });
  assert(session->CountAllChartMeta() >= 2 * kChartsPerCompleteArchive);
  const ChartScanSnapshot interrupted = session->LoadScanSnapshot();
  assert(interrupted.checkpoint.has_value());
  assert(interrupted.completedArchives.size() == 2);

  // Add a new archive that sorts before everything and replace an already
  // completed archive so its mtime no longer matches the recorded identity;
  // the identity-based resume must skip the unchanged known archive and pick
  // up the new and changed ones.
  writeArchiveWith("delta--leading.zip", "Leading", 4);
  writeArchiveWith("gamma-0.zip", "Replaced", 3);

  (void)scanner.Scan(*session, {root});
  assert(session->CountAllChartMeta() ==
         4 + 3 + kChartsPerCompleteArchive + kChartsInPartialArchive);
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

std::atomic_bool denyMetadataRebuildStateWrite{false};

int denyMetadataRebuildStateWriteAuthorizer(void *, int action,
                                             const char *first, const char *,
                                             const char *, const char *) {
  if (denyMetadataRebuildStateWrite.load(std::memory_order_relaxed) &&
      (action == SQLITE_INSERT || action == SQLITE_UPDATE) && first != nullptr &&
      std::string_view(first) == "chart_meta_rebuild_state") {
    return SQLITE_DENY;
  }
  return SQLITE_OK;
}

int installDenyMetadataRebuildStateWrite(sqlite3 *database, char **,
                                         const sqlite3_api_routines *) {
  return sqlite3_set_authorizer(
      database, denyMetadataRebuildStateWriteAuthorizer, nullptr);
}

class ScopedMetadataRebuildStateWriteDenial {
public:
  ScopedMetadataRebuildStateWriteDenial() {
    sqlite3_reset_auto_extension();
    assert(sqlite3_auto_extension(reinterpret_cast<void (*)()>(
               installDenyMetadataRebuildStateWrite)) == SQLITE_OK);
  }

  ~ScopedMetadataRebuildStateWriteDenial() {
    denyMetadataRebuildStateWrite.store(false, std::memory_order_relaxed);
    sqlite3_reset_auto_extension();
  }
};

std::atomic_bool denyChartRead{false};

int denyChartReadAuthorizer(void *, int action, const char *first,
                            const char *, const char *, const char *) {
  if (denyChartRead.load(std::memory_order_relaxed) && action == SQLITE_READ &&
      first != nullptr && std::string_view(first) == "chart_meta") {
    return SQLITE_DENY;
  }
  return SQLITE_OK;
}

int installDenyChartRead(sqlite3 *database, char **,
                         const sqlite3_api_routines *) {
  return sqlite3_set_authorizer(database, denyChartReadAuthorizer, nullptr);
}

class ScopedChartReadDenial {
public:
  ScopedChartReadDenial() {
    sqlite3_reset_auto_extension();
    assert(sqlite3_auto_extension(
               reinterpret_cast<void (*)()>(installDenyChartRead)) ==
           SQLITE_OK);
  }

  ~ScopedChartReadDenial() {
    denyChartRead.store(false, std::memory_order_relaxed);
    sqlite3_reset_auto_extension();
  }
};

std::atomic_bool countPostInsertChartPathReads{false};
std::atomic_bool observedChartInsert{false};
std::atomic_bool observedPostInsertSelect{false};
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
  } else if (action == SQLITE_SELECT &&
             observedChartInsert.load(std::memory_order_relaxed)) {
    observedPostInsertSelect.store(true, std::memory_order_relaxed);
  } else if (action == SQLITE_READ && first != nullptr && second != nullptr &&
             std::string_view(first) == "chart_meta" &&
             std::string_view(second) == "path" &&
             observedPostInsertSelect.load(std::memory_order_relaxed)) {
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
    observedPostInsertSelect.store(false, std::memory_order_relaxed);
    postInsertChartPathReads.store(0, std::memory_order_relaxed);
    sqlite3_reset_auto_extension();
  }

  void start() {
    observedChartInsert.store(false, std::memory_order_relaxed);
    observedPostInsertSelect.store(false, std::memory_order_relaxed);
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
  const auto databasePath = temporary.path() / "chart.db";
  writeChart(root, "sample", "Denied Scanner");
  ScopedInsertDenial denial;

  TestChartRepository repository(databasePath);
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());
  setMetadataRebuildRequired(databasePath, true);
  denyChartInsert.store(true, std::memory_order_relaxed);

  ChartLibraryScanner scanner;
  const ChartScanResult result = scanner.ScanWithResult(*session, {root});
  assert(result.changedCount == 0);
  assert(!result.completed);
  assert(!result.committed);
  assert(session->CountAllChartMeta() == 0);
  denyChartInsert.store(false, std::memory_order_relaxed);
  assert(metadataRebuildRequired(databasePath));
}

void testRebuildFlagClearFailureDoesNotReportCompletedScan() {
  TempDirectory temporary;
  const auto root = temporary.path() / "empty-library";
  const auto databasePath = temporary.path() / "chart.db";
  std::filesystem::create_directories(root);
  ScopedMetadataRebuildStateWriteDenial denial;

  TestChartRepository repository(databasePath);
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());
  setMetadataRebuildRequired(databasePath, true);
  denyMetadataRebuildStateWrite.store(true, std::memory_order_relaxed);

  ChartLibraryScanner scanner;
  const ChartScanResult result = scanner.ScanWithResult(*session, {root});
  assert(result.changedCount == 0);
  assert(!result.completed);
  assert(!result.committed);
  denyMetadataRebuildStateWrite.store(false, std::memory_order_relaxed);
  assert(metadataRebuildRequired(databasePath));
}

void testMissingFullScanRootPreservesMetadataRebuildState() {
  TempDirectory temporary;
  const auto root = temporary.path() / "disconnected-library";
  const auto databasePath = temporary.path() / "chart.db";

  TestChartRepository repository(databasePath);
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());
  setMetadataRebuildRequired(databasePath, true);

  ChartLibraryScanner scanner;
  const ChartScanResult result = scanner.ScanWithResult(*session, {root});
  assert(result.changedCount == 0);
  assert(!result.completed);
  assert(!result.committed);
  assert(metadataRebuildRequired(databasePath));
}

void testAddedScanStorageFailureDoesNotQualifyExistingChart() {
  TempDirectory temporary;
  const auto root = temporary.path() / "library";
  writeChart(root, "sample", "Existing Scanner");

  TestChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  ChartLibraryScanner scanner;
  {
    auto session = repository.OpenSession();
    assert(session.has_value());
    assert(scanner.ScanAdded(*session, {root}) == 1);
    assert(session->CountAllChartMeta() == 1);
  }

  ScopedInsertDenial denial;
  auto deniedSession = repository.OpenSession();
  assert(deniedSession.has_value());
  denyChartInsert.store(true, std::memory_order_relaxed);

  const ChartScanResult result =
      scanner.ScanAddedWithResult(*deniedSession, {root});
  assert(!result.completed);
  assert(!result.committed);
  assert(deniedSession->CountAllChartMeta() == 1);
}

void testAddedScanParseFailureDoesNotQualifyExistingChart() {
  TempDirectory temporary;
  const auto root = temporary.path() / "library";
  const auto chartPath = writeChart(root, "sample", "Existing Parser");

  TestChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());
  ChartLibraryScanner scanner;
  const ChartScanResult initial = scanner.ScanAddedWithResult(*session, {root});
  assert(initial.completed);
  assert(initial.committed);
  assert(initial.upsertedChartPaths.size() == 1);

  {
    std::ofstream invalidChart(chartPath);
    invalidChart << "#TITLE Invalid replacement\n";
  }
  const ChartScanResult failedParse =
      scanner.ScanAddedWithResult(*session, {root});
  assert(failedParse.completed);
  assert(failedParse.committed);
  assert(failedParse.upsertedChartPaths.empty());
  assert(session->CountAllChartMeta() == 1);
}

void testArchiveChartCountReportsStorageReadFailure() {
  TempDirectory temporary;
  TestChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  ScopedChartReadDenial denial;
  auto session = repository.OpenSession();
  assert(session.has_value());
  auto batch = session->BeginScanBatch();
  assert(batch.has_value());
  denyChartRead.store(true, std::memory_order_relaxed);

  assert(!batch->CountChartsInArchive(temporary.path() / "archive.zip")
              .has_value());
}

void testDeleteChartsInArchiveRemovesOnlyArchiveCharts() {
  TempDirectory temporary;
  const auto root = temporary.path() / "library";
  std::filesystem::create_directories(root);
  // Archive chart lives in a nested inner folder; a sibling directory with a
  // name that shares the archive's prefix must not be affected.
  const auto archivePath = writeZip(
      root / "prefix%_.zip",
      {{"pack/inside.bms", chartText("Archive Inside")}});
  writeChart(root / "prefix%_other", "loose", "Loose Chart");

  TestChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());
  auto batch = session->BeginScanBatch();
  assert(batch.has_value());

  assert(batch->UpsertArchiveCache({.path = archivePath,
                                    .solid = false,
                                    .uncompressedSize = 0,
                                    .fileCount = 1,
                                    .chartCount = 1}));
  bms_parser::ChartMeta archiveChart;
  archiveChart.BmsPath = archive_file::makeVirtualPath(
      archivePath, std::filesystem::path("pack/inside.bms"));
  archiveChart.MD5 = "aabbccddeeff00112233445566778899";
  archiveChart.SHA256 = "00112233445566778899aabbccddeeff"
                        "00112233445566778899aabbccddeeff";
  archiveChart.Title = "Archive Inside";
  assert(batch->UpsertChart(archiveChart, std::nullopt, false, {}));

  bms_parser::ChartMeta looseChart;
  looseChart.BmsPath = (root / "prefix%_other" / "loose.bms").lexically_normal();
  looseChart.MD5 = "ffeeddccbbaa99887766554433221100";
  looseChart.SHA256 = "ffeeddccbbaa99887766554433221100"
                      "ffeeddccbbaa99887766554433221100";
  looseChart.Title = "Loose Chart";
  assert(batch->UpsertChart(looseChart, std::nullopt, false, {}));
  assert(batch->Commit());

  assert(session->CountAllChartMeta() == 2);
  auto deleteBatch = session->BeginScanBatch();
  assert(deleteBatch.has_value());
  assert(deleteBatch->DeleteChartsInArchive(archivePath));
  assert(deleteBatch->Commit());
  assert(session->CountAllChartMeta() == 1);
  std::vector<bms_parser::ChartMeta> remaining;
  session->SelectAllChartMeta(remaining);
  assert(remaining.size() == 1);
  assert(remaining.front().Title == "Loose Chart");
}

void testArchiveStorageFailureDoesNotWriteCache() {
  TempDirectory temporary;
  const auto root = temporary.path() / "library";
  const auto archivePath = writeZip(
      root / "denied.zip", {{"inside.bms", chartText("Denied Archive")}});
  ScopedInsertDenial denial;

  TestChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());
  denyChartInsert.store(true, std::memory_order_relaxed);

  ChartLibraryScanner scanner;
  assert(scanner.Scan(*session, {archivePath}) >= 0);
  const ChartScanSnapshot snapshot = session->LoadScanSnapshot();
  assert(snapshot.charts.empty());
  assert(snapshot.archiveCache.empty());
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

  TestChartRepository repository(temporary.path() / "chart.db");
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

void testArchiveIndexProgressFollowsFolderTraversal() {
  TempDirectory temporary;
  const auto root = temporary.path() / "library";
  writeZip(root / "progress.zip",
           {{"inside.bms", chartText("Progress Archive")}});

  TestChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());
  ChartLibraryScanner scanner;

  std::vector<ChartScanProgressStage> stages;
  assert(scanner.Scan(*session, {root}, nullptr,
                      [&](const ChartScanProgress &progress) {
                        if (stages.empty() || stages.back() != progress.stage) {
                          stages.push_back(progress.stage);
                        }
                      }) == 2);

  const auto scanning =
      std::find(stages.begin(), stages.end(),
                ChartScanProgressStage::ScanningRoots);
  const auto indexing =
      std::find(stages.begin(), stages.end(),
                ChartScanProgressStage::IndexingArchives);
  const auto preparing =
      std::find(stages.begin(), stages.end(),
                ChartScanProgressStage::PreparingUpdates);
  assert(scanning != stages.end());
  assert(indexing != stages.end());
  assert(preparing != stages.end());
  assert(scanning < indexing);
  assert(indexing < preparing);
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

  TestChartRepository repository(temporary.path() / "chart.db");
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
  const auto logLines = archive_file::debugLogLines();
  for (const auto &archive : roots) {
    assert(hasArchiveLog(logLines, archive,
                         "Prefetching indexed archive chart batch:"));
    assert(!hasArchiveLog(logLines, archive,
                          "Queued bounded DB archive chart batch parse:"));
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
  TestChartRepository repository(temporary.path() / "chart.db");
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

  TestChartRepository repository(temporary.path() / "chart.db");
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

  TestChartRepository repository(temporary.path() / "chart.db");
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

  TestChartRepository repository(temporary.path() / "chart.db");
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

  bms_parser::ChartMeta postCheckpointRow = interrupted.charts.front();
  postCheckpointRow.BmsPath = archive_file::makeVirtualPath(
      archivePath, std::filesystem::path("chart-100.bms"));
  auto postCheckpointBatch = session->BeginScanBatch();
  assert(postCheckpointBatch.has_value());
  assert(postCheckpointBatch->UpsertChart(postCheckpointRow, std::nullopt));
  assert(postCheckpointBatch->Commit());
  assert(session->CountAllChartMeta() == 100);

  (void)scanner.Scan(*session, {archivePath});
  assert(session->CountAllChartMeta() == kValidCount);
  const ChartScanSnapshot resumed = session->LoadScanSnapshot();
  assert(resumed.archiveCache.size() == 1);
  assert(resumed.archiveCache.front().chartCount == kValidCount);
  assert(!resumed.checkpoint.has_value());
}

void testArchiveStreamFailurePreservesCheckpointPrefix() {
  constexpr int kChartCount = 105;
  TempDirectory temporary;
  const auto root = temporary.path() / "library";
  const auto databasePath = temporary.path() / "chart.db";
  std::vector<std::pair<std::string, std::string>> files;
  files.reserve(kChartCount);
  for (int index = 0; index < kChartCount; ++index) {
    files.emplace_back("chart-" + std::to_string(index) + ".bms",
                       chartText("Failure Resume " + std::to_string(index)));
  }
  const auto archivePath =
      writeZip(root / "failed-resume.zip", files);

  TestChartRepository repository(databasePath);
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
  assert(session->CountAllChartMeta() == 100);
  const ChartScanSnapshot interrupted = session->LoadScanSnapshot();
  assert(interrupted.checkpoint.has_value());
  assert(interrupted.checkpoint->subIndex == 100);
  setMetadataRebuildRequired(databasePath, true);

  bool corrupted = false;
  const ChartScanResult result = scanner.ScanWithResult(
      *session, {archivePath}, nullptr,
      [&](const ChartScanProgress &progress) {
        if (!corrupted &&
            progress.stage == ChartScanProgressStage::ReadingArchive) {
          corrupted = corruptStoredZipPayload(archivePath, "Failure Resume 102");
        }
      });
  assert(corrupted);
  // An archive that cannot be read is skipped instead of aborting the whole
  // refresh: the scan completes, the durable charts from the interrupted run
  // are preserved, and the checkpoint is cleared on success.
  assert(result.completed);
  assert(result.committed);
  assert(session->CountAllChartMeta() == 100);
  const ChartScanSnapshot failed = session->LoadScanSnapshot();
  assert(failed.archiveCache.empty());
  assert(!failed.checkpoint.has_value());
  assert(!metadataRebuildRequired(databasePath));
}

void testUnreadableArchivePreservesMetadataRebuildState() {
  TempDirectory temporary;
  const auto root = temporary.path() / "library";
  const auto databasePath = temporary.path() / "chart.db";
  std::filesystem::create_directories(root);
  const auto archivePath = root / "unreadable.zip";
  std::ofstream(archivePath, std::ios::binary) << "not an archive";

  TestChartRepository repository(databasePath);
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());
  setMetadataRebuildRequired(databasePath, true);

  ChartLibraryScanner scanner;
  const ChartScanResult result = scanner.ScanWithResult(*session, {root});
  assert(!result.completed);
  assert(!result.committed);
  assert(session->CountAllChartMeta() == 0);
  assert(metadataRebuildRequired(databasePath));
}

void testStopAtPreparingUpdatesCancelsArchivePrefetch() {
  constexpr int kChartCount = 32;
  TempDirectory temporary;
  const auto root = temporary.path() / "library";
  const std::string padding = "#COMMENT " + std::string(64 * 1024, 'x') + "\n";
  auto makeFiles = [&](std::string_view prefix) {
    std::vector<std::pair<std::string, std::string>> files;
    files.reserve(kChartCount);
    for (int index = 0; index < kChartCount; ++index) {
      files.emplace_back(std::string(prefix) + "/chart-" +
                             std::to_string(index) + ".bms",
                         chartText(std::string(prefix) + " " +
                                   std::to_string(index)) +
                             padding);
    }
    return files;
  };
  const auto firstArchive =
      writeZip(root / "cancel-first.zip", makeFiles("first"));
  const auto secondArchive =
      writeZip(root / "cancel-second.zip", makeFiles("second"));

  TestChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());
  ChartLibraryScanner scanner;
  std::stop_source stop;
  const auto stopToken = stop.get_token();
  bool reachedPreparingUpdates = false;

  assert(scanner.Scan(
             *session, {firstArchive, secondArchive}, &stopToken,
             [&](const ChartScanProgress &progress) {
               if (progress.stage ==
                   ChartScanProgressStage::PreparingUpdates) {
                 reachedPreparingUpdates = true;
                 stop.request_stop();
               }
             }) == 0);
  assert(reachedPreparingUpdates);
  assert(session->CountAllChartMeta() == 0);
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

  TestChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());
  ChartLibraryScanner scanner;

  assert(scanner.Scan(*session, {archivePath}) == kChartCount + 1);
  const ChartScanSnapshot snapshot = session->LoadScanSnapshot();
  assert(snapshot.charts.size() == kChartCount);
  assert(snapshot.archiveCache.size() == 1);
  assert(snapshot.archiveCache.front().chartCount == kChartCount);
  const auto logLines = archive_file::debugLogLines();
  assert(hasArchiveLog(logLines, archivePath,
                       "Finished single archive concurrent chart parse:"));
  assert(!hasArchiveLog(logLines, archivePath,
                        "Prefetching indexed archive chart batch:"));
  assert(scanner.Scan(*session, {archivePath}) == 0);
}

void testMultipleLargeArchivesPrefetchDuringPreparation() {
  constexpr int kChartCount = 24;
  TempDirectory temporary;
  const auto root = temporary.path() / "library";

  const auto makeFiles = [](std::string_view directory,
                            std::string_view titlePrefix) {
    std::vector<std::pair<std::string, std::string>> files;
    files.reserve(kChartCount);
    for (int index = 0; index < kChartCount; ++index) {
      files.emplace_back(
          std::string(directory) + "/chart-" + std::to_string(index) + ".bms",
          chartText(std::string(titlePrefix) + " " + std::to_string(index)));
    }
    return files;
  };

  const auto firstArchive =
      writeZip(root / "continuous-first.zip", makeFiles("first", "First"));
  const auto secondArchive =
      writeZip(root / "continuous-second.zip", makeFiles("second", "Second"));

  TestChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());
  ChartLibraryScanner scanner;

  assert(scanner.Scan(*session, {firstArchive, secondArchive}) ==
         kChartCount * 2 + 2);
  assert(session->CountAllChartMeta() == kChartCount * 2);
  const ChartScanSnapshot snapshot = session->LoadScanSnapshot();
  assert(snapshot.archiveCache.size() == 2);
  assert(std::all_of(
      snapshot.archiveCache.begin(), snapshot.archiveCache.end(),
      [](const auto &cache) { return cache.chartCount == kChartCount; }));

  const auto logLines = archive_file::debugLogLines();
  for (const auto &archive : {firstArchive, secondArchive}) {
    assert(hasArchiveLog(logLines, archive,
                         "Prefetching indexed archive chart batch:"));
    assert(!hasArchiveLog(logLines, archive,
                          "Queued bounded DB archive chart batch parse:"));
  }
}

void testArchiveResultApplicationOverlapsLaterArchiveStreaming() {
  constexpr int kFirstChartCount = 16;
  constexpr int kSecondChartCount = 16;
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
  for (int index = 0; index < kSecondChartCount; ++index) {
    secondFiles.emplace_back(
        "second/chart-" + std::to_string(index) + ".bms",
        chartText("Second Archive " + std::to_string(index)));
  }
  const auto secondArchive =
      writeZip(root / "ordered-drain-second.zip", secondFiles);

  TestChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());
  ChartLibraryScanner scanner;

  StreamingEntryGate gate(secondArchive, "second/chart-0.bms");
  std::atomic_int changed{-1};
  std::thread scanThread([&] {
    changed.store(scanner.Scan(*session, {firstArchive, secondArchive}),
                  std::memory_order_release);
  });
  const bool secondReaderHeld = gate.waitUntilHeld();
  const bool firstBatchApplied =
      secondReaderHeld && waitForArchiveLog(
                              firstArchive,
                              "Inserting streamed DB chart batch:");
  gate.release();
  scanThread.join();

  assert(secondReaderHeld);
  assert(firstBatchApplied);
  assert(changed.load(std::memory_order_acquire) ==
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
  constexpr int kFirstChartCount = 16;
  constexpr int kSecondChartCount = 16;
  if (parallel_worker_count(kFirstChartCount + kSecondChartCount) <= 1) {
    return;
  }
  TempDirectory temporary;
  const auto root = temporary.path() / "library";

  std::vector<std::pair<std::string, std::string>> firstFiles;
  firstFiles.reserve(kFirstChartCount);
  for (int index = 0; index < kFirstChartCount; ++index) {
    firstFiles.emplace_back(
        "first/chart-" + std::to_string(index) + ".bms",
        chartText("Same Archive Overlap " + std::to_string(index)));
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

  TestChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());
  ChartLibraryScanner scanner;

  StreamingEntryGate gate(firstArchive, "first/chart-12.bms");
  std::atomic_int changed{-1};
  std::thread scanThread([&] {
    changed.store(scanner.Scan(*session, {firstArchive, secondArchive}),
                  std::memory_order_release);
  });
  const bool firstReaderHeld = gate.waitUntilHeld();
  const bool firstBatchApplied =
      firstReaderHeld && waitForArchiveLog(firstArchive,
                                            "Inserting streamed DB chart batch:");
  gate.release();
  scanThread.join();

  assert(firstReaderHeld);
  assert(firstBatchApplied);
  assert(changed.load(std::memory_order_acquire) ==
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
  if (parallel_worker_count(kFixtureCount) <= 2) {
    return;
  }

  TempDirectory temporary;
  const auto root = temporary.path() / "library";
  std::vector<std::filesystem::path> roots;
  for (int index = 0; index < kFixtureCount; ++index) {
    roots.push_back(writeZip(root / ("empty-" + std::to_string(index) + ".zip"),
                             {{"readme.txt", "not a chart"}}));
  }

  TestChartRepository repository(temporary.path() / "chart.db");
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

void testConcurrentPauseInterruptionStopsScanCleanly() {
  constexpr int kFixtureCount = 8;
  if (parallel_worker_count(kFixtureCount) <= 2) {
    return;
  }

  TempDirectory temporary;
  const auto root = temporary.path() / "library";
  for (int index = 0; index < kFixtureCount; ++index) {
    writeChart(root, "chart-" + std::to_string(index),
               "Interrupted " + std::to_string(index));
  }

  TestChartRepository repository(temporary.path() / "chart.db");
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
    return false;
  };

  const ChartScanResult result =
      scanner.ScanWithResult(*session, {root}, nullptr, nullptr,
                             pauseCallback);
  assert(!result.completed);
  assert(!result.committed);
  assert(workerThreads.size() >= 2);
  assert(!timedOut);
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

  TestChartRepository repository(temporary.path() / "chart.db");
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

void testClearChartMetaThenRescanRepopulatesLibrary() {
  constexpr int kChartsPerArchive = 25;
  TempDirectory temporary;
  const auto root = temporary.path() / "library";
  const auto writeArchiveWith = [&](const std::string &name,
                                    const std::string &prefix,
                                    int chartCount) {
    std::vector<std::pair<std::string, std::string>> files;
    for (int chartIndex = 0; chartIndex < chartCount; ++chartIndex) {
      files.emplace_back("chart-" + std::to_string(chartIndex) + ".bms",
                         chartText(prefix + " " + std::to_string(chartIndex)));
    }
    return writeZip(root / name, std::move(files));
  };

  writeArchiveWith("alpha-0.zip", "Alpha 0", kChartsPerArchive);
  writeArchiveWith("alpha-1.zip", "Alpha 1", kChartsPerArchive);
  writeArchiveWith("alpha-2.zip", "Alpha 2", kChartsPerArchive);

  TestChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());
  ChartLibraryScanner scanner;

  // Interrupt during the third archive so the first two are recorded as
  // completed by identity in chart_scan_completed_archive.
  std::stop_source stop;
  const auto stopToken = stop.get_token();
  int parsingCurrent = 0;
  (void)scanner.Scan(
      *session, {root}, &stopToken,
      [&](const ChartScanProgress &progress) {
        if (progress.stage == ChartScanProgressStage::ParsingCharts) {
          parsingCurrent = progress.current;
        }
      },
      nullptr,
      [&]() -> std::uint64_t {
        return parsingCurrent >= (2 * kChartsPerArchive + 10) ? 1 : 0;
      },
      [&](std::uint64_t request) {
        assert(request == 1);
        stop.request_stop();
      });
  const ChartScanSnapshot interrupted = session->LoadScanSnapshot();
  assert(interrupted.completedArchives.size() == 2);
  assert(session->CountAllChartMeta() > 0);

  // Manual library rebuild: clearing chart metadata must also clear the
  // completed-archive markers, or the next scan skips every unchanged
  // archive (path/size/mtime match) and the library appears empty.
  assert(session->ClearChartMeta());
  assert(session->CountAllChartMeta() == 0);

  (void)scanner.Scan(*session, {root});
  assert(session->CountAllChartMeta() == 3 * kChartsPerArchive);
}

void testScopedRefreshPreservesLibraryCompletedMarkersAndIndexFiles() {
  constexpr int kChartsPerArchive = 25;
  TempDirectory temporary;
  const auto root = temporary.path() / "library";
  const auto folderA = root / "a";
  const auto folderB = root / "b";
  const auto folderC = root / "c";
  std::filesystem::create_directories(folderA);
  std::filesystem::create_directories(folderB);
  std::filesystem::create_directories(folderC);
  const auto writeArchiveIn = [&](const std::filesystem::path &directory,
                                  const std::string &name,
                                  const std::string &prefix,
                                  int chartCount) {
    std::vector<std::pair<std::string, std::string>> files;
    for (int chartIndex = 0; chartIndex < chartCount; ++chartIndex) {
      files.emplace_back("chart-" + std::to_string(chartIndex) + ".bms",
                         chartText(prefix + " " + std::to_string(chartIndex)));
    }
    return writeZip(directory / name, std::move(files));
  };
  writeArchiveIn(folderA, "alpha-0.zip", "Alpha 0", kChartsPerArchive);
  writeArchiveIn(folderB, "alpha-1.zip", "Alpha 1", kChartsPerArchive);
  writeArchiveIn(folderC, "alpha-2.zip", "Alpha 2", kChartsPerArchive);

  TestChartRepository repository(temporary.path() / "chart.db");
  assert(repository.EnsureReady());
  auto session = repository.OpenSession();
  assert(session.has_value());
  ChartLibraryScanner scanner;

  const auto cacheDir = temporary.path() / "idx";
  std::filesystem::create_directories(cacheDir);
  archive_file::setArchiveIndexCacheDirectory(cacheDir);
  const auto cachedIndexCount = [&]() {
    std::size_t count = 0;
    for (const auto &entry : std::filesystem::directory_iterator(cacheDir)) {
      std::error_code error;
      if (entry.is_regular_file(error) && !error &&
          entry.path().extension() == ".idx") {
        ++count;
      }
    }
    return count;
  };

  // Interrupt during the third archive so the first two are recorded as
  // completed for the whole library.
  std::stop_source stop;
  const auto stopToken = stop.get_token();
  int parsingCurrent = 0;
  (void)scanner.Scan(
      *session, {root}, &stopToken,
      [&](const ChartScanProgress &progress) {
        if (progress.stage == ChartScanProgressStage::ParsingCharts) {
          parsingCurrent = progress.current;
        }
      },
      nullptr,
      [&]() -> std::uint64_t {
        return parsingCurrent >= (2 * kChartsPerArchive + 10) ? 1 : 0;
      },
      [&](std::uint64_t request) {
        assert(request == 1);
        stop.request_stop();
      });
  const ChartScanSnapshot interrupted = session->LoadScanSnapshot();
  assert(interrupted.completedArchives.size() == 2);
  const std::size_t indexCountBefore = cachedIndexCount();
  assert(indexCountBefore >= 1);

  // A scoped refresh of a single folder must not wipe the full library's
  // completed-archive markers nor drop the still-present sibling archives'
  // persisted index files.
  const ChartScanResult scoped =
      scanner.ScanScopedWithResult(*session, {folderC});
  assert(scoped.completed);
  const ChartScanSnapshot scopedSnapshot = session->LoadScanSnapshot();
  assert(scopedSnapshot.completedArchives.size() == 3);
  assert(cachedIndexCount() == indexCountBefore);

  // The scoped archive is fully parsed and out-of-scope archives keep their
  // charts.
  assert(session->CountAllChartMeta() == 3 * kChartsPerArchive);
}

} // namespace

int main() {
  testBasicNoOpAndDeleteScan();
  testSequenceFeaturesMatchBeatorajaSongData();
  testFolderRecordsMatchBeatorajaFolderTraversal();
  testFolderTextDocumentFlagMatchesBeatorajaScanScope();
  testArchiveFolderTextDocumentFlagMatchesBeatorajaScanScope();
  testKnownChartRefreshesFolderTextDocumentFlag();
  testAddedDirectoryScanPreservesUnrelatedMissingChart();
  testAddedArchivePathIsIndexed();
  testFullScanSkipsOnlyFindBmsPrivateStorageDirectory();
  testStopAndPauseBeforeWork();
  testArchiveCheckpointResumeSurvivesArchiveOrderChanges();
  testStorageFailureLeavesNoChart();
  testRebuildFlagClearFailureDoesNotReportCompletedScan();
  testMissingFullScanRootPreservesMetadataRebuildState();
  testAddedScanStorageFailureDoesNotQualifyExistingChart();
  testAddedScanParseFailureDoesNotQualifyExistingChart();
  testArchiveChartCountReportsStorageReadFailure();
  testDeleteChartsInArchiveRemovesOnlyArchiveCharts();
  testArchiveStorageFailureDoesNotWriteCache();
  testMixedOrdinaryAndArchiveEntitiesIndexExactlyOnce();
  testArchiveIndexProgressFollowsFolderTraversal();
  testManySmallArchivesPreserveDiscoveryOrderAndCache();
  testNormalArchiveScanDoesNotRecountStoredRows();
  testMultiEntryArchivePreservesPreparedResultOrderAndCache();
  testArchiveCheckpointResumeUsesOrderedFallbackPipeline();
  testMidArchiveCheckpointResumePreservesValidCacheCount();
  testArchiveStreamFailurePreservesCheckpointPrefix();
  testUnreadableArchivePreservesMetadataRebuildState();
  testStopAtPreparingUpdatesCancelsArchivePrefetch();
  testLargeSingleArchivePreservesAllChartResults();
  testMultipleLargeArchivesPrefetchDuringPreparation();
  testArchiveResultApplicationOverlapsLaterArchiveStreaming();
  testArchiveResultApplicationOverlapsItsOwnStreaming();
  testArchiveInspectionUsesMultipleEntityWorkers();
  testConcurrentPauseInterruptionStopsScanCleanly();
  testBlockedArchiveDoesNotDelayLaterOrdinaryEntities();
  testClearChartMetaThenRescanRepopulatesLibrary();
  testScopedRefreshPreservesLibraryCompletedMarkersAndIndexFiles();
  return 0;
}
