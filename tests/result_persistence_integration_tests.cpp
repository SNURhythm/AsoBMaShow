#include "../src/ResultPersistenceCoordinator.h"

#include "../src/FileChecksum.h"
#include "../src/ProfileDatabaseActivity.h"
#include "../src/repositories/SqliteRAII.h"
#include "../src/Utils.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace result_persistence;

class TemporaryDirectory {
public:
  explicit TemporaryDirectory(std::string_view label) {
    static std::atomic<unsigned long long> sequence{0};
    const auto tick =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ =
        std::filesystem::temp_directory_path() /
        ("asobmashow-result-coordinator-" + std::string(label) + "-" +
         std::to_string(tick) + "-" + std::to_string(sequence.fetch_add(1)));
    std::filesystem::create_directories(path_);
  }

  ~TemporaryDirectory() { std::filesystem::remove_all(path_); }

  TemporaryDirectory(const TemporaryDirectory &) = delete;
  TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;

  [[nodiscard]] const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

SqliteConnectionHandle openDatabase(const std::filesystem::path &path) {
  sqlite3 *database = nullptr;
  assert(sqlite3_open(path.string().c_str(), &database) == SQLITE_OK);
  return SqliteConnectionHandle(database);
}

int queryInt(sqlite3 *database, const std::string &sql) {
  SqliteStatementHandle statement;
  const int prepareResult = prepareSqliteStatement(database, sql, statement);
  if (prepareResult != SQLITE_OK) {
    std::cerr << "query prepare failed: " << sqlite3_errmsg(database) << " | "
              << sql << '\n';
  }
  assert(prepareResult == SQLITE_OK);
  assert(sqlite3_step(statement.get()) == SQLITE_ROW);
  return sqlite3_column_int(statement.get(), 0);
}

void execOrAbort(sqlite3 *database, const std::string &sql) {
  char *message = nullptr;
  const int result = sqlite3_exec(database, sql.c_str(), nullptr, nullptr,
                                  &message);
  if (result != SQLITE_OK) {
    std::cerr << "SQL failed: " << (message ? message : "unknown error")
              << " | " << sql << '\n';
  }
  sqlite3_free(message);
  assert(result == SQLITE_OK);
}

std::string quoteSql(std::string_view value) {
  std::string quoted{"'"};
  for (const char character : value) {
    quoted += character;
    if (character == '\'') {
      quoted += '\'';
    }
  }
  quoted += '\'';
  return quoted;
}

ScoreProvenance sampleProvenance(const std::string &hash) {
  ScoreProvenance value;
  value.ruleset = RulesetDescriptor::Current();
  value.stages = {{
      .chartMd5 = "md5-" + hash,
      .chartSha256 = "sha-" + hash,
      .longNoteMode = 2,
      .judgeRankSource = JudgeRankSource::Chart,
      .sourceJudgeRank = 2,
      .effectiveJudgeWindows = {{PGreat, -10000, 10000},
                                {Great, -30000, 30000}},
  }};
  value.gaugeType = GaugeType::Hard;
  value.player1 = {.option = "RANDOM", .seed = 1234};
  value.inputDevices = {InputDeviceCategory::Keyboard};
  value.eligibility = ScoreEligibility::Verified;
  return value;
}

ReplayData sampleReplay(const std::filesystem::path &root,
                        const std::string &hash) {
  ReplayData replay;
  replay.chartMeta.BmsPath = root / "BMS" / (hash + ".bms");
  replay.chartMeta.SHA256 = file_checksum::sha256(hash);
  replay.chartMeta.Title = "Title " + hash;
  replay.chartMeta.Artist = "Artist";
  replay.chartMeta.TotalNotes = 50;
  replay.chartMeta.TotalLongNotes = 1;
  replay.chartMeta.LnMode = 2;
  replay.initialGaugeType = GaugeType::Hard;
  replay.gaugeAutoShift = GaugeAutoShiftMode::BestClear;
  replay.finalScore = 91;
  replay.maxCombo = 45;
  replay.finalGauge = 82.5f;
  replay.clearType = kClearTypeHardClearRank;
  replay.playOption = "RANDOM";
  replay.playOptionSeed = 1234;
  replay.events.push_back({.action = ReplayEventAction::Press,
                           .lane = 3,
                           .noteTimeMicros = 100000,
                           .songTimeMicros = 100100,
                           .judgeTimeMicros = 100050,
                           .judgement = PGreat,
                           .diffMicros = -50,
                           .gauge = 82.5f,
                           .gaugeType = GaugeType::Hard,
                           .combo = 1,
                           .score = 2});
  replay.provenance = sampleProvenance(hash);
  return replay;
}

std::string attemptId(int suffix) {
  assert(suffix >= 0 && suffix <= 4095);
  std::string value = "00000000-0000-4000-8000-000000000000";
  constexpr std::string_view digits = "0123456789abcdef";
  value[value.size() - 3] =
      digits[static_cast<std::size_t>((suffix / 256) % 16)];
  value[value.size() - 2] =
      digits[static_cast<std::size_t>((suffix / 16) % 16)];
  value.back() = digits[static_cast<std::size_t>(suffix % 16)];
  return value;
}

ChartResultAttempt sampleAttempt(const std::filesystem::path &root,
                                 const std::string &hash, int suffix) {
  ReplayData replay = sampleReplay(root, hash);
  replay.provenance.stages.front().chartMd5 = replay.chartMeta.MD5;
  replay.provenance.stages.front().chartSha256 = replay.chartMeta.SHA256;
  replay.provenance.stages.front().longNoteMode = replay.chartMeta.LnMode;
  replay.touchSamples.push_back({.action = ReplayTouchAction::Move,
                                 .fingerId = 17,
                                 .songTimeMicros = 100200,
                                 .x = 0.25f,
                                 .y = 0.75f});
  replay.laneCoverEvents.push_back({.songTimeMicros = 100300,
                                    .noteStartPositionPercent = 31,
                                    .resetVisibleTimeReference = true});

  ChartScoreWrite score{
      .chartPath = Utils::GetStoragePathUtf8RelativeToDocuments(
          replay.chartMeta.BmsPath, "BMS/"),
      .chartMd5 = replay.chartMeta.MD5,
      .chartSha256 = replay.chartMeta.SHA256,
      .chartTitle = replay.chartMeta.Title,
      .chartArtist = replay.chartMeta.Artist,
      .longNoteMode = replay.chartMeta.LnMode,
      .score = replay.finalScore,
      .maxScore = replay.chartMeta.TotalNotes * 2,
      .maxCombo = replay.maxCombo,
      .comboBreak = 5,
      .pGreat = 40,
      .great = 11,
      .good = 2,
      .bad = 1,
      .poor = 3,
      .kPoor = 4,
      .fast = 7,
      .slow = 8,
      .finalGauge = replay.finalGauge,
      .clearType = replay.clearType,
      .provenance = replay.provenance,
  };
  return {.attemptId = attemptId(suffix),
          .replay = replay,
          .score = score,
          .payloadFingerprint = payloadFingerprint(replay, score)};
}

ir::IrOutboxDraft automaticIrDraft(const ChartResultAttempt &attempt,
                                   std::int64_t createdAtUnixMillis) {
  return {
      .providerId = "tachi",
      .attemptId = attempt.attemptId,
      .chartMd5 = attempt.score.chartMd5,
      .chartSha256 = attempt.score.chartSha256,
      .payloadJson = R"({"score":123})",
      .createdAtUnixMillis = createdAtUnixMillis,
  };
}

void assertDatabaseCounts(const std::filesystem::path &replayPath,
                          const std::filesystem::path &scorePath,
                          int replayCount, int scoreCount, int pendingCount) {
  auto replayDatabase = openDatabase(replayPath);
  auto scoreDatabase = openDatabase(scorePath);
  const int actualReplayCount =
      queryInt(replayDatabase.get(), "SELECT COUNT(*) FROM replays");
  const int actualPendingCount = queryInt(
      replayDatabase.get(), "SELECT COUNT(*) FROM pending_chart_score_writes");
  const int actualScoreCount =
      queryInt(scoreDatabase.get(), "SELECT COUNT(*) FROM scores");
  if (actualReplayCount != replayCount || actualScoreCount != scoreCount ||
      actualPendingCount != pendingCount) {
    std::cerr << "database counts: replay=" << actualReplayCount
              << " score=" << actualScoreCount
              << " pending=" << actualPendingCount << " (expected "
              << replayCount << ", " << scoreCount << ", " << pendingCount
              << ")\n";
  }
  assert(actualReplayCount == replayCount);
  assert(actualScoreCount == scoreCount);
  assert(actualPendingCount == pendingCount);
}

void testPersistCreatesOneReplayOneScoreNoPending() {
  TemporaryDirectory temporary("persist");
  const auto replayPath = temporary.path() / "replay.db";
  const auto scorePath = temporary.path() / "score.db";
  ReplayRepository replay(replayPath);
  ScoreRepository score(scorePath);
  Coordinator coordinator(score, replay);
  const ChartResultAttempt fixed =
      sampleAttempt(temporary.path(), "fixed-persist", 1);

  const SaveOutcome first = coordinator.persist(fixed);
  const SaveOutcome retry = coordinator.persist(fixed);

  assert(first.state == SaveState::Saved);
  assert(first.saved() && first.durable());
  assert(first.receipt.has_value());
  assert(retry.state == SaveState::Saved);
  assert(retry.receipt.has_value());
  assert(retry.receipt->replayId == first.receipt->replayId);
  assertDatabaseCounts(replayPath, scorePath, 1, 1, 0);
}

void testCrashAfterStageRecoversExactlyOneScore() {
  TemporaryDirectory temporary("crash-after-stage");
  const auto replayPath = temporary.path() / "replay.db";
  const auto scorePath = temporary.path() / "score.db";
  ReplayRepository replay(replayPath);
  ScoreRepository score(scorePath);
  assert(score.EnsureSchema());
  const ChartResultAttempt fixed =
      sampleAttempt(temporary.path(), "fixed-stage-crash", 2);

  const StageOutcome staged = replay.StageChartResult(fixed, {});
  assert(staged.status == StageStatus::Staged);
  assert(staged.receipt.has_value());
  assertDatabaseCounts(replayPath, scorePath, 1, 0, 1);

  Coordinator coordinator(score, replay);
  const RecoverySummary recovered = coordinator.recoverAll();
  const RecoverySummary repeated = coordinator.recoverAll();

  assert(recovered.attempted == 1);
  assert(recovered.saved == 1);
  assert(recovered.pending == 0);
  assert(recovered.conflicts == 0);
  assert(recovered.userMessage.empty());
  assert(repeated.attempted == 0);
  assert(repeated.saved == 0);
  assertDatabaseCounts(replayPath, scorePath, 1, 1, 0);
}

void testCommittedScoreBeforeAckRecoversWithoutDuplicate() {
  TemporaryDirectory temporary("committed-before-ack");
  const auto replayPath = temporary.path() / "replay.db";
  const auto scorePath = temporary.path() / "score.db";
  ReplayRepository replay(replayPath);
  ScoreRepository score(scorePath);
  const ChartResultAttempt fixed =
      sampleAttempt(temporary.path(), "fixed-before-ack", 3);

  const StageOutcome staged = replay.StageChartResult(fixed, {});
  assert(staged.status == StageStatus::Staged);
  const PendingReadOutcome pendingScore =
      replay.LoadPendingChartScore(fixed.attemptId);
  assert(pendingScore.status == PendingReadStatus::Found);
  assert(pendingScore.value.has_value());
  assert(score.SaveProjectedScore(*pendingScore.value).status ==
         ProjectionStatus::Inserted);
  assertDatabaseCounts(replayPath, scorePath, 1, 1, 1);

  Coordinator coordinator(score, replay);
  const RecoverySummary recovered = coordinator.recoverAll();

  assert(recovered.attempted == 1);
  assert(recovered.saved == 1);
  assert(recovered.pending == 0);
  assert(recovered.conflicts == 0);
  assertDatabaseCounts(replayPath, scorePath, 1, 1, 0);
}

void testActivationFailureRetainsPendingAndRecoveryActivatesIr() {
  TemporaryDirectory temporary("ir-activation-recovery");
  const auto replayPath = temporary.path() / "replay.db";
  const auto scorePath = temporary.path() / "score.db";
  ReplayRepository replay(replayPath);
  ScoreRepository score(scorePath);
  const ChartResultAttempt fixed =
      sampleAttempt(temporary.path(), "fixed-ir-activation", 5);
  const std::array drafts{automaticIrDraft(fixed, 50'000)};

  const StageOutcome staged = replay.StageChartResult(fixed, drafts);
  assert(staged.status == StageStatus::Staged);
  const PendingReadOutcome pending =
      replay.LoadPendingChartScore(fixed.attemptId);
  assert(pending.status == PendingReadStatus::Found && pending.value);
  assert(score.SaveProjectedScore(*pending.value).status ==
         ProjectionStatus::Inserted);

  replay.Shutdown();
  auto database = openDatabase(replayPath);
  execOrAbort(database.get(),
              "CREATE TRIGGER fail_ir_activation BEFORE UPDATE OF "
              "local_result_ready ON ir_outbox BEGIN SELECT RAISE(ABORT, "
              "'forced IR activation failure'); END");
  database.reset();

  Coordinator coordinator(score, replay);
  const SaveOutcome failed = coordinator.persist(fixed, drafts);
  assert(failed.state == SaveState::PendingAcknowledgement);
  assertDatabaseCounts(replayPath, scorePath, 1, 1, 1);
  database = openDatabase(replayPath);
  assert(queryInt(database.get(),
                  "SELECT local_result_ready FROM ir_outbox") == 0);
  execOrAbort(database.get(), "DROP TRIGGER fail_ir_activation");
  database.reset();
  assert(replay.ListDueIrOutbox(100'000).entries.empty());

  const RecoverySummary recovered = coordinator.recoverAll();
  assert(recovered.attempted == 1);
  assert(recovered.saved == 1);
  assert(recovered.pending == 0);
  assert(recovered.conflicts == 0);
  assertDatabaseCounts(replayPath, scorePath, 1, 1, 0);
  const auto due = replay.ListDueIrOutbox(
      std::numeric_limits<std::int64_t>::max());
  assert(due.status == ir::IrOutboxBatchStatus::Loaded);
  assert(due.entries.size() == 1);
  assert(due.entries.front().localResultReady);
}

void testProfileSwitchCannotAcquireGateMidPersist() {
  TemporaryDirectory temporary("profile-gate");
  ReplayRepository replay(temporary.path() / "replay.db");
  ScoreRepository score(temporary.path() / "score.db");
  const ChartResultAttempt fixed =
      sampleAttempt(temporary.path(), "fixed-profile-gate", 4);

  std::mutex mutex;
  std::condition_variable condition;
  bool projectionEntered = false;
  bool releaseProjection = false;
  bool stageObservedWriteLease = false;

  Dependencies dependencies{
      .stage =
          [&](const ChartResultAttempt &value,
              std::span<const ir::IrOutboxDraft> drafts) {
            stageObservedWriteLease = profile_database_activity::writesActive();
            return replay.StageChartResult(value, drafts);
          },
      .loadPending =
          [&](std::string_view id) { return replay.LoadPendingChartScore(id); },
      .listPending =
          [&](std::size_t limit) {
            return replay.ListPendingChartScores(limit);
          },
      .project =
          [&](const PendingChartScoreWrite &value) {
            {
              std::lock_guard lock(mutex);
              projectionEntered = true;
            }
            condition.notify_all();
            {
              std::unique_lock lock(mutex);
              condition.wait(lock, [&] { return releaseProjection; });
            }
            return score.SaveProjectedScore(value);
          },
      .acknowledgeAndActivate =
          [&](std::string_view id, int replayId) {
            return replay.AcknowledgePendingChartScoreAndActivateIr(id, replayId);
          },
      .recordRecoveryAttempt =
          [&](std::string_view id, RecoveryAttemptKind kind) {
            return replay.RecordPendingChartScoreRecoveryAttempt(id, kind);
          },
  };
  Coordinator coordinator(std::move(dependencies));
  std::optional<SaveOutcome> saveOutcome;
  std::thread persistThread([&] { saveOutcome = coordinator.persist(fixed); });

  bool projectionReached = false;
  {
    std::unique_lock lock(mutex);
    projectionReached = condition.wait_for(lock, std::chrono::seconds(5),
                                           [&] { return projectionEntered; });
  }

  bool switchOwnedDuringPersist = true;
  bool switchProbeRan = false;
  if (projectionReached) {
    std::thread switchThread([&] {
      profile_database_activity::SwitchGuard switchGuard;
      switchOwnedDuringPersist = switchGuard.ownsLock();
    });
    switchThread.join();
    switchProbeRan = true;
  }

  {
    std::lock_guard lock(mutex);
    releaseProjection = true;
  }
  condition.notify_all();
  persistThread.join();

  profile_database_activity::SwitchGuard afterPersist;
  const bool switchOwnedAfterPersist = afterPersist.ownsLock();
  assert(projectionReached);
  assert(switchProbeRan);
  assert(stageObservedWriteLease);
  assert(!switchOwnedDuringPersist);
  assert(saveOutcome.has_value());
  assert(saveOutcome->state == SaveState::Saved);
  assert(switchOwnedAfterPersist);
}

void testSuccessfulBoundedRecoveryReportsRemainingBacklog() {
  TemporaryDirectory temporary("bounded-recovery-backlog");
  const auto replayPath = temporary.path() / "replay.db";
  const auto scorePath = temporary.path() / "score.db";
  ReplayRepository replay(replayPath);
  ScoreRepository score(scorePath);
  assert(score.EnsureSchema());

  for (int suffix = 0; suffix < 3; ++suffix) {
    const ChartResultAttempt attempt =
        sampleAttempt(temporary.path(),
                      "bounded-backlog-" + std::to_string(suffix), suffix);
    assert(replay.StageChartResult(attempt, {}).status == StageStatus::Staged);
  }
  assertDatabaseCounts(replayPath, scorePath, 3, 0, 3);

  Coordinator coordinator(score, replay);
  const RecoverySummary first = coordinator.recoverAll(2);

  assert(first.attempted == 2);
  assert(first.saved == 2);
  assert(first.pending == 1);
  assert(first.conflicts == 0);
  assert(!first.userMessage.empty());
  assertDatabaseCounts(replayPath, scorePath, 3, 2, 1);

  const RecoverySummary second = coordinator.recoverAll(2);
  assert(second.attempted == 1);
  assert(second.saved == 1);
  assert(second.pending == 0);
  assert(second.conflicts == 0);
  assert(second.userMessage.empty());
  assertDatabaseCounts(replayPath, scorePath, 3, 3, 0);
}

void testRecoveryRetainsConflictAndProcessesLaterValidRow() {
  TemporaryDirectory temporary("recovery-fairness");
  const auto replayPath = temporary.path() / "replay.db";
  const auto scorePath = temporary.path() / "score.db";
  ReplayRepository replay(replayPath);
  ScoreRepository score(scorePath);

  std::vector<std::string> conflictIds;
  conflictIds.reserve(256);
  for (int suffix = 0; suffix < 256; ++suffix) {
    const ChartResultAttempt conflict =
        sampleAttempt(temporary.path(),
                      "persistent-conflict-" + std::to_string(suffix), suffix);
    const StageOutcome staged = replay.StageChartResult(conflict, {});
    assert(staged.status == StageStatus::Staged);
    const PendingReadOutcome loaded =
        replay.LoadPendingChartScore(conflict.attemptId);
    assert(loaded.status == PendingReadStatus::Found);
    assert(loaded.value.has_value());
    PendingChartScoreWrite conflictingProjection = *loaded.value;
    ++conflictingProjection.score.fast;
    assert(score.SaveProjectedScore(conflictingProjection).status ==
           ProjectionStatus::Inserted);
    conflictIds.push_back(conflict.attemptId);
  }

  const ChartResultAttempt valid =
      sampleAttempt(temporary.path(), "never-attempted-valid", 256);
  assert(replay.StageChartResult(valid, {}).status == StageStatus::Staged);
  assertDatabaseCounts(replayPath, scorePath, 257, 256, 257);

  Coordinator coordinator(score, replay);
  const RecoverySummary first = coordinator.recoverAll();
  const RecoverySummary second = coordinator.recoverAll();

  assert(first.attempted == 256);
  assert(first.saved == 0);
  assert(first.pending == 1);
  assert(first.conflicts == 256);
  assert(!first.userMessage.empty());
  assert(second.attempted == 256);
  assert(second.saved == 1);
  assert(second.pending == 1);
  assert(second.conflicts == 255);
  assert(!second.userMessage.empty());
  assertDatabaseCounts(replayPath, scorePath, 257, 257, 256);

  auto replayDatabase = openDatabase(replayPath);
  assert(queryInt(replayDatabase.get(),
                  "SELECT COUNT(*) FROM pending_chart_score_writes WHERE "
                  "attempt_id=" +
                      quoteSql(valid.attemptId)) == 0);
  for (const std::string &id : conflictIds) {
    assert(queryInt(replayDatabase.get(),
                    "SELECT COUNT(*) FROM pending_chart_score_writes WHERE "
                    "attempt_id=" +
                        quoteSql(id)) == 1);
  }
}

} // namespace

int main() {
  testPersistCreatesOneReplayOneScoreNoPending();
  testCrashAfterStageRecoversExactlyOneScore();
  testCommittedScoreBeforeAckRecoversWithoutDuplicate();
  testActivationFailureRetainsPendingAndRecoveryActivatesIr();
  testProfileSwitchCannotAcquireGateMidPersist();
  testSuccessfulBoundedRecoveryReportsRemainingBacklog();
  testRecoveryRetainsConflictAndProcessesLaterValidRow();
  std::cout << "result persistence integration tests passed\n";
  return 0;
}
