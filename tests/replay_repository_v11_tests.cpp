#include "repositories/ReplayRepository.h"
#include "ir/IrSubmissionSnapshot.h"
#include "replay/BeatorajaReplayCodec.h"
#include "replay/ReplayFileStore.h"
#include "replay/ReplayPlaybackData.h"
#include "sqlite3.h"

#include <algorithm>
#include <barrier>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

std::string repeated(char value, std::size_t count) {
  return std::string(count, value);
}

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    std::mt19937_64 random(std::random_device{}());
    const auto base = std::filesystem::temp_directory_path();
    for (int attempt = 0; attempt < 100; ++attempt) {
      path_ = base / ("asobmashow-replay-v11-" + std::to_string(random()));
      std::error_code error;
      if (std::filesystem::create_directory(path_, error)) {
        return;
      }
    }
    throw std::runtime_error("could not create replay repository test folder");
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

class Database {
public:
  explicit Database(const std::filesystem::path &path) {
    if (sqlite3_open_v2(path.string().c_str(), &database_,
                        SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
      throw std::runtime_error("could not open replay test database");
    }
    sqlite3_extended_result_codes(database_, 1);
    sqlite3_exec(database_, "PRAGMA foreign_keys=ON", nullptr, nullptr,
                 nullptr);
  }

  ~Database() { sqlite3_close(database_); }
  sqlite3 *get() const { return database_; }

private:
  sqlite3 *database_ = nullptr;
};

bool execute(sqlite3 *database, std::string_view sql) {
  char *error = nullptr;
  const int result = sqlite3_exec(database, std::string(sql).c_str(), nullptr,
                                  nullptr, &error);
  if (result != SQLITE_OK) {
    sqlite3_free(error);
    return false;
  }
  return true;
}

std::int64_t scalarInteger(sqlite3 *database, std::string_view sql) {
  sqlite3_stmt *statement = nullptr;
  if (sqlite3_prepare_v2(database, std::string(sql).c_str(), -1, &statement,
                         nullptr) != SQLITE_OK ||
      sqlite3_step(statement) != SQLITE_ROW) {
    sqlite3_finalize(statement);
    return -1;
  }
  const auto value = sqlite3_column_int64(statement, 0);
  const bool complete = sqlite3_step(statement) == SQLITE_DONE;
  sqlite3_finalize(statement);
  return complete ? value : -1;
}

std::set<std::string> stringSet(sqlite3 *database, std::string_view sql) {
  std::set<std::string> values;
  sqlite3_stmt *statement = nullptr;
  if (sqlite3_prepare_v2(database, std::string(sql).c_str(), -1, &statement,
                         nullptr) != SQLITE_OK) {
    return values;
  }
  int step = SQLITE_OK;
  while ((step = sqlite3_step(statement)) == SQLITE_ROW) {
    const auto *text = sqlite3_column_text(statement, 0);
    if (text != nullptr) {
      values.emplace(reinterpret_cast<const char *>(text));
    }
  }
  if (step != SQLITE_DONE) {
    values.clear();
  }
  sqlite3_finalize(statement);
  return values;
}

result_persistence::PersistedChartResult validResult(std::string attemptId) {
  result_persistence::PersistedChartResult result;
  result.attemptId = std::move(attemptId);
  result.score.chartPath = "sample/song.bms";
  result.score.chartMd5 = repeated('b', 32);
  result.score.chartSha256 = repeated('a', 64);
  result.score.chartTitle = "Title";
  result.score.chartArtist = "Artist";
  result.score.longNoteMode = 1;
  result.score.score = 7;
  result.score.maxScore = 10;
  result.score.maxCombo = 4;
  result.score.comboBreak = 1;
  result.score.pGreat = 3;
  result.score.great = 1;
  result.score.good = 1;
  result.score.fast = 2;
  result.score.slow = 1;
  result.score.finalGauge = 82.5F;
  result.score.clearType = kClearTypeNormalClearRank;
  result.score.provenance = ScoreProvenance::Legacy();
  result.keyMode = 7;
  result.adoptedGaugeHistory = {20.0F, 48.5F, 82.5F};
  result.judgementTiming = result_persistence::ChartJudgementTiming{};
  result.judgementTiming->byJudgement[PGreat] = {.fast = 1, .slow = 1};
  result.judgementTiming->byJudgement[Great] = {.fast = 1, .slow = 0};
  result.playedAtUnixMillis = 1'700'000'000'123LL;
  result.resultFingerprint = result_persistence::resultFingerprint(result);
  return result;
}

ir::IrSubmissionSnapshot
snapshotFor(const result_persistence::PersistedChartResult &result) {
  std::string diagnostic;
  const auto snapshot = ir::captureIrSubmissionSnapshot(result, diagnostic);
  expect(snapshot.has_value(),
         "valid result captures a standalone IR snapshot");
  return snapshot.value_or(ir::IrSubmissionSnapshot{});
}

ReplayFileReference referenceFor(const ReplayFileReservation &reservation,
                                 char hash = 'c') {
  return {.stem = reservation.stem,
          .historyIndex = reservation.historyIndex,
          .relativePath = reservation.relativePath,
          .contentSha256 = repeated(hash, 64),
          .compressedSize = 1234,
          .codecVersion = replay::BeatorajaReplayCodec::kCodecVersion};
}

replay::ReplayPlaybackData samplePlayback() {
  replay::ReplayPlaybackData playback;
  playback.setup.chartMd5 = repeated('b', 32);
  playback.setup.chartSha256 = repeated('a', 64);
  playback.setup.keyMode = 7;
  playback.setup.longNoteMode = 1;
  playback.setup.playOption = "NORMAL";
  playback.setup.playOption2 = "NORMAL";
  playback.setup.playbackRulesetId = "beatoraja";
  playback.setup.playbackRulesetRevision = 1;
  playback.input = {
      {.songTimeMicros = 1000,
       .control = {.kind = replay::LogicalControlKind::Lane,
                   .player = 1,
                   .lane = 0},
       .pressed = true},
      {.songTimeMicros = 2000,
       .control = {.kind = replay::LogicalControlKind::Lane,
                   .player = 1,
                   .lane = 0},
       .pressed = false},
  };
  return playback;
}

void testReplayFileReadIsIndependentFromResultAndIr() {
  TemporaryDirectory temporary;
  ReplayRepository repository(temporary.path() / "replay.db");
  expect(repository.EnsureSchema(), "file-backed read schema is available");
  constexpr std::string_view attempt =
      "123e4567-e89b-42d3-a456-426614174001";
  const std::string stem = repeated('a', 64);
  const auto reservation = repository.reserveReplayFile(attempt, stem);
  expect(reservation.reservation.has_value(),
         "file-backed read reserves a replay path");
  if (!reservation.reservation.has_value()) {
    return;
  }

  const auto playback = samplePlayback();
  replay::BeatorajaReplayCodec codec;
  std::string diagnostic;
  const auto encoded = codec.encodeChart(
      playback, 1'700'000'000'123LL, diagnostic);
  replay::ReplayFileStore store(temporary.path());
  const replay::ReplayPathIdentity path{
      .stem = reservation.reservation->stem,
      .historyIndex = reservation.reservation->historyIndex,
      .relativePath = reservation.reservation->relativePath,
  };
  const auto finalized =
      encoded.has_value()
          ? store.finalize(path, *encoded, codec,
                           {.stageSha256 = {repeated('a', 64)},
                            .course = false},
                           "repository_read")
          : replay::FinalizeOutcome{};
  expect(finalized.metadata.has_value(),
         "file-backed read installs a valid replay file");
  if (!finalized.metadata.has_value()) {
    return;
  }

  auto result = validResult(std::string(attempt));
  const auto snapshot = snapshotFor(result);
  const ReplayFileReference reference{
      .stem = reservation.reservation->stem,
      .historyIndex = reservation.reservation->historyIndex,
      .relativePath = finalized.metadata->relativePath,
      .contentSha256 = finalized.metadata->sha256,
      .compressedSize = finalized.metadata->compressedSize,
      .codecVersion = finalized.metadata->codecVersion,
  };
  const auto staged =
      repository.stageCompletedChartAttempt(result, snapshot, reference, {});
  expect(staged.receipt.has_value(),
         "file-backed read stages its compact result");
  if (!staged.receipt.has_value()) {
    return;
  }

  const auto loaded =
      repository.loadChartReplayPlayback(staged.receipt->resultId);
  expect(loaded.status == ChartReplayPlaybackReadOutcome::Status::Loaded &&
             loaded.playback.has_value() && *loaded.playback == playback,
         "watch loads and decodes raw input from the referenced brd file");

  expect(store.remove(*finalized.metadata, diagnostic),
         "test user deletion removes only the replay file");
  const auto unavailable =
      repository.loadChartReplayPlayback(staged.receipt->resultId);
  expect(unavailable.status ==
             ChartReplayPlaybackReadOutcome::Status::ReplayUnavailable,
         "watch reports replay unavailable after user file deletion");
  expect(repository.loadChartResult(staged.receipt->resultId).status ==
             ResultReadOutcome::Status::Loaded,
         "result recall remains available after replay file deletion");
  expect(repository.loadIrSubmissionSnapshot(attempt).status ==
             ir::IrSubmissionSnapshotReadOutcome::Status::Loaded,
         "manual IR snapshot remains available after replay file deletion");
}

void testFreshCompactSchema() {
  TemporaryDirectory temporary;
  const auto databasePath = temporary.path() / "replay.db";
  ReplayRepository repository(databasePath);
  expect(repository.EnsureSchema(), "fresh v11 replay schema is created");
  repository.Shutdown();

  Database database(databasePath);
  expect(scalarInteger(database.get(), "PRAGMA user_version") == 11,
         "fresh replay database uses schema version 11");
  const auto tables = stringSet(
      database.get(),
      "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name");
  for (std::string_view required :
       {"chart_results", "course_results", "course_result_stages",
        "replay_files", "replay_file_reservations", "replay_stem_sequences",
        "ir_submission_snapshots", "pending_chart_score_writes", "ir_outbox",
        "ir_submission_receipts", "ir_remote_scores"}) {
    expect(tables.contains(std::string(required)),
           std::string("compact schema contains ") + std::string(required));
  }
  for (std::string_view legacy :
       {"replays", "replay_events", "replay_touch_samples",
        "replay_lane_cover_events"}) {
    expect(!tables.contains(std::string(legacy)),
           std::string("fresh schema omits legacy table ") +
               std::string(legacy));
  }
  const auto indexes = stringSet(
      database.get(),
      "SELECT name FROM sqlite_master WHERE type='index' ORDER BY name");
  for (std::string_view required :
       {"idx_chart_results_sha256_played", "idx_course_results_key_played",
        "idx_replay_files_chart_result", "idx_replay_reservations_stem_index",
        "idx_ir_submission_snapshots_fingerprint"}) {
    expect(indexes.contains(std::string(required)),
           std::string("compact schema contains index ") +
               std::string(required));
  }
  const auto pendingColumns =
      stringSet(database.get(), "SELECT name FROM pragma_table_info("
                                "'pending_chart_score_writes')");
  expect(pendingColumns.contains("result_id") &&
             !pendingColumns.contains("replay_id"),
         "pending score rows reference results instead of replay rows");
  const auto replayForeignTables =
      stringSet(database.get(), "SELECT \"table\" FROM pragma_foreign_key_list("
                                "'replay_files')");
  expect(replayForeignTables.contains("chart_results") &&
             replayForeignTables.contains("course_results"),
         "replay files associate with compact chart or course results");
}

void testMalformedVersion11FailsClosed() {
  TemporaryDirectory temporary;
  const auto databasePath = temporary.path() / "replay.db";
  ReplayRepository repository(databasePath);
  expect(repository.EnsureSchema(), "v11 validation fixture is created");
  repository.Shutdown();
  {
    Database database(databasePath);
    expect(execute(database.get(),
                   "DROP INDEX idx_replay_reservations_stem_index"),
           "v11 validation fixture drops one required index");
  }
  expect(!repository.EnsureSchema(),
         "partial v11 schema fails closed instead of self-repairing");
}

void testCompactStageAndIndependentReads() {
  TemporaryDirectory temporary;
  const auto databasePath = temporary.path() / "replay.db";
  ReplayRepository repository(databasePath);
  expect(repository.EnsureSchema(), "compact staging schema is available");

  constexpr std::string_view attempt = "123e4567-e89b-42d3-a456-426614174000";
  const std::string stem = repeated('a', 64);
  const auto reservation = repository.reserveReplayFile(attempt, stem);
  expect(reservation.status == ReservationOutcome::Status::Reserved &&
             reservation.reservation.has_value(),
         "first replay file reservation succeeds");
  if (!reservation.reservation.has_value()) {
    return;
  }

  auto result = validResult(std::string(attempt));
  const auto snapshot = snapshotFor(result);
  const auto replayFile = referenceFor(*reservation.reservation);

  replay::ReplayPlaybackData rawReplay;
  rawReplay.input.resize(100'000);
  const auto staged =
      repository.stageCompletedChartAttempt(result, snapshot, replayFile, {});
  expect(staged.status == result_persistence::StageStatus::Staged &&
             staged.receipt.has_value() && staged.receipt->resultId > 0,
         "compact chart result stages with one replay-file reference");
  if (!staged.receipt.has_value()) {
    return;
  }
  const int resultId = staged.receipt->resultId;
  result.resultId = resultId;
  const auto loaded = repository.loadChartResult(resultId);
  expect(loaded.status == ResultReadOutcome::Status::Loaded &&
             loaded.record.has_value() && loaded.record->result == result &&
             loaded.record->replayFile.has_value(),
         "result facts and replay reference load without replay events");
  if (loaded.record && loaded.record->replayFile) {
    auto expectedReference = replayFile;
    expectedReference.id = loaded.record->replayFile->id;
    expectedReference.recordId = resultId;
    expect(*loaded.record->replayFile == expectedReference,
           "loaded replay-file metadata is exact");
  }
  const auto loadedSnapshot = repository.loadIrSubmissionSnapshot(attempt);
  expect(loadedSnapshot.status ==
                 ir::IrSubmissionSnapshotReadOutcome::Status::Loaded &&
             loadedSnapshot.snapshot == snapshot,
         "postponed IR snapshot loads independently from replay bytes");

  auto idempotentInput = result;
  idempotentInput.resultId = 0;
  const auto repeatedStage = repository.stageCompletedChartAttempt(
      idempotentInput, snapshot, replayFile, {});
  expect(
      repeatedStage.status == result_persistence::StageStatus::AlreadyStaged &&
          repeatedStage.receipt && repeatedStage.receipt->resultId == resultId,
      "exact repeated staging is idempotent");
  auto conflictingFile = replayFile;
  conflictingFile.contentSha256[0] = 'd';
  const auto conflict = repository.stageCompletedChartAttempt(
      idempotentInput, snapshot, conflictingFile, {});
  expect(conflict.status == result_persistence::StageStatus::IntegrityConflict,
         "same attempt with different file metadata fails closed");
  auto anotherResult = idempotentInput;
  ++anotherResult.playedAtUnixMillis;
  anotherResult.resultFingerprint =
      result_persistence::resultFingerprint(anotherResult);
  const auto unrelatedSnapshot = snapshotFor(anotherResult);
  const auto snapshotConflict = repository.stageCompletedChartAttempt(
      idempotentInput, unrelatedSnapshot, replayFile, {});
  expect(snapshotConflict.status ==
             result_persistence::StageStatus::IntegrityConflict,
         "IR snapshot must be derived from the exact compact result");

  repository.Shutdown();
  Database database(databasePath);
  expect(scalarInteger(database.get(), "SELECT count(*) FROM chart_results") ==
                 1 &&
             scalarInteger(database.get(),
                           "SELECT count(*) FROM replay_files") == 1 &&
             scalarInteger(database.get(),
                           "SELECT count(*) FROM ir_submission_snapshots") == 1,
         "100,000 replay inputs add no SQLite rows");
  expect(scalarInteger(database.get(),
                       "SELECT count(*) FROM pending_chart_score_writes") == 1,
         "compact staging retains one pending score projection");
  expect(!execute(database.get(),
                  "INSERT INTO replay_files(chart_result_id,course_result_id,"
                  "stem,history_index,relative_path,content_sha256,"
                  "compressed_size,codec_version) VALUES(" +
                      std::to_string(resultId) + ",NULL,'" + repeated('d', 64) +
                      "',77,'replay/association-conflict.brd','" +
                      repeated('e', 64) + "',1,2)"),
         "one compact result cannot acquire a second replay association");
}

void testReservationIntegrityAndMonotonicity() {
  TemporaryDirectory temporary;
  const auto databasePath = temporary.path() / "replay.db";
  ReplayRepository repository(databasePath);
  expect(repository.EnsureSchema(), "reservation schema is available");
  const std::string stem = repeated('e', 64);
  constexpr std::string_view firstAttempt =
      "123e4567-e89b-42d3-a456-426614174010";
  const auto first = repository.reserveReplayFile(firstAttempt, stem);
  const auto repeatedReservation =
      repository.reserveReplayFile(firstAttempt, stem);
  expect(first.status == ReservationOutcome::Status::Reserved &&
             first.reservation &&
             repeatedReservation.status ==
                 ReservationOutcome::Status::AlreadyReserved &&
             repeatedReservation.reservation == first.reservation,
         "repeated attempt and stem reuse one reservation");
  const auto wrongStem =
      repository.reserveReplayFile(firstAttempt, repeated('f', 64));
  expect(wrongStem.status == ReservationOutcome::Status::IntegrityConflict,
         "one attempt cannot switch replay stems");

  constexpr int concurrentCount = 4;
  std::barrier<> start(concurrentCount);
  std::mutex resultMutex;
  std::vector<std::int64_t> indexes;
  std::vector<std::thread> threads;
  for (int index = 0; index < concurrentCount; ++index) {
    threads.emplace_back([&, index] {
      start.arrive_and_wait();
      const std::string attempt =
          "123e4567-e89b-42d3-a456-42661417402" + std::to_string(index);
      const auto outcome = repository.reserveReplayFile(attempt, stem);
      std::lock_guard lock(resultMutex);
      if (outcome.status == ReservationOutcome::Status::Reserved &&
          outcome.reservation) {
        indexes.push_back(outcome.reservation->historyIndex);
      }
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }
  std::ranges::sort(indexes);
  expect(indexes == std::vector<std::int64_t>({1, 2, 3, 4}),
         "concurrent reservations receive consecutive unique indexes");

  repository.Shutdown();
  {
    Database database(databasePath);
    const std::string existingPath =
        first.reservation ? first.reservation->relativePath.generic_string()
                          : "replay/missing.brd";
    expect(!execute(database.get(),
                    "INSERT INTO replay_file_reservations(attempt_id,stem,"
                    "history_index,relative_path,created_at_unix_ms) VALUES("
                    "'123e4567-e89b-42d3-a456-426614174099','x',0,'" +
                        existingPath + "',1)"),
           "direct relative-path reuse violates a database constraint");
    expect(!execute(database.get(),
                    "INSERT INTO replay_file_reservations(attempt_id,stem,"
                    "history_index,relative_path,created_at_unix_ms) VALUES("
                    "'123e4567-e89b-42d3-a456-426614174098','" +
                        stem + "',0,'replay/unused-pair.brd',1)"),
           "direct stem/index reuse violates a database constraint");
    expect(!execute(database.get(),
                    "INSERT INTO replay_file_reservations(attempt_id,stem,"
                    "history_index,relative_path,created_at_unix_ms) VALUES('" +
                        std::string(firstAttempt) +
                        "','x',99,'replay/unused-attempt.brd',1)"),
           "direct attempt-ID reuse violates a database constraint");

    expect(
        execute(database.get(),
                "DELETE FROM replay_file_reservations WHERE stem='" + stem +
                    "'"),
        "test can remove live reservations without removing high-water mark");
  }
  const auto afterDeletion = repository.reserveReplayFile(
      "123e4567-e89b-42d3-a456-426614174030", stem);
  expect(afterDeletion.status == ReservationOutcome::Status::Reserved &&
             afterDeletion.reservation &&
             afterDeletion.reservation->historyIndex == 5,
         "deleted reservation indexes are never reused");
}

void testMalformedVersion10FailsClosed() {
  TemporaryDirectory temporary;
  const auto databasePath = temporary.path() / "replay.db";
  sqlite3 *raw = nullptr;
  expect(sqlite3_open(databasePath.string().c_str(), &raw) == SQLITE_OK,
         "legacy fixture database opens");
  expect(execute(raw, "CREATE TABLE replays(id INTEGER PRIMARY KEY);"
                      "INSERT INTO replays(id) VALUES(7);"
                      "PRAGMA user_version=10;"),
         "legacy v10 fixture is created");
  sqlite3_close(raw);

  ReplayRepository repository(databasePath);
  expect(!repository.EnsureSchema(),
         "malformed v10 database fails closed during atomic migration");
  repository.Shutdown();

  Database database(databasePath);
  expect(scalarInteger(database.get(), "PRAGMA user_version") == 10 &&
             scalarInteger(database.get(), "SELECT count(*) FROM replays") ==
                 1 &&
             scalarInteger(database.get(),
                           "SELECT count(*) FROM sqlite_master WHERE "
                           "type='table' AND name='chart_results'") == 0,
         "malformed v10 migration leaves legacy data and schema untouched");
}

} // namespace

int main() {
  testFreshCompactSchema();
  testMalformedVersion11FailsClosed();
  testCompactStageAndIndependentReads();
  testReplayFileReadIsIndependentFromResultAndIr();
  testReservationIntegrityAndMonotonicity();
  testMalformedVersion10FailsClosed();
  if (failures != 0) {
    std::cerr << failures << " replay repository v11 test(s) failed\n";
    return 1;
  }
  std::cout << "Replay repository v11 tests passed\n";
  return 0;
}
