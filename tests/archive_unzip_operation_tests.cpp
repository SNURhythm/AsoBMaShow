#include "../src/scene/ArchiveUnzipOperation.h"
#include "../src/ArchiveRAII.h"
#include "../src/ChartLibraryScanner.h"
#include "../src/library/ArchiveUnzipRecovery.h"
#include "../src/sqlite3.h"

#include <archive_entry.h>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iostream>
#include <thread>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

std::filesystem::path testExecutable;

class Fixture {
public:
  Fixture()
      : root(std::filesystem::temp_directory_path() /
             ("archive-unzip-operation-" + std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()))),
        repository(root / "library.db") {
    std::filesystem::create_directories(root);
    archive_file::setArchiveIndexCacheDirectory(root / "cache");
    auto session = repository.OpenSession();
    assert(session && session->EnsureSchema());
  }

  ~Fixture() {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }

  ChartMetaRecord archive(int chartCount = 1, const std::string &name = "song.zip") {
    const auto archivePath = root / name;
    auto writer = makeArchiveWriteHandle();
    assert(archive_write_set_format_zip(writer.get()) == ARCHIVE_OK);
    assert(archive_write_open_filename(writer.get(), archivePath.string().c_str()) == ARCHIVE_OK);
    for (int chartIndex = 0; chartIndex < chartCount; ++chartIndex) {
      auto entry = std::unique_ptr<archive_entry, decltype(&archive_entry_free)>(
          archive_entry_new(), archive_entry_free);
      const std::string contents = "#TITLE Extracted " + name + std::to_string(chartIndex) +
          "\n#BPM 120\n#WAV01 sound.wav\n#00111:01\n";
      const std::string path = "song/chart" + std::to_string(chartIndex) + ".bms";
      archive_entry_set_pathname(entry.get(), path.c_str());
      archive_entry_set_size(entry.get(), contents.size());
      archive_entry_set_filetype(entry.get(), AE_IFREG);
      archive_entry_set_perm(entry.get(), 0644);
      assert(archive_write_header(writer.get(), entry.get()) == ARCHIVE_OK);
      assert(archive_write_data(writer.get(), contents.data(), contents.size()) ==
             static_cast<la_ssize_t>(contents.size()));
      assert(archive_write_finish_entry(writer.get()) == ARCHIVE_OK);
    }
    assert(archive_write_close(writer.get()) == ARCHIVE_OK);
    ChartMetaRecord record;
    record.solidArchive = true;
    record.meta.BmsPath = archivePath;
    return record;
  }

  ChartMetaRecord indexedArchive(const std::string &name, int chartCount = 1) {
    const auto record = archive(chartCount, name);
    auto session = repository.OpenSession();
    auto batch = session->BeginScanBatch();
    assert(batch && batch->UpsertSolidArchive({.path = record.meta.BmsPath}));
    assert(batch->Commit());
    return record;
  }

  void failChartWrites() {
    sqlite3 *database = nullptr;
    assert(sqlite3_open((root / "library.db").string().c_str(), &database) == SQLITE_OK);
    assert(sqlite3_exec(database,
        "CREATE TRIGGER reject_chart BEFORE INSERT ON chart_meta BEGIN SELECT RAISE(FAIL, 'test write failure'); END",
        nullptr, nullptr, nullptr) == SQLITE_OK);
    assert(sqlite3_close(database) == SQLITE_OK);
  }

  std::filesystem::path root;
  ChartRepository repository;
};

ArchiveUnzipResult waitForResult(ArchiveUnzipOperation &operation) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (std::chrono::steady_clock::now() < deadline) {
    if (auto result = operation.takeResult()) {
      return *result;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  assert(false && "archive operation did not finish");
  return {};
}

ArchiveUnzipResult runAll(ChartRepository &repository, bool deleteAfterUnzip,
                         std::stop_token stopToken = {},
                         archive_file::UnzipProgressCallback progress = nullptr) {
  return ArchiveUnzipOperation::RunAll(repository, deleteAfterUnzip, stopToken, progress);
}

void executeSql(const std::filesystem::path &path, const char *sql) {
  sqlite3 *database = nullptr;
  assert(sqlite3_open(path.string().c_str(), &database) == SQLITE_OK);
  assert(sqlite3_exec(database, sql, nullptr, nullptr, nullptr) == SQLITE_OK);
  assert(sqlite3_close(database) == SQLITE_OK);
}

void runCrashingChild(const std::filesystem::path &root, const std::string &phase,
                      const std::string &mode = "--crash-unzip") {
  const auto command = "\"" + testExecutable.string() + "\" " + mode + " \"" +
                       root.string() + "\" " + phase;
  const auto status = std::system(command.c_str());
#ifdef _WIN32
  assert(status == 73);
#else
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 73);
#endif
}

void crashUnzip(const std::filesystem::path &root, const std::string &phase) {
  ChartRepository repository(root / "library.db");
  archive_file::setArchiveIndexCacheDirectory(root / "cache");
  bool sawFirstWrite = false;
  runAll(repository, true, {}, [&](const archive_file::UnzipProgress &progress) {
    if (phase == "partial" &&
        (progress.message.find("Unzipping archive") != std::string::npos ||
         progress.message.find("Writing unzipped files") != std::string::npos)) {
      if (sawFirstWrite) std::_Exit(73);
      sawFirstWrite = true;
    }
    if (phase == "after-delete" && progress.message.find("b.zip") != std::string::npos) {
      std::_Exit(73);
    }
    if (phase == "complete" && progress.message.find("Unzip complete") != std::string::npos) {
      std::_Exit(73);
    }
    if (phase == "before-index" && progress.message == "Indexing extracted folders") {
      std::_Exit(73);
    }
    if (phase == "during-index" && progress.indexing && progress.current > 0) {
      std::_Exit(73);
    }
  });
  std::_Exit(74);
}

void crashRecovery(const std::filesystem::path &root) {
  ChartRepository repository(root / "library.db");
  auto session = repository.OpenSession();
  assert(session && session->EnsureSchema());
  archive_unzip_recovery::recover(*session, {}, [](const ChartScanProgress &progress) {
    if (progress.stage == ChartScanProgressStage::ParsingCharts && progress.current > 0) {
      std::_Exit(73);
    }
  });
  std::_Exit(74);
}

void unexpectedExitPreservesRecoveryWork() {
  Fixture fixture;
  const auto first = fixture.indexedArchive("a.zip");
  const auto second = fixture.indexedArchive("b.zip");
  runCrashingChild(fixture.root, "after-delete");
  assert(!std::filesystem::exists(first.meta.BmsPath));
  assert(std::filesystem::exists(second.meta.BmsPath));
  sqlite3 *database = nullptr;
  assert(sqlite3_open((fixture.root / "library.db").string().c_str(), &database) == SQLITE_OK);
  sqlite3_stmt *statement = nullptr;
  assert(sqlite3_prepare_v2(database, "SELECT COUNT(*) FROM archive_unzip_recovery",
                          -1, &statement, nullptr) == SQLITE_OK);
  assert(sqlite3_step(statement) == SQLITE_ROW);
  assert(sqlite3_column_int(statement, 0) == 1);
  sqlite3_finalize(statement);
  assert(sqlite3_close(database) == SQLITE_OK);
}

void partialExtractionIsNotIndexedByOrdinaryStartupScan() {
  Fixture fixture;
  fixture.indexedArchive("a.zip", 4);
  runCrashingChild(fixture.root, "partial");
  auto session = fixture.repository.OpenSession();
  const auto pending = session->LoadUnzipRecovery();
  assert(pending && pending->size() == 1);
  const auto folder = pending->front().outputFolder;
  assert(std::filesystem::exists(folder / ".asobmashow_unzip_incomplete"));
  assert(std::filesystem::exists(folder / "song/chart0.bms"));
  ChartLibraryScanner scanner;
  const auto scanned = scanner.ScanAddedWithResult(*session, {folder.parent_path()});
  assert(scanned.completed && scanned.committed);
  ChartMetaQuery query;
  query.recursiveFolder = folder;
  query.rawSongData = true;
  assert(session->CountChartMeta(query) == 0);
  for (const auto &root : {folder, folder / "song", folder / "song/chart0.bms"}) {
    const auto added = scanner.ScanAddedWithResult(*session, {root});
    assert(added.completed && added.changedCount == 0);
    assert(session->CountChartMeta(query) == 0);
  }
  assert(archive_unzip_recovery::recover(*session).completed);
  assert(session->LoadUnzipRecovery()->empty());
  assert(std::filesystem::exists(folder / "song/chart0.bms"));
}

void restartRecoversEveryCompletedCrashBoundary(const std::string &phase) {
  Fixture fixture;
  const auto first = fixture.indexedArchive("a.zip", 4);
  const auto second = fixture.indexedArchive("b.zip", 2);
  runCrashingChild(fixture.root, phase);
  ChartRepository restarted(fixture.root / "library.db");
  auto session = restarted.OpenSession();
  assert(session && session->EnsureSchema());
  const auto pending = session->LoadUnzipRecovery();
  assert(pending && !pending->empty());
  const bool firstSurvived = std::filesystem::exists(first.meta.BmsPath);
  const bool secondSurvived = std::filesystem::exists(second.meta.BmsPath);
  const int expectedCharts = phase == "complete" || phase == "after-delete" ? 4 : 6;
  if (phase == "during-index") {
    session.reset();
    runCrashingChild(fixture.root, "index", "--crash-recovery");
    session = restarted.OpenSession();
    assert(session);
    assert(!session->LoadUnzipRecovery()->empty());
  }
  const auto recovered = archive_unzip_recovery::recover(*session);
  assert(recovered.completed && recovered.libraryChanged);
  assert(session->CountAllChartMeta() == expectedCharts);
  assert(session->CountSolidArchives() == int(firstSurvived) + int(secondSurvived));
  assert(session->LoadUnzipRecovery()->empty());
  assert(std::filesystem::exists(first.meta.BmsPath) == firstSurvived);
  assert(std::filesystem::exists(second.meta.BmsPath) == secondSurvived);
  const auto revision = restarted.GetLibraryRevision();
  const auto repeated = archive_unzip_recovery::recover(*session);
  assert(repeated.completed && !repeated.libraryChanged);
  assert(restarted.GetLibraryRevision() == revision);
  assert(session->CountAllChartMeta() == expectedCharts);
}

void recoveryRetriesFailedCleanupIndexAndAcknowledgement() {
  for (const auto &failure : {"cleanup", "index", "acknowledgement"}) {
    Fixture fixture;
    fixture.indexedArchive("a.zip", 4);
    fixture.indexedArchive("b.zip", 2);
    runCrashingChild(fixture.root, "before-index");
    const std::string table = std::string(failure) == "cleanup" ? "solid_archives" :
                             std::string(failure) == "index" ? "chart_meta" : "archive_unzip_recovery";
    const std::string action = std::string(failure) == "index" ? "INSERT" : "DELETE";
    const auto sql = "CREATE TRIGGER reject_recovery BEFORE " + action + " ON " + table +
                     " BEGIN SELECT RAISE(FAIL, 'test recovery failure'); END";
    executeSql(fixture.root / "library.db", sql.c_str());
    ChartRepository restarted(fixture.root / "library.db");
    auto session = restarted.OpenSession();
    assert(!archive_unzip_recovery::recover(*session).completed);
    assert(session->LoadUnzipRecovery()->size() == 2);
    if (std::string(failure) == "cleanup") assert(session->CountSolidArchives() == 2);
    executeSql(fixture.root / "library.db", "DROP TRIGGER reject_recovery");
    assert(archive_unzip_recovery::recover(*session).completed);
    assert(session->LoadUnzipRecovery()->empty());
    assert(session->CountSolidArchives() == 0);
    assert(session->CountAllChartMeta() == 6);
  }
}

void journalFailurePreventsExtractionAndDeletion() {
  Fixture fixture;
  const auto record = fixture.indexedArchive("a.zip");
  executeSql(fixture.root / "library.db",
      "CREATE TRIGGER reject_journal BEFORE INSERT ON archive_unzip_recovery "
      "BEGIN SELECT RAISE(FAIL, 'test journal failure'); END");
  const auto result = runAll(fixture.repository, true);
  assert(!result.success && result.succeededCount == 0 && result.deletedCount == 0);
  assert(std::filesystem::exists(record.meta.BmsPath));
  assert(!std::filesystem::exists(fixture.root / "a"));
}

void tornMarkerLeavesTheOriginalAndPartialOutputAlone() {
  Fixture fixture;
  const auto original = fixture.indexedArchive("a.zip", 4);
  runCrashingChild(fixture.root, "partial");
  auto session = fixture.repository.OpenSession();
  const auto record = session->LoadUnzipRecovery()->front();
  std::ofstream(record.outputFolder / ".asobmashow_unzip_complete") << record.archiveKey;
  assert(archive_unzip_recovery::recover(*session).completed);
  assert(session->LoadUnzipRecovery()->empty());
  assert(session->CountAllChartMeta() == 0 && session->CountSolidArchives() == 1);
  assert(std::filesystem::exists(original.meta.BmsPath));
  assert(std::filesystem::exists(record.outputFolder / "song/chart0.bms"));
  assert(std::filesystem::exists(record.outputFolder / ".asobmashow_unzip_incomplete"));
}

void journalNormalizesPathAliasesForRecoveryAndAcknowledgement() {
  Fixture fixture;
  fixture.indexedArchive("a.zip");
  fixture.indexedArchive("b.zip");
  runCrashingChild(fixture.root, "after-delete");
  auto session = fixture.repository.OpenSession();
  auto record = session->LoadUnzipRecovery()->front();
  record.archivePath = fixture.root / "." / record.archivePath.filename();
  record.outputFolder = fixture.root / "." / record.outputFolder.filename();
  assert(session->SaveUnzipRecovery(record));
  const auto pending = session->LoadUnzipRecovery();
  assert(pending && pending->size() == 1);
  assert(pending->front().archivePath == record.archivePath.lexically_normal());
  assert(pending->front().outputFolder == record.outputFolder.lexically_normal());
  assert(archive_unzip_recovery::recover(*session).completed);
  assert(session->LoadUnzipRecovery()->empty());
}

void cancellingAnUnzipWaitingForRecoveryDoesNotBlockShutdown() {
  Fixture fixture;
  fixture.indexedArchive("a.zip");
  ArchiveUnzipOperation operation(fixture.repository);
  std::unique_lock recoveryLock(archive_unzip_recovery::operationMutex());
  assert(operation.startAll(true));
  auto shutdown = std::async(std::launch::async, [&] { operation.cancelAndWait(); });
  const bool cancelledWithoutRecoveryFinishing =
      shutdown.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
  recoveryLock.unlock();
  shutdown.get();
  assert(cancelledWithoutRecoveryFinishing);
}

void inaccessibleChartSubfolderRetainsRecoveryUntilItCanBeIndexed(bool denyFile = false) {
#ifndef _WIN32
  if (geteuid() == 0) return;
  Fixture fixture;
  fixture.indexedArchive("a.zip", 4);
  fixture.indexedArchive("b.zip");
  runCrashingChild(fixture.root, "after-delete");
  auto session = fixture.repository.OpenSession();
  const auto record = session->LoadUnzipRecovery()->front();
  const auto denied = denyFile ? record.outputFolder / "song/chart0.bms"
                              : record.outputFolder / "song";
  const auto permissions = std::filesystem::status(denied).permissions();
  std::filesystem::permissions(denied, std::filesystem::perms::none);
  const auto recovery = archive_unzip_recovery::recover(*session);
  std::filesystem::permissions(denied, permissions);
  assert(!recovery.completed);
  assert(session->LoadUnzipRecovery()->size() == 1);
  assert(archive_unzip_recovery::recover(*session).completed);
  assert(session->LoadUnzipRecovery()->empty());
  assert(session->CountAllChartMeta() == 4);
#endif
}

void disconnectedOutputDuringFinalIndexRemainsQueuedAlongsideHealthyOutputs() {
  Fixture fixture;
  fixture.indexedArchive("a.zip", 4);
  fixture.indexedArchive("b.zip", 2);
  std::filesystem::path disconnected, moved;
  const auto result = runAll(fixture.repository, true, {},
      [&](const archive_file::UnzipProgress &progress) {
        if (progress.message != "Indexing extracted folders") return;
        auto session = fixture.repository.OpenSession();
        disconnected = session->LoadUnzipRecovery()->front().outputFolder;
        moved = disconnected.string() + "-offline";
        std::filesystem::rename(disconnected, moved);
      });
  assert(!disconnected.empty());
  std::filesystem::rename(moved, disconnected);
  assert(!result.success && !result.scanCommitted);
  auto session = fixture.repository.OpenSession();
  assert(session->LoadUnzipRecovery()->size() == 2);
  assert(archive_unzip_recovery::recover(*session).completed);
  assert(session->LoadUnzipRecovery()->empty());
  assert(session->CountAllChartMeta() == 6);
}

void unavailableOrInvalidCompletedOutputRetainsRecoveryWork() {
  Fixture fixture;
  fixture.indexedArchive("a.zip");
  fixture.indexedArchive("b.zip");
  runCrashingChild(fixture.root, "after-delete");
  auto session = fixture.repository.OpenSession();
  const auto record = session->LoadUnzipRecovery()->front();
  const auto moved = record.outputFolder.string() + "-offline";
  std::filesystem::rename(record.outputFolder, moved);
  assert(!archive_unzip_recovery::recover(*session).completed);
  assert(session->LoadUnzipRecovery()->size() == 1);
  assert(session->CountSolidArchives() == 2);
  std::filesystem::rename(moved, record.outputFolder);
  const auto marker = record.outputFolder / ".asobmashow_unzip_complete";
  std::ofstream(marker, std::ios::trunc) << "wrong key\n";
  assert(!archive_unzip_recovery::recover(*session).completed);
  assert(session->LoadUnzipRecovery()->size() == 1);
  assert(session->CountSolidArchives() == 2);
  std::ofstream(marker, std::ios::trunc) << record.archiveKey << '\n' << fspath_to_utf8(record.archivePath) << '\n';
  std::ofstream(record.outputFolder / ".asobmashow_unzip_incomplete") << "1\n";
  assert(archive_unzip_recovery::recover(*session).completed);
  assert(session->LoadUnzipRecovery()->empty());
  assert(session->CountSolidArchives() == 1 && session->CountAllChartMeta() == 1);
  assert(!std::filesystem::exists(record.outputFolder / ".asobmashow_unzip_incomplete"));
}

void batchDeletesEachOriginalBeforeStartingNextArchiveAndIndexesOnce() {
  Fixture fixture;
  const auto first = fixture.indexedArchive("a.zip");
  const auto second = fixture.indexedArchive("b.zip");
  const auto revision = fixture.repository.GetLibraryRevision();
  bool sawSecond = false;
  int indexStarts = 0;
  const auto result = runAll(fixture.repository, true, {},
      [&](const archive_file::UnzipProgress &progress) {
        assert(progress.fraction >= 0.0 && progress.fraction <= 1.0);
        if (progress.message.find("b.zip") != std::string::npos) {
          sawSecond = true;
          assert(!std::filesystem::exists(first.meta.BmsPath));
          auto session = fixture.repository.OpenSession();
          assert(session->CountSolidArchives() == 2);
          assert(fixture.repository.GetLibraryRevision() == revision);
          std::vector<bms_parser::ChartMeta> charts;
          session->SelectAllChartMeta(charts);
          assert(charts.empty());
        }
        if (progress.message == "Indexing extracted folders") {
          ++indexStarts;
          assert(fixture.repository.GetLibraryRevision() == revision);
          assert(!std::filesystem::exists(first.meta.BmsPath));
          assert(!std::filesystem::exists(second.meta.BmsPath));
        }
      });
  assert(result.success);
  assert(result.batch && result.archiveCount == 2);
  assert(result.completedCount == 2 && result.succeededCount == 2);
  assert(result.failedCount == 0 && result.deletionFailedCount == 0);
  assert(result.deletedCount == 2);
  assert(sawSecond);
  assert(indexStarts == 1);
  assert(result.chartPath.empty());
  assert(result.libraryChanged);
  assert(fixture.repository.GetLibraryRevision() == revision + 2);
  assert(!std::filesystem::exists(second.meta.BmsPath));
  auto session = fixture.repository.OpenSession();
  assert(session->CountSolidArchives() == 0);
  std::vector<bms_parser::ChartMeta> charts;
  session->SelectAllChartMeta(charts);
  assert(charts.size() == 2);
  for (const auto &chart : charts) {
    assert(std::filesystem::exists(chart.BmsPath));
  }
}

void batchKeepModeRetainsOriginalsAndIgnoresUnindexedArchives() {
  Fixture fixture;
  const auto first = fixture.indexedArchive("a.zip");
  const auto second = fixture.indexedArchive("b.zip");
  const auto unindexed = fixture.archive(1, "unindexed.zip");
  const auto revision = fixture.repository.GetLibraryRevision();
  const auto result = runAll(fixture.repository, false);
  assert(result.success && result.chartPath.empty());
  assert(result.succeededCount == 2 && result.deletedCount == 0);
  assert(std::filesystem::exists(first.meta.BmsPath));
  assert(std::filesystem::exists(second.meta.BmsPath));
  assert(std::filesystem::exists(unindexed.meta.BmsPath));
  auto session = fixture.repository.OpenSession();
  assert(session->CountSolidArchives() == 2);
  std::vector<bms_parser::ChartMeta> charts;
  session->SelectAllChartMeta(charts);
  assert(charts.size() == 2);
  assert(fixture.repository.GetLibraryRevision() == revision + 1);
}

void batchFailurePreservesOriginalAndContinues() {
  Fixture fixture;
  const auto first = fixture.indexedArchive("a.zip");
  const auto second = fixture.indexedArchive("b.zip");
  std::ofstream(first.meta.BmsPath, std::ios::trunc) << "not an archive";
  const auto result = runAll(fixture.repository, true);
  assert(!result.success && !result.cancelled);
  assert(result.completedCount == 2 && result.succeededCount == 1);
  assert(result.failedCount == 1 && result.deletedCount == 1);
  assert(std::filesystem::exists(first.meta.BmsPath));
  assert(!std::filesystem::exists(second.meta.BmsPath));
  auto session = fixture.repository.OpenSession();
  assert(session->CountSolidArchives() == 1);
  assert(session->CountAllChartMeta() == 1);
}

void batchDeleteReextractsInsteadOfTrustingCompletedFolder(bool removeFile) {
  Fixture fixture;
  const auto record = fixture.indexedArchive("song.zip");
  const auto original = ArchiveUnzipOperation::Run(record, fixture.repository, {});
  assert(original.success && !original.chartPath.empty());
  std::ifstream input(original.chartPath, std::ios::binary);
  const std::string contents((std::istreambuf_iterator<char>(input)), {});
  input.close();
  assert(!contents.empty());
  if (removeFile) {
    assert(std::filesystem::remove(original.chartPath));
  } else {
    std::ofstream(original.chartPath, std::ios::binary | std::ios::trunc)
        << std::string(contents.size(), 'x');
  }
  assert(std::filesystem::exists(original.outputFolder / ".asobmashow_unzip_complete"));
  const auto result = runAll(fixture.repository, true);
  assert(result.success && result.deletedCount == 1);
  assert(!std::filesystem::exists(record.meta.BmsPath));
  auto freshOutput = original.outputFolder;
  freshOutput += " 2";
  const auto freshChart = freshOutput / original.chartPath.lexically_relative(original.outputFolder);
  assert(std::filesystem::is_regular_file(freshChart));
  std::ifstream restored(freshChart, std::ios::binary);
  assert(std::string((std::istreambuf_iterator<char>(restored)), {}) == contents);
  assert(std::filesystem::exists(original.chartPath) == !removeFile);
}

void batchFailedFinalScanReportsFailureAndPreservesExtractedFiles() {
  Fixture fixture;
  const auto first = fixture.indexedArchive("a.zip");
  const auto second = fixture.indexedArchive("b.zip");
  fixture.failChartWrites();
  const auto result = runAll(fixture.repository, true);
  assert(!result.success && !result.cancelled);
  assert(result.completedCount == 2 && result.failedCount == 0);
  assert(result.succeededCount == 2 && result.deletedCount == 2);
  assert(!result.scanCommitted);
  assert(result.message.find("Failed to index extracted folders") != std::string::npos);
  assert(!std::filesystem::exists(first.meta.BmsPath));
  assert(!std::filesystem::exists(second.meta.BmsPath));
  assert(std::filesystem::exists(fixture.root / "a" / "song" / "chart0.bms"));
  assert(std::filesystem::exists(fixture.root / "b" / "song" / "chart0.bms"));
  auto session = fixture.repository.OpenSession();
  assert(session->CountSolidArchives() == 0);
  assert(session->CountAllChartMeta() == 0);
}

void batchCancellationPreservesCurrentAndRemainderAfterPriorDeletion() {
  Fixture fixture;
  const auto first = fixture.indexedArchive("a.zip");
  const auto second = fixture.indexedArchive("b.zip", 4);
  const auto third = fixture.indexedArchive("c.zip");
  bool sawThird = false;
  int indexStarts = 0;
  std::stop_source stop;
  const auto result = runAll(fixture.repository, true, stop.get_token(),
      [&](const archive_file::UnzipProgress &progress) {
        sawThird = sawThird || progress.message.find("c.zip") != std::string::npos;
        if (progress.message.find("b.zip") != std::string::npos &&
            progress.message.find("Preparing unzip") != std::string::npos) {
          stop.request_stop();
        }
        if (progress.message == "Indexing extracted folders") {
          ++indexStarts;
          assert(stop.stop_requested());
        }
      });
  assert(result.cancelled && !result.success);
  assert(result.archiveCount == 3 && result.completedCount == 1);
  assert(result.succeededCount == 1 && result.deletedCount == 1);
  assert(!sawThird);
  assert(indexStarts == 1 && result.scanCommitted);
  assert(result.libraryChanged && result.chartPath.empty());
  assert(!std::filesystem::exists(first.meta.BmsPath));
  assert(std::filesystem::exists(second.meta.BmsPath));
  assert(std::filesystem::exists(third.meta.BmsPath));
  auto session = fixture.repository.OpenSession();
  assert(session->CountSolidArchives() == 2);
  std::vector<bms_parser::ChartMeta> charts;
  session->SelectAllChartMeta(charts);
  assert(charts.size() == 1);
  for (const auto &chart : charts) {
    assert(chart.Title.find("c.zip") == std::string::npos);
  }
}

void cancellationDuringFinalIndexDoesNotInterruptTheScan() {
  Fixture fixture;
  fixture.indexedArchive("a.zip", 4);
  fixture.indexedArchive("b.zip", 4);
  std::stop_source stop;
  int indexStarts = 0;
  const auto result = runAll(fixture.repository, false, stop.get_token(),
      [&](const archive_file::UnzipProgress &progress) {
        if (progress.message == "Indexing extracted folders") {
          ++indexStarts;
          stop.request_stop();
        }
      });
  assert(indexStarts == 1);
  assert(result.cancelled && result.scanCommitted && result.libraryChanged);
  auto session = fixture.repository.OpenSession();
  assert(session->CountAllChartMeta() == 8);
  assert(session->CountSolidArchives() == 2);
}

void batchCancelledBeforeQueryDoesNotExtractOrDelete() {
  Fixture fixture;
  const auto record = fixture.indexedArchive("a.zip");
  std::stop_source stop;
  stop.request_stop();
  const auto result = runAll(fixture.repository, true, stop.get_token());
  assert(result.cancelled && !result.success && !result.libraryChanged);
  assert(std::filesystem::exists(record.meta.BmsPath));
  auto session = fixture.repository.OpenSession();
  assert(session->CountAllChartMeta() == 0);
  assert(session->CountSolidArchives() == 1);
}

void batchCleanupFailureRollsBackArchiveRecordsAndStillIndexesOutputs() {
  Fixture fixture;
  const auto first = fixture.indexedArchive("a.zip");
  const auto second = fixture.indexedArchive("b.zip");
  sqlite3 *database = nullptr;
  assert(sqlite3_open((fixture.root / "library.db").string().c_str(), &database) == SQLITE_OK);
  assert(sqlite3_exec(database,
      "CREATE TRIGGER reject_archive_delete BEFORE DELETE ON solid_archives "
      "WHEN OLD.path LIKE '%b.zip' BEGIN SELECT RAISE(FAIL, 'test cleanup failure'); END",
      nullptr, nullptr, nullptr) == SQLITE_OK);
  assert(sqlite3_close(database) == SQLITE_OK);
  const auto result = runAll(fixture.repository, true);
  assert(!result.success && result.scanCommitted && result.libraryChanged);
  assert(result.succeededCount == 2 && result.failedCount == 0);
  assert(result.deletedCount == 2 && result.deletionFailedCount == 2);
  assert(!std::filesystem::exists(first.meta.BmsPath));
  assert(!std::filesystem::exists(second.meta.BmsPath));
  assert(result.message.find("Failed to remove deleted archive records") != std::string::npos);
  auto session = fixture.repository.OpenSession();
  assert(session->CountSolidArchives() == 2);
  assert(session->CountAllChartMeta() == 2);
}

void batchWorkerGuardsDuplicateStartsAndKeepsShutdownNotifications() {
  Fixture fixture;
  const auto first = fixture.indexedArchive("a.zip", 4);
  const auto second = fixture.indexedArchive("b.zip");
  ArchiveUnzipOperation operation(fixture.repository);
  const auto revision = fixture.repository.GetLibraryRevision();
  assert(operation.startAll(false));
  assert(!operation.startAll(true));
  assert(!operation.start(first));
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (fixture.repository.GetLibraryRevision() == revision &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  assert(fixture.repository.GetLibraryRevision() > revision);
  operation.cancelAndWait();
  operation.cancelAndWait();
  assert(!operation.inProgress());
  assert(!operation.takeProgress() && !operation.takeResult());
  assert(operation.takeLibraryChanged());
  assert(!operation.takeLibraryChanged());
  assert(!operation.canDeleteArchive());
  assert(std::filesystem::exists(first.meta.BmsPath));
  assert(std::filesystem::exists(second.meta.BmsPath));
  assert(operation.startAll(true));
  const auto result = waitForResult(operation);
  assert(result.batch && result.success && result.deletedCount == 2);
  assert(result.chartPath.empty() && !operation.canDeleteArchive());
  assert(operation.takeLibraryChanged());
  assert(!operation.takeLibraryChanged());
}

void emptyBatchCompletesWithoutLibraryChanges() {
  Fixture fixture;
  ArchiveUnzipOperation operation(fixture.repository);
  assert(operation.startAll(true));
  const auto result = waitForResult(operation);
  assert(result.batch && result.success && !result.cancelled);
  assert(result.archiveCount == 0 && result.completedCount == 0);
  assert(result.chartPath.empty() && !result.libraryChanged);
  assert(!operation.takeLibraryChanged() && !operation.canDeleteArchive());
}

void successPreservesUnrelatedLibraryAndRequiresExplicitDeletion() {
  Fixture fixture;
  const auto unrelated = fixture.root / "unrelated";
  std::filesystem::create_directories(unrelated);
  std::ofstream(unrelated / "other.bms") << "#TITLE Other\n#BPM 150\n#00111:01\n";
  auto session = fixture.repository.OpenSession();
  ChartLibraryScanner scanner;
  const auto initial = scanner.ScanAddedWithResult(*session, {unrelated});
  assert(initial.completed && initial.committed);
  std::filesystem::remove_all(unrelated);
  const auto record = fixture.archive();
  ArchiveUnzipOperation operation(fixture.repository);
  std::string message;
  assert(!operation.deleteArchive(message));
  assert(operation.start(record));
  assert(!operation.start(record));
  const auto result = waitForResult(operation);
  assert(result.success && !result.cancelled);
  assert(!operation.inProgress());
  assert(!result.outputFolder.empty());
  assert(std::filesystem::exists(result.chartPath));
  assert(result.chartPath.lexically_relative(result.outputFolder).begin()->string() != "..");
  assert(result.archivePath == record.meta.BmsPath);
  assert(std::filesystem::exists(record.meta.BmsPath));
  std::vector<bms_parser::ChartMeta> charts;
  session->SelectAllChartMeta(charts);
  assert(charts.size() == 2);
  assert(operation.canDeleteArchive());
  assert(operation.deleteArchive(message));
  assert(!std::filesystem::exists(record.meta.BmsPath));
  assert(std::filesystem::exists(result.chartPath));
  assert(!operation.canDeleteArchive());
  assert(!operation.deleteArchive(message));
}

void keepAndDestructionNeverDeleteArchive() {
  Fixture fixture;
  const auto record = fixture.archive();
  {
    ArchiveUnzipOperation operation(fixture.repository);
    assert(operation.start(record));
    assert(waitForResult(operation).success);
    operation.keepArchive();
    std::string message;
    assert(!operation.canDeleteArchive());
    assert(!operation.deleteArchive(message));
  }
  assert(std::filesystem::exists(record.meta.BmsPath));
}

void failuresNeverOfferDeletion() {
  Fixture fixture;
  auto record = fixture.archive();
  std::ofstream(record.meta.BmsPath, std::ios::trunc) << "not an archive";
  ArchiveUnzipOperation operation(fixture.repository);
  assert(operation.start(record));
  const auto result = waitForResult(operation);
  assert(!result.success && !result.cancelled);
  assert(!result.message.empty());
  assert(!operation.canDeleteArchive());
  assert(std::filesystem::exists(record.meta.BmsPath));
  record.solidArchive = false;
  assert(!operation.start(record));
}

void cancellationBeforeExtractionAndDuringRefreshIsNotSuccess() {
  Fixture fixture;
  const auto record = fixture.archive();
  std::stop_source cancelled;
  cancelled.request_stop();
  const auto before = ArchiveUnzipOperation::Run(record, fixture.repository, cancelled.get_token());
  assert(before.cancelled && !before.success);
  assert(before.outputFolder.empty());
  std::stop_source during;
  const auto after = ArchiveUnzipOperation::Run(record, fixture.repository, during.get_token(),
      [&](const archive_file::UnzipProgress &progress) {
        if (progress.message == "Refreshing library") {
          during.request_stop();
        }
      });
  assert(after.cancelled && !after.success);
  assert(!after.outputFolder.empty());
  assert(std::filesystem::exists(record.meta.BmsPath));
}

void failedScanIsNotSuccess() {
  Fixture fixture;
  const auto record = fixture.archive();
  fixture.failChartWrites();
  ArchiveUnzipOperation operation(fixture.repository);
  assert(operation.start(record));
  const auto result = waitForResult(operation);
  assert(!result.success && !result.cancelled);
  assert(!result.outputFolder.empty());
  assert(!operation.canDeleteArchive());
  assert(std::filesystem::exists(record.meta.BmsPath));
}

void shutdownDiscardsPendingPublicationsAndAllowsRestart() {
  Fixture fixture;
  const auto record = fixture.archive();
  ArchiveUnzipOperation operation(fixture.repository);
  assert(operation.start(record));
  operation.cancelAndWait();
  assert(!operation.inProgress());
  assert(!operation.takeResult());
  assert(!operation.takeProgress());
  assert(!operation.canDeleteArchive());
  assert(std::filesystem::exists(record.meta.BmsPath));
  assert(operation.start(record));
  assert(waitForResult(operation).success);
}

void cancelledScanExposesCommittedChangesWithoutSuccess() {
  Fixture fixture;
  const auto record = fixture.archive(4);
  const auto revision = fixture.repository.GetLibraryRevision();
  std::stop_source stop;
  const auto result = ArchiveUnzipOperation::Run(
      record, fixture.repository, stop.get_token(),
      [&](const archive_file::UnzipProgress &progress) {
        if (progress.message == "Indexing extracted charts" && progress.current > 0) {
          stop.request_stop();
        }
      });
  assert(result.cancelled && !result.success);
  auto session = fixture.repository.OpenSession();
  std::vector<bms_parser::ChartMeta> charts;
  session->SelectAllChartMeta(charts);
  assert(!charts.empty() && charts.size() < 4);
  assert(fixture.repository.GetLibraryRevision() > revision);
  assert(result.libraryChanged);
  assert(result.scanCommitted);
  assert(std::filesystem::exists(record.meta.BmsPath));
}

void unavailableAndMissingArchivesNeverChangeLibraryOrAllowDeletion() {
  Fixture fixture;
  auto record = fixture.archive();
  ArchiveUnzipOperation operation(fixture.repository);
  record.unavailable = true;
  assert(!operation.start(record));
  assert(!operation.inProgress());
  assert(!operation.takeResult());
  assert(!operation.takeLibraryChanged());
  assert(std::filesystem::exists(record.meta.BmsPath));
  record.unavailable = false;
  std::filesystem::remove(record.meta.BmsPath);
  assert(operation.start(record));
  const auto result = waitForResult(operation);
  assert(!result.success && !result.cancelled);
  assert(!result.libraryChanged && !result.scanCommitted);
  assert(!operation.takeLibraryChanged());
  assert(!operation.canDeleteArchive());
  assert(result.outputFolder.empty());
}

void cancelAndWaitPreservesCommittedChangeNotificationExactlyOnce() {
  Fixture fixture;
  const auto record = fixture.archive(4);
  ArchiveUnzipOperation operation(fixture.repository);
  const auto revision = fixture.repository.GetLibraryRevision();
  assert(!operation.takeLibraryChanged());
  assert(operation.start(record));
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (fixture.repository.GetLibraryRevision() == revision &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  assert(fixture.repository.GetLibraryRevision() > revision);
  operation.cancelAndWait();
  operation.cancelAndWait();
  assert(!operation.takeResult());
  assert(!operation.takeProgress());
  assert(operation.takeLibraryChanged());
  assert(!operation.takeLibraryChanged());
  assert(!operation.canDeleteArchive());
  assert(std::filesystem::exists(record.meta.BmsPath));
}

}

int main(int argc, char **argv) {
  if (argc == 4 && std::string(argv[1]) == "--crash-unzip") {
    crashUnzip(argv[2], argv[3]);
  }
  if (argc == 4 && std::string(argv[1]) == "--crash-recovery") {
    crashRecovery(argv[2]);
  }
  testExecutable = std::filesystem::absolute(argv[0]);
  disconnectedOutputDuringFinalIndexRemainsQueuedAlongsideHealthyOutputs();
  cancellingAnUnzipWaitingForRecoveryDoesNotBlockShutdown();
  inaccessibleChartSubfolderRetainsRecoveryUntilItCanBeIndexed();
  inaccessibleChartSubfolderRetainsRecoveryUntilItCanBeIndexed(true);
  tornMarkerLeavesTheOriginalAndPartialOutputAlone();
  journalNormalizesPathAliasesForRecoveryAndAcknowledgement();
  unexpectedExitPreservesRecoveryWork();
  partialExtractionIsNotIndexedByOrdinaryStartupScan();
  for (const auto &phase : {"complete", "after-delete", "before-index", "during-index"}) {
    restartRecoversEveryCompletedCrashBoundary(phase);
  }
  recoveryRetriesFailedCleanupIndexAndAcknowledgement();
  journalFailurePreventsExtractionAndDeletion();
  unavailableOrInvalidCompletedOutputRetainsRecoveryWork();
  batchDeletesEachOriginalBeforeStartingNextArchiveAndIndexesOnce();
  batchKeepModeRetainsOriginalsAndIgnoresUnindexedArchives();
  batchFailurePreservesOriginalAndContinues();
  batchDeleteReextractsInsteadOfTrustingCompletedFolder(true);
  batchDeleteReextractsInsteadOfTrustingCompletedFolder(false);
  batchFailedFinalScanReportsFailureAndPreservesExtractedFiles();
  batchCancellationPreservesCurrentAndRemainderAfterPriorDeletion();
  cancellationDuringFinalIndexDoesNotInterruptTheScan();
  batchCancelledBeforeQueryDoesNotExtractOrDelete();
  batchCleanupFailureRollsBackArchiveRecordsAndStillIndexesOutputs();
  batchWorkerGuardsDuplicateStartsAndKeepsShutdownNotifications();
  emptyBatchCompletesWithoutLibraryChanges();
  successPreservesUnrelatedLibraryAndRequiresExplicitDeletion();
  keepAndDestructionNeverDeleteArchive();
  failuresNeverOfferDeletion();
  cancellationBeforeExtractionAndDuringRefreshIsNotSuccess();
  failedScanIsNotSuccess();
  shutdownDiscardsPendingPublicationsAndAllowsRestart();
  cancelledScanExposesCommittedChangesWithoutSuccess();
  unavailableAndMissingArchivesNeverChangeLibraryOrAllowDeletion();
  cancelAndWaitPreservesCommittedChangeNotificationExactlyOnce();
  std::cout << "archive_unzip_operation_tests passed\n";
}
