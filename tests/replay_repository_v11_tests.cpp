#include "repositories/ReplayRepository.h"
#include "FileChecksum.h"
#include "ir/IrSubmissionSnapshot.h"
#include "replay/BeatorajaReplayCodec.h"
#include "replay/ReplayFileStore.h"
#include "replay/ReplayPlaybackData.h"
#include "sqlite3.h"

#include <algorithm>
#include <array>
#include <barrier>
#include <chrono>
#include <filesystem>
#include <fstream>
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

std::string sqlQuote(std::string_view value) {
  std::string result("'");
  for (const char character : value) {
    result.push_back(character);
    if (character == '\'') {
      result.push_back('\'');
    }
  }
  result.push_back('\'');
  return result;
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

void writeFile(const std::filesystem::path &path, std::string_view contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

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
  result.adoptedGaugeType = GaugeType::Easy;
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

replay::ReplayFileMetadata metadataForFile(
    const std::filesystem::path &root,
    const std::filesystem::path &relativePath) {
  std::string diagnostic;
  const auto absolute = root / relativePath;
  const auto hash = file_checksum::sha256File(absolute, diagnostic);
  return {.relativePath = relativePath,
          .sha256 = hash.value_or(std::string{}),
          .compressedSize = std::filesystem::file_size(absolute),
          .codecVersion = replay::BeatorajaReplayCodec::kCodecVersion};
}

void testReplayFileReadIsIndependentFromResultAndIr() {
  TemporaryDirectory temporary;
  ReplayRepository repository(temporary.path() / "replay.db");
  expect(repository.EnsureSchema(), "file-backed read schema is available");
  constexpr std::string_view attempt = "123e4567-e89b-42d3-a456-426614174001";
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
  const auto encoded =
      codec.encodeChart(playback, 1'700'000'000'123LL, diagnostic);
  replay::ReplayFileStore store(temporary.path());
  const replay::ReplayPathIdentity path{
      .stem = reservation.reservation->stem,
      .historyIndex = reservation.reservation->historyIndex,
      .relativePath = reservation.reservation->relativePath,
  };
  const auto finalized =
      encoded.has_value() ? store.finalize(path, *encoded, codec,
                                           {.stageSha256 = {repeated('a', 64)},
                                            .stageLongNoteModes = {1},
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
  expect(repository.markReplayFileReservationFinalized(
             *reservation.reservation, *finalized.metadata, diagnostic),
         "file-backed read records finalized replay ownership");
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

void testChartReplayRejectsLongNoteModeMismatch() {
  TemporaryDirectory temporary;
  ReplayRepository repository(temporary.path() / "replay.db");
  expect(repository.EnsureSchema(),
         "long-note mismatch replay schema is available");
  constexpr std::string_view attempt =
      "123e4567-e89b-42d3-a456-426614174002";
  const std::string stem = repeated('a', 64);
  const auto reservation = repository.reserveReplayFile(attempt, stem);
  expect(reservation.reservation.has_value(),
         "long-note mismatch reserves a replay path");
  if (!reservation.reservation.has_value()) {
    return;
  }

  auto playback = samplePlayback();
  playback.setup.longNoteMode = 2;
  replay::BeatorajaReplayCodec codec;
  std::string diagnostic;
  const auto encoded =
      codec.encodeChart(playback, 1'700'000'000'123LL, diagnostic);
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
                            .stageLongNoteModes = {2},
                            .course = false},
                           "long_note_mismatch")
          : replay::FinalizeOutcome{};
  expect(finalized.metadata.has_value(),
         "long-note mismatch installs a valid replay file");
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
  expect(repository.markReplayFileReservationFinalized(
             *reservation.reservation, *finalized.metadata, diagnostic),
         "long-note mismatch records finalized replay ownership");
  const auto staged =
      repository.stageCompletedChartAttempt(result, snapshot, reference, {});
  expect(staged.receipt.has_value(),
         "long-note mismatch stages its compact result");
  if (!staged.receipt.has_value()) {
    return;
  }

  const auto loaded =
      repository.loadChartReplayPlayback(staged.receipt->resultId);
  expect(loaded.status ==
             ChartReplayPlaybackReadOutcome::Status::IntegrityConflict,
         "chart replay rejects a long-note mode mismatch");
}

void testChartReplayRejectsStemThatOmitsDecodedLongNotePrefix() {
  TemporaryDirectory temporary;
  ReplayRepository repository(temporary.path() / "replay.db");
  expect(repository.EnsureSchema(), "wrong-stem replay schema is available");
  constexpr std::string_view attempt = "123e4567-e89b-42d3-a456-426614174003";
  const std::string wrongStem = repeated('a', 64);
  const auto reservation = repository.reserveReplayFile(attempt, wrongStem);
  expect(reservation.reservation.has_value(),
         "wrong-stem replay reserves a canonical but unrelated path");
  if (!reservation.reservation.has_value()) {
    return;
  }

  auto playback = samplePlayback();
  playback.setup.longNoteMode = 2;
  playback.setup.hasUndefinedLongNotes = true;
  replay::BeatorajaReplayCodec codec;
  std::string diagnostic;
  const auto encoded =
      codec.encodeChart(playback, 1'700'000'000'123LL, diagnostic);
  replay::ReplayFileStore store(temporary.path());
  const replay::ReplayPathIdentity path{
      .stem = reservation.reservation->stem,
      .historyIndex = reservation.reservation->historyIndex,
      .relativePath = reservation.reservation->relativePath,
  };
  const auto finalized =
      encoded.has_value() ? store.finalize(path, *encoded, codec,
                                           {.stageSha256 = {repeated('a', 64)},
                                            .stageLongNoteModes = {2},
                                            .course = false},
                                           "wrong_stem")
                          : replay::FinalizeOutcome{};
  expect(finalized.metadata.has_value(),
         "wrong-stem fixture installs a semantically valid replay file");
  if (!finalized.metadata.has_value()) {
    return;
  }

  auto result = validResult(std::string(attempt));
  result.score.longNoteMode = 2;
  result.resultFingerprint = result_persistence::resultFingerprint(result);
  const auto snapshot = snapshotFor(result);
  const ReplayFileReference reference{
      .stem = reservation.reservation->stem,
      .historyIndex = reservation.reservation->historyIndex,
      .relativePath = finalized.metadata->relativePath,
      .contentSha256 = finalized.metadata->sha256,
      .compressedSize = finalized.metadata->compressedSize,
      .codecVersion = finalized.metadata->codecVersion,
  };
  expect(repository.markReplayFileReservationFinalized(
             *reservation.reservation, *finalized.metadata, diagnostic),
         "wrong-stem fixture records finalized replay ownership");
  const auto staged =
      repository.stageCompletedChartAttempt(result, snapshot, reference, {});
  expect(staged.receipt.has_value(),
         "wrong-stem fixture stages its compact result");
  if (!staged.receipt.has_value()) {
    return;
  }

  const auto loaded =
      repository.loadChartReplayPlayback(staged.receipt->resultId);
  expect(loaded.status ==
             ChartReplayPlaybackReadOutcome::Status::IntegrityConflict,
         "chart replay rejects a filename stem missing its decoded LN prefix");
}

void testFreshCompactSchema() {
  TemporaryDirectory temporary;
  const auto databasePath = temporary.path() / "replay.db";
  ReplayRepository repository(databasePath);
  expect(repository.EnsureSchema(), "fresh compact replay schema is created");
  repository.Shutdown();

  Database database(databasePath);
  expect(scalarInteger(database.get(), "PRAGMA user_version") == 14,
         "fresh replay database uses schema version 14");
  const auto chartColumns = stringSet(
      database.get(), "SELECT name FROM pragma_table_info('chart_results')");
  const auto courseStageColumns = stringSet(
      database.get(),
      "SELECT name FROM pragma_table_info('course_result_stages')");
  expect(chartColumns.contains("adopted_gauge_type") &&
             courseStageColumns.contains("adopted_gauge_type"),
         "compact result tables store adopted gauge types");
  const auto reservationColumns = stringSet(
      database.get(),
      "SELECT name FROM pragma_table_info('replay_file_reservations')");
  expect(reservationColumns.contains("finalized_content_sha256") &&
             reservationColumns.contains("finalized_compressed_size"),
         "replay reservations store finalized ownership evidence");
  const auto courseColumns = stringSet(
      database.get(), "SELECT name FROM pragma_table_info('course_results')");
  expect(courseColumns.contains("entry_facts_json"),
         "course results retain presentation facts for every entry");
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

void testVersion11AdoptedGaugeMigration() {
  TemporaryDirectory temporary;
  const auto databasePath = temporary.path() / "replay.db";
  ReplayRepository repository(databasePath);
  expect(repository.EnsureSchema(), "schema-11 migration fixture is created");
  repository.Shutdown();

  {
    Database database(databasePath);
    expect(execute(database.get(),
                   "PRAGMA writable_schema=ON;") &&
               execute(database.get(),
                       "UPDATE sqlite_master SET sql=replace(replace(sql,"
                       "'adopted_gauge_type INTEGER NOT NULL,',''),"
                       "',CHECK(adopted_gauge_type BETWEEN 0 AND 5)','') "
                       "WHERE type='table' AND name='chart_results'") &&
               execute(database.get(),
                       "UPDATE sqlite_master SET sql=replace(replace(sql,"
                       "'adopted_gauge_type INTEGER NOT NULL,',''),"
                       "'CHECK(adopted_gauge_type BETWEEN 0 AND 5),','') "
                       "WHERE type='table' AND "
                       "name='course_result_stages'") &&
               execute(database.get(),
                       "UPDATE sqlite_master SET sql=replace(replace(replace("
                       "sql,'finalized_content_sha256 TEXT,',''),"
                       "'finalized_compressed_size INTEGER,',''),"
                       "'CHECK((finalized_content_sha256 IS NULL AND "
                       "finalized_compressed_size IS NULL) OR "
                       "(length(finalized_content_sha256)=64 AND "
                       "finalized_compressed_size>0)),','') WHERE "
                       "type='table' AND name='replay_file_reservations'") &&
               execute(database.get(),
                       "UPDATE sqlite_master SET sql=replace(sql,"
                       "',entry_facts_json TEXT NOT NULL','') WHERE "
                       "type='table' AND name='course_results'") &&
               execute(database.get(), "PRAGMA writable_schema=OFF") &&
               execute(database.get(), "PRAGMA user_version=11"),
           "schema-12 and schema-13-only columns are removed from the fixture");
  }

  expect(repository.EnsureSchema(),
         "schema 11 migrates adopted gauge columns atomically");
  repository.Shutdown();

  Database database(databasePath);
  expect(scalarInteger(database.get(), "PRAGMA user_version") == 14 &&
             stringSet(database.get(),
                       "SELECT name FROM pragma_table_info('chart_results')")
                 .contains("adopted_gauge_type") &&
             stringSet(database.get(),
                       "SELECT name FROM pragma_table_info("
                       "'course_result_stages')")
                 .contains("adopted_gauge_type"),
         "schema-11 migration commits version and both columns together");
}

void testProfileBindingCleansStaleReplayTemporaries() {
  TemporaryDirectory temporary;
  const auto startupRoot = temporary.path() / "startup-profile";
  const auto startupReplay = startupRoot / "replay";
  const auto staleStartup =
      startupReplay / ("." + repeated('a', 64) + ".brd.startup.tmp");
  const auto recentStartup =
      startupReplay / ("." + repeated('b', 64) + ".brd.recent.tmp");
  const auto malformedStartup =
      startupReplay / ("." + repeated('d', 64) + ".brd.bad.token.tmp");
  const auto unrelatedStartup = startupReplay / "keep.tmp";
  writeFile(staleStartup, "stale");
  writeFile(recentStartup, "recent");
  writeFile(malformedStartup, "malformed");
  writeFile(unrelatedStartup, "unrelated");
  std::error_code timeError;
  std::filesystem::last_write_time(
      staleStartup,
      std::filesystem::file_time_type::clock::now() - std::chrono::hours(2),
      timeError);
  expect(!timeError, "startup stale replay timestamp is set");
  std::filesystem::last_write_time(
      malformedStartup,
      std::filesystem::file_time_type::clock::now() - std::chrono::hours(2),
      timeError);
  expect(!timeError, "malformed replay temporary timestamp is set");

  ReplayRepository repository;
  repository.SetDatabasePath(startupRoot / "replays.db");
  expect(!std::filesystem::exists(staleStartup),
         "startup replay binding removes a stale private temporary");
  expect(std::filesystem::exists(recentStartup) &&
             std::filesystem::exists(malformedStartup) &&
             std::filesystem::exists(unrelatedStartup),
         "startup replay binding preserves recent and unowned files");

  const auto activatedRoot = temporary.path() / "activated-profile";
  const auto staleActivated = activatedRoot / "replay" /
                              ("." + repeated('c', 64) + ".brd.activation.tmp");
  writeFile(staleActivated, "stale");
  timeError.clear();
  std::filesystem::last_write_time(
      staleActivated,
      std::filesystem::file_time_type::clock::now() - std::chrono::hours(2),
      timeError);
  expect(!timeError, "activation stale replay timestamp is set");

  std::string bindError;
  expect(repository.BindDatabasePath(activatedRoot / "replays.db", bindError),
         "target replay database binds for activation cleanup");
  expect(!std::filesystem::exists(staleActivated),
         "profile activation removes a stale private replay temporary");
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

void testMalformedCurrentSchemaColumnFailsClosed() {
  TemporaryDirectory temporary;
  const auto databasePath = temporary.path() / "replay.db";
  ReplayRepository repository(databasePath);
  expect(repository.EnsureSchema(), "current validation fixture is created");
  repository.Shutdown();
  {
    Database database(databasePath);
    expect(execute(database.get(), "PRAGMA writable_schema=ON") &&
               execute(database.get(),
                       "UPDATE sqlite_master SET sql=replace(sql,"
                       "'chart_artist TEXT NOT NULL,','') WHERE type='table' "
                       "AND name='chart_results'") &&
               execute(database.get(), "PRAGMA writable_schema=OFF"),
           "current validation fixture removes one ordinary result column");
  }
  expect(!repository.EnsureSchema(),
         "same-version schema with a missing ordinary column fails closed");
}

void testMalformedCurrentSchemaDefaultFailsClosed() {
  struct CounterfeitDefaultCase {
    std::string_view replacement;
    std::string_view label;
  };
  constexpr std::array cases{
      CounterfeitDefaultCase{"created_at TEXT NOT NULL,", "missing"},
      CounterfeitDefaultCase{
          "created_at TEXT NOT NULL DEFAULT '1970-01-01 00:00:00',",
          "incorrect"},
  };
  for (const auto &counterfeit : cases) {
    TemporaryDirectory temporary;
    const auto databasePath = temporary.path() / "replay.db";
    ReplayRepository repository(databasePath);
    expect(repository.EnsureSchema(),
           "current default-validation fixture is created");
    repository.Shutdown();
    {
      Database database(databasePath);
      expect(
          execute(database.get(), "PRAGMA writable_schema=ON") &&
              execute(database.get(),
                      "UPDATE sqlite_master SET sql=replace(sql,"
                      "'created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,'," +
                          sqlQuote(counterfeit.replacement) +
                          ") WHERE type='table' AND "
                          "name='chart_results'") &&
              execute(database.get(), "PRAGMA writable_schema=OFF"),
          std::string("current schema installs a ") +
              std::string(counterfeit.label) + " created_at default");
    }
    expect(!repository.EnsureSchema(),
           std::string("same-version schema with a ") +
               std::string(counterfeit.label) +
               " created_at default fails closed");
  }
}

void testEquivalentReformattedColumnDefaultIsAccepted() {
  TemporaryDirectory temporary;
  const auto databasePath = temporary.path() / "replay.db";
  ReplayRepository repository(databasePath);
  expect(repository.EnsureSchema(),
         "reformatted default-validation fixture is created");
  repository.Shutdown();
  {
    Database database(databasePath);
    expect(execute(database.get(), "PRAGMA writable_schema=ON") &&
               execute(database.get(),
                       "UPDATE sqlite_master SET sql=replace(sql,"
                       "'created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,',"
                       "'created_at TEXT NOT NULL DEFAULT ( current_timestamp "
                       "),') WHERE type='table' AND name='chart_results'") &&
               execute(database.get(), "PRAGMA writable_schema=OFF"),
           "created_at default is reformatted without changing its meaning");
  }
  expect(repository.EnsureSchema(),
         "semantically equivalent created_at default formatting is accepted");
}

void testMalformedCurrentSchemaIndexDefinitionFailsClosed() {
  TemporaryDirectory temporary;
  const auto databasePath = temporary.path() / "replay.db";
  ReplayRepository repository(databasePath);
  expect(repository.EnsureSchema(),
         "current index-definition validation fixture is created");
  repository.Shutdown();
  {
    Database database(databasePath);
    expect(execute(database.get(),
                   "DROP INDEX idx_chart_results_sha256_played;") &&
               execute(database.get(),
                       "CREATE INDEX idx_chart_results_sha256_played ON "
                       "chart_results(chart_md5)"),
           "required index name is recreated with the wrong definition");
  }
  expect(!repository.EnsureSchema(),
         "same-version schema with a counterfeit index fails closed");
}

void testEquivalentReformattedNamedIndexIsAccepted() {
  TemporaryDirectory temporary;
  const auto databasePath = temporary.path() / "replay.db";
  ReplayRepository repository(databasePath);
  expect(repository.EnsureSchema(),
         "reformatted index validation fixture is created");
  repository.Shutdown();
  {
    Database database(databasePath);
    expect(execute(database.get(),
                   "DROP INDEX idx_chart_results_sha256_played;") &&
               execute(database.get(),
                       "create index \"idx_chart_results_sha256_played\" on "
                       "\"chart_results\" ( \"chart_sha256\" , "
                       "\"played_at_unix_ms\" desc , \"id\" desc )"),
           "required named index is recreated with equivalent quoted SQL");
  }
  expect(repository.EnsureSchema(),
         "semantically equivalent named-index formatting is accepted");
}

void testEquivalentReformattedCriticalChecksAreAccepted() {
  TemporaryDirectory temporary;
  const auto databasePath = temporary.path() / "replay.db";
  ReplayRepository repository(databasePath);
  expect(repository.EnsureSchema(),
         "reformatted CHECK validation fixture is created");
  repository.Shutdown();
  {
    Database database(databasePath);
    expect(execute(database.get(), "PRAGMA writable_schema=ON") &&
               execute(database.get(),
                       "UPDATE sqlite_master SET sql=replace(replace(sql,"
                       "'CHECK(local_result_ready IN (0,1))',"
                       "'check ( \"local_result_ready\" in ( 0 , 1 ) )'),"
                       "'CHECK(next_request_user_intent IN (0,1))',"
                       "'check ( \"next_request_user_intent\" in "
                       "( 0 , 1 ) )') WHERE type='table' AND "
                       "name='ir_outbox'") &&
               execute(database.get(), "PRAGMA writable_schema=OFF"),
           "critical CHECK constraints are reformatted without changing "
           "their meaning");
  }
  expect(repository.EnsureSchema(),
         "semantically equivalent CHECK formatting is accepted");
}

void testPartialCounterfeitUniqueConstraintFailsClosed() {
  TemporaryDirectory temporary;
  const auto databasePath = temporary.path() / "replay.db";
  ReplayRepository repository(databasePath);
  expect(repository.EnsureSchema(),
         "partial UNIQUE validation fixture is created");
  repository.Shutdown();
  {
    Database database(databasePath);
    expect(
        execute(database.get(),
                "ALTER TABLE replay_file_reservations RENAME TO "
                "replay_file_reservations_old;"
                "DROP INDEX idx_replay_reservations_stem_index;"
                "CREATE TABLE replay_file_reservations("
                "attempt_id TEXT PRIMARY KEY,stem TEXT NOT NULL,"
                "history_index INTEGER NOT NULL,"
                "relative_path TEXT UNIQUE NOT NULL,"
                "created_at_unix_ms INTEGER NOT NULL,"
                "finalized_content_sha256 TEXT,"
                "finalized_compressed_size INTEGER,"
                "CHECK(history_index>=0),"
                "CHECK((finalized_content_sha256 IS NULL AND "
                "finalized_compressed_size IS NULL) OR "
                "(length(finalized_content_sha256)=64 AND "
                "finalized_compressed_size>0)));"
                "CREATE UNIQUE INDEX counterfeit_reservation_stem_history ON "
                "replay_file_reservations(stem,history_index) "
                "WHERE history_index>0;"
                "CREATE INDEX idx_replay_reservations_stem_index ON "
                "replay_file_reservations(stem,history_index);"
                "DROP TABLE replay_file_reservations_old;"),
        "required composite UNIQUE is replaced by a partial index");
  }
  expect(!repository.EnsureSchema(),
         "partial counterfeit UNIQUE constraint fails closed");
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
  std::string ownershipDiagnostic;
  expect(repository.markReplayFileReservationFinalized(
             *reservation.reservation,
             {.relativePath = replayFile.relativePath,
              .sha256 = replayFile.contentSha256,
              .compressedSize = replayFile.compressedSize,
              .codecVersion = replayFile.codecVersion},
             ownershipDiagnostic),
         "compact staging records finalized replay ownership");

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
  bms_parser::ChartMeta lookup;
  lookup.MD5 = result.score.chartMd5;
  lookup.SHA256 = result.score.chartSha256;
  const auto summaries = repository.ListReplays(lookup, 0);
  expect(summaries.size() == 1 &&
             summaries.front().replayFileState ==
                 ReplaySummary::ReplayFileState::Unchecked,
         "record summaries defer replay-file inspection until selection");
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

void testProfileStartupReclaimsFilelessReservations() {
  TemporaryDirectory temporary;
  const auto databasePath = temporary.path() / "replay.db";
  const std::string stem = repeated('c', 64);
  ReplayRepository repository(databasePath);
  expect(repository.EnsureSchema(), "abandoned reservation schema is ready");
  const auto first = repository.reserveReplayFile(
      "123e4567-e89b-42d3-a456-426614174110", stem);
  const auto second = repository.reserveReplayFile(
      "123e4567-e89b-42d3-a456-426614174111", stem);
  expect(first.reservation && first.reservation->historyIndex == 0 &&
             second.reservation && second.reservation->historyIndex == 1,
         "fileless reservations consume the first two indexes before restart");
  repository.Shutdown();

  ReplayRepository reopened(databasePath);
  expect(reopened.EnsureSchema(),
         "profile restart opens abandoned reservation storage");
  const auto reclaimed = reopened.reserveReplayFile(
      "123e4567-e89b-42d3-a456-426614174112", stem);
  expect(reclaimed.status == ReservationOutcome::Status::Reserved &&
             reclaimed.reservation &&
             reclaimed.reservation->historyIndex == 0,
         "profile startup reclaims fileless reservation indexes");
}

void testReservationSkipsExistingUntrackedReplay() {
  TemporaryDirectory temporary;
  const auto databasePath = temporary.path() / "replay.db";
  const std::string stem = repeated('b', 64);
  ReplayRepository repository(databasePath);
  expect(repository.EnsureSchema(),
         "collision-safe reservation schema is ready");
  std::string pathDiagnostic;
  const auto occupied = replay::pathForStem(stem, 0, pathDiagnostic);
  expect(occupied.has_value(), "stock slot zero path is valid");
  if (!occupied) {
    return;
  }
  writeFile(temporary.path() / occupied->relativePath, "external replay");

  const auto reserved = repository.reserveReplayFile(
      "123e4567-e89b-42d3-a456-426614174100", stem);
  expect(reserved.reservation && reserved.reservation->historyIndex == 1 &&
             std::filesystem::exists(temporary.path() /
                                     occupied->relativePath),
         "allocator skips an existing replay that is absent from SQLite");
}

void testReservationSkipsCanonicalCrossStemPathAlias() {
  TemporaryDirectory temporary;
  const auto databasePath = temporary.path() / "replay.db";
  ReplayRepository repository(databasePath);
  expect(repository.EnsureSchema(),
         "cross-stem collision reservation schema is ready");

  replay::CoursePathInput unconstrainedInput{
      .stageSha256 = {repeated('a', 64)},
  };
  replay::CoursePathInput constrainedInput = unconstrainedInput;
  constrainedInput.beatorajaConstraintIds = {12};
  std::string diagnostic;
  const auto unconstrainedStem =
      replay::courseStem(unconstrainedInput, diagnostic);
  const auto constrainedStem = replay::courseStem(constrainedInput, diagnostic);
  expect(unconstrainedStem == "aaaaaaaaaa" &&
             constrainedStem == "aaaaaaaaaa_12",
         "course fixtures produce the canonical history/constraint alias");
  if (!unconstrainedStem || !constrainedStem) {
    return;
  }

  std::optional<ReplayFileReservation> twelfthHistory;
  for (int index = 0; index <= 12; ++index) {
    const std::string suffix =
        index < 10 ? "0" + std::to_string(index) : std::to_string(index);
    const auto reserved = repository.reserveReplayFile(
        "123e4567-e89b-42d3-a456-4266141742" + suffix, *unconstrainedStem);
    expect(reserved.status == ReservationOutcome::Status::Reserved &&
               reserved.reservation &&
               reserved.reservation->historyIndex == index,
           "unconstrained course allocates the expected history path");
    if (index == 12) {
      twelfthHistory = reserved.reservation;
    }
  }

  const auto constrained = repository.reserveReplayFile(
      "123e4567-e89b-42d3-a456-426614174300", *constrainedStem);
  expect(twelfthHistory.has_value(),
         "unconstrained course owns its twelfth history path");
  expect(constrained.status == ReservationOutcome::Status::Reserved,
         "aliased constrained course reservation succeeds");
  expect(constrained.reservation && constrained.reservation->historyIndex == 1,
         "aliased constrained course advances to history one");
  expect(twelfthHistory && constrained.reservation &&
             constrained.reservation->relativePath !=
                 twelfthHistory->relativePath,
         "allocator skips a SQLite-owned path aliased by another canonical "
         "course stem");
}

void testProfileStartupPreservesUnownedReplayCollision() {
  TemporaryDirectory temporary;
  const auto databasePath = temporary.path() / "replay.db";
  const std::string stem = repeated('d', 64);
  constexpr std::string_view installedAttempt =
      "123e4567-e89b-42d3-a456-426614174120";
  ReplayRepository repository(databasePath);
  expect(repository.EnsureSchema(), "installed reservation schema is ready");
  const auto installed =
      repository.reserveReplayFile(installedAttempt, stem);
  const auto abandoned = repository.reserveReplayFile(
      "123e4567-e89b-42d3-a456-426614174121", stem);
  expect(installed.reservation && installed.reservation->historyIndex == 0 &&
             abandoned.reservation && abandoned.reservation->historyIndex == 1,
         "installed and abandoned reservations occupy consecutive indexes");
  if (!installed.reservation) {
    return;
  }
  writeFile(temporary.path() / installed.reservation->relativePath,
            "installed replay bytes");
  repository.Shutdown();

  ReplayRepository reopened(databasePath);
  expect(reopened.EnsureSchema(),
         "profile restart opens installed reservation storage");
  expect(std::filesystem::exists(temporary.path() /
                                 installed.reservation->relativePath),
         "startup preserves a file not proven to belong to the reservation");
  const auto reclaimed = reopened.reserveReplayFile(installedAttempt, stem);
  expect(reclaimed.status == ReservationOutcome::Status::Reserved &&
             reclaimed.reservation &&
             reclaimed.reservation->historyIndex == 1,
         "startup drops the reservation but skips the preserved occupied slot");
  const auto afterRecovery = reopened.reserveReplayFile(
      "123e4567-e89b-42d3-a456-426614174122", stem);
  expect(afterRecovery.status == ReservationOutcome::Status::Reserved &&
             afterRecovery.reservation &&
             afterRecovery.reservation->historyIndex == 2,
         "reclaimed orphan slots preserve monotonic live reservations");
}

void testProfileStartupReclaimsOwnedFinalReplay() {
  TemporaryDirectory temporary;
  const auto databasePath = temporary.path() / "replay.db";
  const std::string stem = repeated('d', 64);
  constexpr std::string_view attempt =
      "123e4567-e89b-42d3-a456-426614174125";
  ReplayRepository repository(databasePath);
  expect(repository.EnsureSchema(), "owned reservation schema is ready");
  const auto reserved = repository.reserveReplayFile(attempt, stem);
  expect(reserved.reservation.has_value(), "owned replay path is reserved");
  if (!reserved.reservation) {
    return;
  }
  const auto finalPath = temporary.path() / reserved.reservation->relativePath;
  writeFile(finalPath, "finalized owned replay");
  const auto metadata =
      metadataForFile(temporary.path(), reserved.reservation->relativePath);
  std::string diagnostic;
  expect(repository.markReplayFileReservationFinalized(
             *reserved.reservation, metadata, diagnostic),
         "finalized ownership metadata is recorded");
  repository.Shutdown();

  ReplayRepository reopened(databasePath);
  expect(reopened.EnsureSchema(), "owned orphan recovery opens storage");
  expect(!std::filesystem::exists(finalPath),
         "startup removes an orphan only when its content proves ownership");
  const auto reclaimed = reopened.reserveReplayFile(attempt, stem);
  expect(reclaimed.reservation && reclaimed.reservation->historyIndex == 0,
         "owned orphan cleanup makes its slot allocatable again");
}

void testExplicitlyDiscardsUndurableFinalReplay() {
  TemporaryDirectory temporary;
  const auto databasePath = temporary.path() / "replay.db";
  const std::string stem = repeated('e', 64);
  constexpr std::string_view attempt =
      "123e4567-e89b-42d3-a456-426614174130";
  ReplayRepository repository(databasePath);
  expect(repository.EnsureSchema(), "undurable replay schema is ready");
  const auto reserved = repository.reserveReplayFile(attempt, stem);
  expect(reserved.reservation.has_value(),
         "undurable replay reserves its final path");
  if (!reserved.reservation) {
    return;
  }
  const auto finalPath = temporary.path() / reserved.reservation->relativePath;
  writeFile(finalPath, "finalized but unstaged replay");
  const auto metadata =
      metadataForFile(temporary.path(), reserved.reservation->relativePath);

  std::string diagnostic;
  expect(repository.markReplayFileReservationFinalized(
             *reserved.reservation, metadata, diagnostic) &&
             repository.discardUndurableReplay(attempt, stem, diagnostic) &&
             !std::filesystem::exists(finalPath),
         "continue cleanup removes the orphan file and reservation");
  const auto reused = repository.reserveReplayFile(
      "123e4567-e89b-42d3-a456-426614174131", stem);
  expect(reused.reservation && reused.reservation->historyIndex == 1,
         "explicit orphan cleanup keeps the live-session high-water mark");
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
  testVersion11AdoptedGaugeMigration();
  testProfileBindingCleansStaleReplayTemporaries();
  testMalformedVersion11FailsClosed();
  testMalformedCurrentSchemaColumnFailsClosed();
  testMalformedCurrentSchemaDefaultFailsClosed();
  testEquivalentReformattedColumnDefaultIsAccepted();
  testMalformedCurrentSchemaIndexDefinitionFailsClosed();
  testEquivalentReformattedNamedIndexIsAccepted();
  testEquivalentReformattedCriticalChecksAreAccepted();
  testPartialCounterfeitUniqueConstraintFailsClosed();
  testCompactStageAndIndependentReads();
  testReplayFileReadIsIndependentFromResultAndIr();
  testChartReplayRejectsLongNoteModeMismatch();
  testChartReplayRejectsStemThatOmitsDecodedLongNotePrefix();
  testReservationIntegrityAndMonotonicity();
  testReservationSkipsExistingUntrackedReplay();
  testReservationSkipsCanonicalCrossStemPathAlias();
  testProfileStartupReclaimsFilelessReservations();
  testProfileStartupPreservesUnownedReplayCollision();
  testProfileStartupReclaimsOwnedFinalReplay();
  testExplicitlyDiscardsUndurableFinalReplay();
  testMalformedVersion10FailsClosed();
  if (failures != 0) {
    std::cerr << failures << " replay repository v11 test(s) failed\n";
    return 1;
  }
  std::cout << "Replay repository v11 tests passed\n";
  return 0;
}
