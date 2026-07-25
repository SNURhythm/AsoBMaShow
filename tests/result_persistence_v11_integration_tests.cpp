#include "ResultPersistenceCoordinator.h"

#include "ir/IrSubmissionSnapshot.h"
#include "replay/BeatorajaReplayCodec.h"
#include "replay/ReplayFileStore.h"
#include "sqlite3.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace result_persistence;

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
  explicit TemporaryDirectory(std::string_view label) {
    static std::atomic<unsigned long long> sequence{0};
    const auto tick =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("asobmashow-result-v11-" + std::string(label) + "-" +
             std::to_string(tick) + "-" +
             std::to_string(sequence.fetch_add(1)));
    std::filesystem::create_directories(path_);
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

std::int64_t scalar(const std::filesystem::path &databasePath,
                    std::string_view query) {
  sqlite3 *database = nullptr;
  if (sqlite3_open_v2(databasePath.string().c_str(), &database,
                      SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
    sqlite3_close(database);
    return -1;
  }
  sqlite3_stmt *statement = nullptr;
  if (sqlite3_prepare_v2(database, std::string(query).c_str(), -1, &statement,
                         nullptr) != SQLITE_OK ||
      sqlite3_step(statement) != SQLITE_ROW) {
    sqlite3_finalize(statement);
    sqlite3_close(database);
    return -1;
  }
  const std::int64_t value = sqlite3_column_int64(statement, 0);
  const bool complete = sqlite3_step(statement) == SQLITE_DONE;
  sqlite3_finalize(statement);
  sqlite3_close(database);
  return complete ? value : -1;
}

CompletedChartAttempt validAttempt(std::string attemptId, int salt = 0) {
  CompletedChartAttempt attempt;
  auto &result = attempt.result;
  result.attemptId = std::move(attemptId);
  result.score.chartPath = "sample/song-" + std::to_string(salt) + ".bms";
  result.score.chartMd5 = repeated(salt == 0 ? 'b' : 'd', 32);
  result.score.chartSha256 = repeated(salt == 0 ? 'a' : 'e', 64);
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
  ScoreProvenanceBuildInput provenance;
  provenance.chartMeta.MD5 = result.score.chartMd5;
  provenance.chartMeta.SHA256 = result.score.chartSha256;
  provenance.chartMeta.Rank = 2;
  provenance.chartMeta.TotalNotes = 5;
  provenance.chartMeta.HasTotal = true;
  provenance.chartMeta.Total = 100.0;
  provenance.longNoteMode = result.score.longNoteMode;
  provenance.judgeRankSource = JudgeRankSource::Chart;
  provenance.sourceJudgeRank = provenance.chartMeta.Rank;
  provenance.effectiveJudgeWindows = {
      {Bad, {-330'000, 420'000}}, {PGreat, {-10'000, 10'000}},
      {Great, {-30'000, 30'000}}, {Good, {-75'000, 75'000}},
      {Kpoor, {-500'000, 150'000}},
  };
  provenance.totalNotes = provenance.chartMeta.TotalNotes;
  provenance.authoredGaugeTotal = provenance.chartMeta.Total;
  provenance.effectiveGaugeTotal = provenance.chartMeta.Total;
  provenance.inputDevices = {InputDeviceCategory::Keyboard};
  provenance.ruleset = RulesetDescriptor::Current();
  result.score.provenance = makeScoreProvenance(provenance);
  result.keyMode = 7;
  result.adoptedGaugeHistory = {20.0F, 48.5F, 82.5F};
  result.judgementTiming = ChartJudgementTiming{};
  result.judgementTiming->byJudgement[PGreat] = {.fast = 1, .slow = 1};
  result.judgementTiming->byJudgement[Great] = {.fast = 1, .slow = 0};
  result.playedAtUnixMillis = 1'700'000'000'123LL + salt;
  result.resultFingerprint = resultFingerprint(result);

  auto &playback = attempt.replay;
  playback.setup.chartMd5 = result.score.chartMd5;
  playback.setup.chartSha256 = result.score.chartSha256;
  playback.setup.keyMode = result.keyMode;
  playback.setup.longNoteMode = result.score.longNoteMode;
  playback.setup.hasUndefinedLongNotes = false;
  playback.setup.playOption = "NORMAL";
  playback.setup.playOption2 = "NORMAL";
  playback.setup.playbackRulesetId = "asobmashow";
  playback.setup.playbackRulesetRevision = 11;
  playback.setup.initialLaneCoverPercent = 37;
  playback.setup.laneCoverEnabled = true;
  playback.input = {
      {.songTimeMicros = 1000,
       .control = {.kind = replay::LogicalControlKind::Lane,
                   .player = 1,
                   .lane = 0},
       .pressed = true},
      {.songTimeMicros = 1500,
       .control = {.kind = replay::LogicalControlKind::Lane,
                   .player = 1,
                   .lane = 0},
       .pressed = false},
      {.songTimeMicros = 2000,
       .control = {.kind = replay::LogicalControlKind::Start,
                   .player = 1,
                   .lane = -1},
       .pressed = true},
  };
  playback.touchSamples = {{.action = replay::ReplayTouchAction::Down,
                            .fingerId = 7,
                            .songTimeMicros = 2100,
                            .x = 0.25F,
                            .y = 0.75F}};
  playback.laneCoverEvents = {{.songTimeMicros = 2200,
                               .noteStartPositionPercent = 41,
                               .resetVisibleTimeReference = true}};

  std::string diagnostic;
  const auto snapshot = ir::captureIrSubmissionSnapshot(result, diagnostic);
  expect(snapshot.has_value(), "integration fixture snapshot captures");
  if (snapshot) {
    attempt.irSnapshot = *snapshot;
  }
  return attempt;
}

ir::IrOutboxDraft draftFor(const CompletedChartAttempt &attempt) {
  return {
      .providerId = "tachi",
      .attemptId = *attempt.result.attemptId,
      .chartMd5 = attempt.result.score.chartMd5,
      .chartSha256 = attempt.result.score.chartSha256,
      .payloadJson = R"({"score":7})",
      .rulesetProof =
          {
              .rulesetId = "test-rules",
              .rulesetRevision = 1,
              .validationFingerprint = repeated('f', 64),
          },
      .createdAtUnixMillis = attempt.result.playedAtUnixMillis,
  };
}

struct Environment {
  explicit Environment(const std::filesystem::path &root,
                       replay::ReplayFileStoreFaults faults = {})
      : replayDatabase(root / "replay.db"), scoreDatabase(root / "score.db"),
        replayRepository(replayDatabase), scoreRepository(scoreDatabase),
        fileStore(root, std::move(faults)),
        coordinator(scoreRepository, replayRepository, fileStore, codec) {
    expect(replayRepository.EnsureSchema(), "replay schema initializes");
    expect(scoreRepository.EnsureSchema(), "score schema initializes");
  }

  std::filesystem::path replayDatabase;
  std::filesystem::path scoreDatabase;
  ReplayRepository replayRepository;
  ScoreRepository scoreRepository;
  replay::BeatorajaReplayCodec codec;
  replay::ReplayFileStore fileStore;
  Coordinator coordinator;
};

ReplayFileReference finalizedReference(Environment &environment,
                                       const CompletedChartAttempt &attempt) {
  const std::string &attemptId = *attempt.result.attemptId;
  const auto reserved = environment.replayRepository.reserveReplayFile(
      attemptId, attempt.replay.setup.chartSha256);
  expect(reserved.reservation.has_value(), "manual recovery file reserves");
  if (!reserved.reservation) {
    return {};
  }
  std::string diagnostic;
  const auto encoded = environment.codec.encodeChart(
      attempt.replay, attempt.result.playedAtUnixMillis, diagnostic);
  expect(encoded.has_value(), "manual recovery replay encodes");
  if (!encoded) {
    return {};
  }
  const replay::ReplayPathIdentity identity{
      .stem = reserved.reservation->stem,
      .historyIndex = reserved.reservation->historyIndex,
      .relativePath = reserved.reservation->relativePath,
  };
  const auto finalized = environment.fileStore.finalize(
      identity, *encoded, environment.codec,
      {.stageSha256 = {attempt.replay.setup.chartSha256}, .course = false},
      attemptId);
  expect(finalized.metadata.has_value(), "manual recovery replay finalizes");
  if (!finalized.metadata) {
    return {};
  }
  return {
      .stem = identity.stem,
      .historyIndex = identity.historyIndex,
      .relativePath = finalized.metadata->relativePath,
      .contentSha256 = finalized.metadata->sha256,
      .compressedSize = finalized.metadata->compressedSize,
      .codecVersion = finalized.metadata->codecVersion,
  };
}

void testCompleteFileAndDatabasePipeline() {
  TemporaryDirectory temporary("complete");
  Environment environment(temporary.path());
  auto attempt = validAttempt("123e4567-e89b-42d3-a456-426614174000");
  const auto draft = draftFor(attempt);
  const auto saved = environment.coordinator.persist(attempt, {&draft, 1});
  if (!saved.saved()) {
    std::cerr << "complete pipeline state=" << static_cast<int>(saved.state)
              << " diagnostic=" << saved.diagnostic << '\n';
  }
  expect(saved.saved() && saved.receipt && saved.receipt->resultId > 0,
         "real file and SQLite pipeline saves");
  if (!saved.receipt) {
    return;
  }

  const auto loaded = environment.replayRepository.loadChartResult(
      saved.receipt->resultId);
  expect(loaded.status == ResultReadOutcome::Status::Loaded && loaded.record &&
             loaded.record->replayFile,
         "compact result loads with one replay-file reference");
  if (!loaded.record || !loaded.record->replayFile) {
    return;
  }
  const auto &reference = *loaded.record->replayFile;
  expect(reference.relativePath ==
             std::filesystem::path("replay") /
                 (attempt.result.score.chartSha256 + ".brd") &&
             std::filesystem::is_regular_file(temporary.path() /
                                              reference.relativePath),
         "first play uses Beatoraja's replay/<sha256>.brd layout");
  const replay::ReplayFileMetadata metadata{
      .relativePath = reference.relativePath,
      .sha256 = reference.contentSha256,
      .compressedSize = reference.compressedSize,
      .codecVersion = reference.codecVersion,
  };
  const auto decoded = environment.fileStore.load(metadata, environment.codec);
  expect(decoded.chart &&
             decoded.chart->setup.chartSha256 ==
                 attempt.replay.setup.chartSha256 &&
             decoded.chart->input == attempt.replay.input &&
             decoded.chart->touchSamples == attempt.replay.touchSamples &&
             decoded.chart->laneCoverEvents == attempt.replay.laneCoverEvents,
         "persisted .brd round-trips stock and Aso extension events");
  const auto snapshot = environment.replayRepository.loadIrSubmissionSnapshot(
      *attempt.result.attemptId);
  expect(snapshot.snapshot == attempt.irSnapshot,
         "provider-neutral IR snapshot is independent of replay bytes");
  const auto outbox = environment.replayRepository.LoadIrOutbox(
      draft.providerId, draft.attemptId);
  expect(outbox.status == ir::IrOutboxReadStatus::Found && outbox.entry &&
             outbox.entry->localResultReady,
         "automatic IR draft activates only after local score projection");

  expect(scalar(environment.replayDatabase,
                "SELECT count(*) FROM chart_results") == 1 &&
             scalar(environment.replayDatabase,
                    "SELECT count(*) FROM replay_files") == 1 &&
             scalar(environment.replayDatabase,
                    "SELECT count(*) FROM pending_chart_score_writes") == 0 &&
             scalar(environment.scoreDatabase, "SELECT count(*) FROM scores") ==
                 1,
         "successful save leaves one compact result, one file reference, and "
         "one projected score");
  expect(scalar(environment.replayDatabase,
                "SELECT count(*) FROM sqlite_master WHERE type='table' AND "
                "name IN ('replay_events','replay_touch_samples',"
                "'replay_lane_cover_events')") == 0,
         "fresh saves have no per-event SQLite tables");

  const auto retried = environment.coordinator.persist(attempt, {&draft, 1});
  expect(retried.saved() && retried.receipt &&
             retried.receipt->resultId == saved.receipt->resultId &&
             scalar(environment.replayDatabase,
                    "SELECT count(*) FROM replay_files") == 1,
         "retry is idempotent across reservation, file, result, and score");

  std::string removalDiagnostic;
  expect(environment.fileStore.remove(metadata, removalDiagnostic),
         "user can remove the standalone replay file");
  const auto afterDelete = environment.replayRepository.loadChartResult(
      saved.receipt->resultId);
  const auto snapshotAfterDelete =
      environment.replayRepository.loadIrSubmissionSnapshot(
          *attempt.result.attemptId);
  expect(afterDelete.status == ResultReadOutcome::Status::Loaded &&
             snapshotAfterDelete.snapshot == attempt.irSnapshot &&
             scalar(environment.scoreDatabase, "SELECT count(*) FROM scores") ==
                 1,
         "deleting .brd does not delete result, score, provenance, or IR "
         "snapshot");
}

void testFilesystemFailuresNeverStageDatabaseRows() {
  for (std::string_view fault : {"write", "file-sync", "close", "rename",
                                 "directory-sync", "read-back", "decode",
                                 "hash"}) {
    TemporaryDirectory temporary(std::string("fault-") + std::string(fault));
    bool enabled = true;
    Environment environment(
        temporary.path(),
        {.failAt = [&](std::string_view point) {
          return enabled && point == fault;
        }});
    const auto attempt =
        validAttempt("123e4567-e89b-42d3-a456-426614174001");
    const auto failed = environment.coordinator.persist(attempt);
    expect(failed.state == SaveState::UnfinalizedReplay &&
               scalar(environment.replayDatabase,
                      "SELECT count(*) FROM chart_results") == 0 &&
               scalar(environment.replayDatabase,
                      "SELECT count(*) FROM replay_files") == 0,
           std::string("fault before verified finalization stages no DB rows: ") +
               std::string(fault));
    enabled = false;
    const auto retried = environment.coordinator.persist(attempt);
    if (!retried.saved()) {
      std::cerr << "fault retry " << fault
                << " state=" << static_cast<int>(retried.state)
                << " diagnostic=" << retried.diagnostic << '\n';
    }
    expect(retried.saved(),
           std::string("filesystem fault retry reuses reservation/file: ") +
               std::string(fault));
  }
}

void testCrashAfterCompactStageRecoversWithoutReplayReconstruction() {
  TemporaryDirectory temporary("recovery");
  Environment environment(temporary.path());
  const auto attempt =
      validAttempt("123e4567-e89b-42d3-a456-426614174002", 1);
  const auto replayFile = finalizedReference(environment, attempt);
  const auto staged = environment.replayRepository.stageCompletedChartAttempt(
      attempt.result, attempt.irSnapshot, replayFile, {});
  expect(staged.status == StageStatus::Staged && staged.receipt &&
             staged.receipt->scorePending,
         "crash fixture stops after compact staging");
  expect(scalar(environment.replayDatabase,
                "SELECT count(*) FROM pending_chart_score_writes") == 1 &&
             scalar(environment.scoreDatabase, "SELECT count(*) FROM scores") ==
                 0,
         "crash fixture has a durable pending projection");

  const auto recovered = environment.coordinator.recoverAll();
  if (recovered.saved != 1) {
    std::cerr << "recovery diagnostic=" << recovered.diagnostic << '\n';
  }
  expect(recovered.attempted == 1 && recovered.saved == 1 &&
             recovered.pending == 0 && recovered.conflicts == 0 &&
             scalar(environment.replayDatabase,
                    "SELECT count(*) FROM pending_chart_score_writes") == 0 &&
             scalar(environment.scoreDatabase, "SELECT count(*) FROM scores") ==
                 1,
         "startup recovery projects compact facts without replay events");
}

} // namespace

int main() {
  testCompleteFileAndDatabasePipeline();
  testFilesystemFailuresNeverStageDatabaseRows();
  testCrashAfterCompactStageRecoversWithoutReplayReconstruction();
  if (failures != 0) {
    std::cerr << failures << " result persistence integration test(s) failed\n";
    return 1;
  }
  std::cout << "Result persistence v11 integration tests passed\n";
  return 0;
}
