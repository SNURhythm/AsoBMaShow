#include "ResultPersistenceCoordinator.h"
#include "FileChecksum.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The focused coordinator tests inject every side effect. These definitions
// satisfy the production convenience constructors linked into the same object
// without opening SQLite or touching the filesystem.
ReservationOutcome ReplayRepository::reserveReplayFile(std::string_view,
                                                        std::string_view) {
  return {};
}

std::filesystem::path ReplayRepository::GetResolvedDatabasePath() const {
  return {};
}

bool ReplayRepository::markReplayFileReservationFinalized(
    const ReplayFileReservation &, const replay::ReplayFileMetadata &,
    std::string &) {
  return false;
}

result_persistence::StageOutcome ReplayRepository::stageCompletedChartAttempt(
    const result_persistence::PersistedChartResult &,
    const ir::IrSubmissionSnapshot &, const ReplayFileReference &,
    std::span<const ir::IrOutboxDraft>) {
  return {};
}

result_persistence::StageOutcome ReplayRepository::stageCompletedCourseAttempt(
    const result_persistence::PersistedCourseResult &,
    const ReplayFileReference &) {
  return {};
}

result_persistence::PendingReadOutcome
ReplayRepository::LoadPendingChartScore(std::string_view) {
  return {};
}

result_persistence::PendingBatchOutcome
ReplayRepository::ListPendingChartScores(std::size_t) {
  return {};
}

result_persistence::AcknowledgeOutcome
ReplayRepository::AcknowledgePendingChartScoreAndActivateIr(std::string_view,
                                                            int) {
  return {};
}

result_persistence::RecoveryMarkOutcome
ReplayRepository::RecordPendingChartScoreRecoveryAttempt(
    std::string_view, result_persistence::RecoveryAttemptKind) {
  return {};
}

result_persistence::ProjectionOutcome ScoreRepository::SaveProjectedScore(
    const result_persistence::PendingChartScoreWrite &) {
  return {};
}

namespace replay {

BeatorajaReplayCodec::BeatorajaReplayCodec(ReplayCodecLimits limits)
    : limits_(limits) {}

std::optional<std::vector<std::byte>> BeatorajaReplayCodec::encodeChart(
    const ReplayPlaybackData &, std::int64_t, std::string &) const {
  return std::nullopt;
}

std::optional<std::vector<std::byte>> BeatorajaReplayCodec::encodeCourse(
    const CourseReplayPlaybackData &, std::int64_t, std::string &) const {
  return std::nullopt;
}

ReplayFileStore::ReplayFileStore(std::filesystem::path profileRoot,
                                 ReplayFileStoreFaults faults)
    : profileRoot_(std::move(profileRoot)), faults_(std::move(faults)) {}

FinalizeOutcome ReplayFileStore::finalize(
    const ReplayPathIdentity &, std::span<const std::byte>,
    const BeatorajaReplayCodec &, const ExpectedReplayIdentity &,
    std::string_view) {
  return {};
}

} // namespace replay

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

CompletedChartAttempt validAttempt() {
  CompletedChartAttempt attempt;
  auto &result = attempt.result;
  result.attemptId = "123e4567-e89b-42d3-a456-426614174000";
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
  result.judgementTiming = ChartJudgementTiming{};
  result.judgementTiming->byJudgement[PGreat] = {.fast = 1, .slow = 1};
  result.judgementTiming->byJudgement[Great] = {.fast = 1, .slow = 0};
  result.playedAtUnixMillis = 1'700'000'000'123LL;
  result.resultFingerprint = resultFingerprint(result);

  attempt.replay.setup.chartMd5 = result.score.chartMd5;
  attempt.replay.setup.chartSha256 = result.score.chartSha256;
  attempt.replay.setup.keyMode = result.keyMode;
  attempt.replay.setup.longNoteMode = result.score.longNoteMode;
  attempt.replay.setup.playbackRulesetId = "asobmashow";
  attempt.replay.setup.playbackRulesetRevision = 11;
  attempt.replay.input.push_back(
      {.songTimeMicros = 1000,
       .control = {.kind = replay::LogicalControlKind::Lane,
                   .player = 1,
                   .lane = 0},
       .pressed = true});

  std::string diagnostic;
  auto snapshot = ir::captureIrSubmissionSnapshot(result, diagnostic);
  expect(snapshot.has_value(), "fixture IR snapshot captures");
  if (snapshot) {
    attempt.irSnapshot = *snapshot;
  }
  return attempt;
}

CompletedChartAttempt verifiedAttempt() {
  auto attempt = validAttempt();
  auto &result = attempt.result;
  ScoreProvenanceBuildInput input;
  input.chartMeta.MD5 = result.score.chartMd5;
  input.chartMeta.SHA256 = result.score.chartSha256;
  input.chartMeta.KeyMode = result.keyMode;
  input.chartMeta.Rank = 1;
  input.chartMeta.TotalNotes = result.score.maxScore / 2;
  input.chartMeta.HasTotal = true;
  input.chartMeta.Total = 200.0;
  input.longNoteMode = result.score.longNoteMode;
  input.effectiveJudgeWindows = {
      {PGreat, {-10'000, 10'000}}, {Great, {-30'000, 30'000}},
      {Good, {-75'000, 75'000}},   {Bad, {-330'000, 420'000}},
      {Kpoor, {-500'000, 150'000}},
  };
  input.totalNotes = input.chartMeta.TotalNotes;
  input.authoredGaugeTotal = input.chartMeta.Total;
  input.effectiveGaugeTotal = input.chartMeta.Total;
  input.doublePlayOption = replay::DoublePlayOption::Flip;
  result.score.provenance = makeScoreProvenance(input);
  result.resultFingerprint = resultFingerprint(result);

  auto &setup = attempt.replay.setup;
  setup.doublePlayOption = input.doublePlayOption;
  setup.playbackRulesetId = input.ruleset.id;
  setup.playbackRulesetRevision = input.ruleset.version;
  setup.startingGaugePercent = 20.0F;

  std::string diagnostic;
  const auto snapshot = ir::captureIrSubmissionSnapshot(result, diagnostic);
  expect(snapshot.has_value(), "verified fixture IR snapshot captures");
  if (snapshot.has_value()) {
    attempt.irSnapshot = *snapshot;
  }
  return attempt;
}

CompletedCourseAttempt verifiedCourseAttempt() {
  const auto chart = verifiedAttempt();
  CompletedCourseAttempt attempt;
  auto &result = attempt.result;
  result.attemptId = "123e4567-e89b-42d3-a456-426614174001";
  result.courseKey = "course:v1:" + repeated('c', 64);
  result.courseName = "Course";
  result.courseGroupName = "Tests";
  result.constraintJson = "[]";
  result.completedCharts = 1;
  result.totalCharts = 1;
  result.requestedPlayOption = chart.result.score.provenance.player1.option;
  result.assistOption = chart.result.score.provenance.assistOption;
  result.initialGaugeType = chart.result.score.provenance.gaugeType;
  result.gaugeProfile = chart.result.score.provenance.gaugeProfile;
  result.gaugeAutoShift = chart.result.score.provenance.gaugeAutoShift;
  result.gaugeAutoShiftLowerBound =
      chart.result.score.provenance.gaugeAutoShiftLowerBound;
  result.longNoteMode = chart.result.score.longNoteMode;
  result.finalScore = chart.result.score.score;
  result.maxScore = chart.result.score.maxScore;
  result.maxCombo = chart.result.score.maxCombo;
  result.finalGauge = chart.result.score.finalGauge;
  result.clearType = chart.result.score.clearType;
  result.provenance = chart.result.score.provenance;
  result.stages = {{.stageIndex = 0,
                    .score = chart.result.score,
                    .keyMode = chart.result.keyMode,
                    .adoptedGaugeType = chart.result.adoptedGaugeType,
                    .adoptedGaugeHistory =
                        chart.result.adoptedGaugeHistory,
                    .judgementTiming = chart.result.judgementTiming}};
  result.entryFacts = {{.totalNotes = chart.result.score.maxScore / 2,
                        .playLengthMicros = 2'000'000}};
  result.playedAtUnixMillis = chart.result.playedAtUnixMillis;
  result.resultFingerprint = resultFingerprint(result);
  attempt.replay.stages = {chart.replay};
  attempt.replay.restMicrosAfterStage = {0};
  return attempt;
}

struct Harness {
  CompletedChartAttempt attempt = validAttempt();
  std::vector<std::string> events;
  ReservationOutcome reservation;
  bool encodeSucceeds = true;
  bool ownershipRecords = true;
  std::optional<replay::ReplayFileMetadata> recordedOwnership;
  replay::FinalizeOutcome finalized;
  StageOutcome staged;
  PendingReadOutcome loaded;
  ProjectionOutcome projected;
  AcknowledgeOutcome acknowledged;
  PendingBatchOutcome batch;
  RecoveryMarkOutcome marked;

  Harness() {
    const std::string &attemptId = *attempt.result.attemptId;
    const std::string &stem = attempt.result.score.chartSha256;
    reservation = {
        .status = ReservationOutcome::Status::Reserved,
        .reservation = ReplayFileReservation{
            .attemptId = attemptId,
            .stem = stem,
            .historyIndex = 0,
            .relativePath = std::filesystem::path("replay") / (stem + ".brd"),
        },
    };
    const std::vector<std::byte> encoded{std::byte{0x42}};
    file_checksum::Sha256 hash;
    hash.update(encoded);
    finalized = {
        .metadata =
            replay::ReplayFileMetadata{
                .relativePath = reservation.reservation->relativePath,
                .sha256 = hash.finalHex(),
                .compressedSize = encoded.size(),
                .codecVersion = replay::BeatorajaReplayCodec::kCodecVersion,
            },
    };
    staged = {
        .status = StageStatus::Staged,
        .receipt = StageReceipt{.attemptId = attemptId,
                                .resultId = 17,
                                .createdAt = "2026-07-25T10:00:00Z",
                                .scorePending = true},
    };
    loaded = {
        .status = PendingReadStatus::Found,
        .value = PendingChartScoreWrite{
            .attemptId = attemptId,
            .resultId = 17,
            .createdAt = "2026-07-25T10:00:00Z",
            .score = attempt.result.score,
        },
    };
    projected = {.status = ProjectionStatus::Inserted};
    acknowledged = {.status = AcknowledgeStatus::Acknowledged};
    batch = {.storageAvailable = true};
    marked = {.status = RecoveryMarkStatus::Recorded};
  }

  Dependencies dependencies() {
    return {
        .reserve =
            [this](std::string_view, std::string_view) {
              events.emplace_back("reserve");
              return reservation;
            },
        .encodeReplay = [this](const replay::ReplayPlaybackData &, std::int64_t,
                               std::string &diagnostic)
            -> std::optional<std::vector<std::byte>> {
          events.emplace_back("encode");
          if (!encodeSucceeds) {
            diagnostic = "invalid replay";
            return std::nullopt;
          }
          return std::vector<std::byte>{std::byte{0x42}};
        },
        .finalizeReplay =
            [this](const replay::ReplayPathIdentity &,
                   std::span<const std::byte>,
                   const replay::ExpectedReplayIdentity &, std::string_view) {
              events.emplace_back("finalize");
              return finalized;
            },
        .recordFinalizedReplay =
            [this](const ReplayFileReservation &,
                   const replay::ReplayFileMetadata &metadata,
                   std::string &diagnostic) {
              events.emplace_back("own");
              recordedOwnership = metadata;
              if (!ownershipRecords) {
                diagnostic = "ownership write failed";
              }
              return ownershipRecords;
            },
        .stage =
            [this](const PersistedChartResult &,
                   const ir::IrSubmissionSnapshot &,
                   const ReplayFileReference &,
                   std::span<const ir::IrOutboxDraft>) {
              events.emplace_back("stage");
              return staged;
            },
        .loadPending =
            [this](std::string_view) {
              events.emplace_back("load");
              return loaded;
            },
        .listPending =
            [this](std::size_t) {
              events.emplace_back("list");
              return batch;
            },
        .project =
            [this](const PendingChartScoreWrite &) {
              events.emplace_back("project");
              return projected;
            },
        .acknowledgeAndActivate =
            [this](std::string_view, int) {
              events.emplace_back("ack");
              return acknowledged;
            },
        .recordRecoveryAttempt =
            [this](std::string_view, RecoveryAttemptKind) {
              events.emplace_back("mark");
              return marked;
            },
    };
  }
};

void testFileFirstSuccessOrdering() {
  Harness harness;
  Coordinator coordinator(harness.dependencies());
  const SaveOutcome outcome = coordinator.persist(harness.attempt);
  expect(outcome.state == SaveState::Saved, "complete pipeline is saved");
  expect(outcome.durable() && !outcome.retryable(),
         "saved pipeline is durable and final");
  expect(outcome.receipt && !outcome.receipt->scorePending &&
             outcome.receipt->resultId == 17,
         "saved receipt contains the compact result ID");
  expect(harness.events ==
             std::vector<std::string>({"reserve", "encode", "own", "finalize",
                                       "stage", "load", "project", "ack"}),
         "replay ownership is recorded before final-file visibility, compact "
         "DB staging, and score projection");
  expect(harness.recordedOwnership == harness.finalized.metadata,
         "the pre-install ownership marker contains the final path, hash, and "
         "compressed size");
  expect(outcome.validatedReceiptFor(harness.attempt) != nullptr,
         "receipt validates against the completed attempt");
}

void testValidationStopsBeforeSideEffects() {
  Harness harness;
  harness.attempt.replay.setup.chartSha256 = repeated('d', 64);
  Coordinator coordinator(harness.dependencies());
  const auto outcome = coordinator.persist(harness.attempt);
  expect(outcome.state == SaveState::InvalidAttempt,
         "mismatched replay setup is invalid");
  expect(harness.events.empty(), "invalid attempt has no persistence effects");
}

void testSetupAuthorityStopsBeforeSideEffects() {
  {
    Harness harness;
    harness.attempt = verifiedAttempt();
    harness.attempt.replay.setup.doublePlayOption =
        replay::DoublePlayOption::Normal;
    Coordinator coordinator(harness.dependencies());
    const auto outcome = coordinator.persist(harness.attempt);
    expect(outcome.state == SaveState::InvalidAttempt,
           "DP mismatch is invalid before persistence");
    expect(harness.events.empty(),
           "DP mismatch creates no reservation, file, result, or IR draft");
  }
  {
    Harness harness;
    harness.attempt = verifiedAttempt();
    harness.attempt.replay.setup.playbackRatePercent = 95;
    Coordinator coordinator(harness.dependencies());
    const auto outcome = coordinator.persist(harness.attempt);
    expect(outcome.state == SaveState::InvalidAttempt,
           "non-DP setup mismatch uses the same creation-time authority");
    expect(harness.events.empty(),
           "complete setup mismatch stops before persistence effects");
  }
  {
    auto attempt = verifiedCourseAttempt();
    attempt.replay.stages.front().setup.doublePlayOption =
        replay::DoublePlayOption::Normal;
    Coordinator coordinator(Dependencies{});
    const auto outcome = coordinator.persistCourse(attempt);
    expect(outcome.state == SaveState::InvalidAttempt,
           "course DP mismatch is invalid before persistence");
  }
}

void testPhaseFailuresStopAtTheirBoundary() {
  {
    Harness harness;
    harness.reservation = {
        .status = ReservationOutcome::Status::StorageFailure,
        .diagnostic = "reserve failed"};
    Coordinator coordinator(harness.dependencies());
    const auto outcome = coordinator.persist(harness.attempt);
    expect(outcome.state == SaveState::Unstaged && outcome.retryable(),
           "reservation storage failure is retryable and unstaged");
    expect(harness.events == std::vector<std::string>({"reserve"}),
           "reservation failure stops encoding");
  }
  {
    Harness harness;
    harness.encodeSucceeds = false;
    Coordinator coordinator(harness.dependencies());
    const auto outcome = coordinator.persist(harness.attempt);
    expect(outcome.state == SaveState::InvalidAttempt,
           "encoding rejection is a permanent invalid attempt");
    expect(harness.events ==
               std::vector<std::string>({"reserve", "encode"}),
           "encoding rejection stops file finalization");
  }
  {
    Harness harness;
    harness.finalized = {.diagnostic = "rename failed"};
    Coordinator coordinator(harness.dependencies());
    const auto outcome = coordinator.persist(harness.attempt);
    expect(outcome.state == SaveState::UnfinalizedReplay &&
               outcome.retryable() && !outcome.durable(),
           "file finalization failure is explicit and retryable");
    expect(harness.events == std::vector<std::string>(
                                 {"reserve", "encode", "own", "finalize"}),
           "unfinalized replay never reaches SQLite staging");
  }
  {
    Harness harness;
    harness.staged = {.status = StageStatus::StorageFailure,
                      .diagnostic = "transaction failed"};
    Coordinator coordinator(harness.dependencies());
    const auto outcome = coordinator.persist(harness.attempt);
    expect(outcome.state == SaveState::Unstaged && !outcome.durable(),
           "staging failure reports a durable file but no durable result");
    expect(harness.events ==
               std::vector<std::string>(
                   {"reserve", "encode", "own", "finalize", "stage"}),
           "staging failure stops projection");
  }
  {
    Harness harness;
    harness.ownershipRecords = false;
    Coordinator coordinator(harness.dependencies());
    const auto outcome = coordinator.persist(harness.attempt);
    expect(outcome.state == SaveState::UnfinalizedReplay &&
               outcome.retryable() && !outcome.durable(),
           "ownership-marker failure keeps finalized replay retryable");
    expect(harness.events ==
               std::vector<std::string>({"reserve", "encode", "own"}),
           "ownership-marker failure stops before final-file installation");
  }
}

void testRetryAndMetadataIntegrity() {
  Harness harness;
  harness.reservation.status = ReservationOutcome::Status::AlreadyReserved;
  harness.finalized.existingIdenticalFile = true;
  harness.staged.status = StageStatus::AlreadyStaged;
  Coordinator coordinator(harness.dependencies());
  const auto retry = coordinator.persist(harness.attempt);
  expect(retry.saved(), "idempotent reservation, file, and stage retry saves");

  Harness corrupt;
  corrupt.finalized.metadata->relativePath = "replay/different.brd";
  Coordinator corruptCoordinator(corrupt.dependencies());
  const auto conflict = corruptCoordinator.persist(corrupt.attempt);
  expect(conflict.state == SaveState::UnstagedConflict,
         "finalized path mismatch fails closed");
  expect(corrupt.events ==
             std::vector<std::string>({"reserve", "encode", "own", "finalize"}),
         "path conflict stops staging");
}

void testPendingProjectionStates() {
  Harness projectionFailure;
  projectionFailure.projected = {
      .status = ProjectionStatus::StorageFailure,
      .diagnostic = "score DB unavailable"};
  Coordinator first(projectionFailure.dependencies());
  const auto pendingScore = first.persist(projectionFailure.attempt);
  expect(pendingScore.state == SaveState::PendingScore &&
             pendingScore.durable() && pendingScore.retryable(),
         "projection failure retains a durable pending result");
  expect(projectionFailure.events.back() == "project",
         "projection failure does not acknowledge");

  Harness acknowledgementFailure;
  acknowledgementFailure.acknowledged = {
      .status = AcknowledgeStatus::StorageFailure,
      .diagnostic = "ack failed"};
  Coordinator second(acknowledgementFailure.dependencies());
  const auto pendingAck = second.persist(acknowledgementFailure.attempt);
  expect(pendingAck.state == SaveState::PendingAcknowledgement &&
             pendingAck.durable() && pendingAck.retryable(),
         "acknowledgement failure retains a durable pending result");
}

void testRecoveryUsesResultIdentity() {
  Harness harness;
  harness.batch.entries.push_back({
      .status = PendingReadStatus::Found,
      .attemptId = *harness.attempt.result.attemptId,
      .value = *harness.loaded.value,
  });
  Coordinator coordinator(harness.dependencies());
  const auto summary = coordinator.recoverAll();
  expect(summary.attempted == 1 && summary.saved == 1 &&
             summary.pending == 0 && summary.conflicts == 0,
         "recovery projects and acknowledges a compact pending result");
  expect(harness.events ==
             std::vector<std::string>({"list", "project", "ack"}),
         "successful recovery does not mutate raw replay data");

  auto incomplete = harness.dependencies();
  incomplete.listPending = {};
  Coordinator missing(std::move(incomplete));
  const auto failed = missing.recoverAll();
  expect(failed.pending == 1 && !failed.userMessage.empty(),
         "incomplete recovery dependencies fail without throwing");
}

} // namespace

int main() {
  testFileFirstSuccessOrdering();
  testValidationStopsBeforeSideEffects();
  testSetupAuthorityStopsBeforeSideEffects();
  testPhaseFailuresStopAtTheirBoundary();
  testRetryAndMetadataIntegrity();
  testPendingProjectionStates();
  testRecoveryUsesResultIdentity();
  if (failures != 0) {
    std::cerr << failures << " result persistence coordinator test(s) failed\n";
    return 1;
  }
  std::cout << "Result persistence coordinator tests passed\n";
  return 0;
}
