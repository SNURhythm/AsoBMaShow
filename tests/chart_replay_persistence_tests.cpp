#include "replay/ChartReplayPersistence.h"
#include "replay/ChartReplayCapture.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace replay;

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
    path = std::filesystem::temp_directory_path() /
           ("asobmashow-chart-replay-persistence-" +
            std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(path);
  }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
  std::filesystem::path path;
};

result_persistence::ModernChartResult result() {
  result_persistence::ModernChartResult value;
  value.attemptId = "123e4567-e89b-42d3-a456-426614174000";
  value.score.chartPath = "library/chart.bms";
  value.score.chartMd5 = repeated('b', 32);
  value.score.chartSha256 = repeated('a', 64);
  value.score.chartTitle = "Title";
  value.score.chartArtist = "Artist";
  value.score.longNoteMode = 1;
  value.score.score = 7;
  value.score.maxScore = 10;
  value.score.maxCombo = 4;
  value.score.comboBreak = 1;
  value.score.pGreat = 3;
  value.score.great = 1;
  value.score.good = 1;
  value.score.finalGauge = 82.5F;
  value.score.clearType = kClearTypeNormalClearRank;

  ScoreProvenanceBuildInput provenance;
  provenance.chartMeta.MD5 = value.score.chartMd5;
  provenance.chartMeta.SHA256 = value.score.chartSha256;
  provenance.chartMeta.KeyMode = 7;
  provenance.chartMeta.Rank = 2;
  provenance.chartMeta.TotalNotes = 5;
  provenance.chartMeta.HasTotal = true;
  provenance.chartMeta.Total = 200.0;
  provenance.longNoteMode = value.score.longNoteMode;
  provenance.sourceJudgeRank = 2;
  provenance.effectiveJudgeWindows = {
      {PGreat, {-10'000, 10'000}}, {Great, {-30'000, 30'000}},
      {Good, {-75'000, 75'000}},   {Bad, {-200'000, 200'000}},
      {Kpoor, {-1'000'000, 0}},
  };
  provenance.totalNotes = 5;
  provenance.authoredGaugeTotal = 200.0;
  provenance.effectiveGaugeTotal = 200.0;
  provenance.startingGaugePercent = 20;
  provenance.inputDevices = {InputDeviceCategory::Keyboard};
  value.score.provenance = makeScoreProvenance(provenance);
  value.keyMode = 7;
  value.adoptedGaugeType = GaugeType::Normal;
  value.adoptedGaugeHistory = {20.0F, 48.5F, 82.5F};
  value.playedAtUnixMillis = 1'700'000'000'123LL;
  value.resultFingerprint = result_persistence::modernResultFingerprint(value);
  std::string diagnostic;
  expect(result_persistence::validateModernChartResult(value, diagnostic),
         "modern result fixture validates");
  return value;
}

ReplayChartDocument
replayDocument(const result_persistence::ModernChartResult &saved) {
  ReplayChartDocument replay;
  replay.timeBounds = {.completionSongTimeMicros = 5'000'000};
  replay.playback.setup.chart = {.md5 = saved.score.chartMd5,
                                 .sha256 = saved.score.chartSha256,
                                 .keyMode = saved.keyMode};
  replay.playback.setup.longNoteMode = saved.score.longNoteMode;
  replay.playback.setup.initialGaugeType = saved.score.provenance.gaugeType;
  replay.playback.setup.gaugeProfile = saved.score.provenance.gaugeProfile;
  replay.playback.setup.gaugeAutoShift = saved.score.provenance.gaugeAutoShift;
  replay.playback.setup.gaugeAutoShiftLowerBound =
      saved.score.provenance.gaugeAutoShiftLowerBound;
  replay.playback.setup.ruleset = saved.score.provenance.ruleset;
  replay.playback.setup.playback = saved.score.provenance.playback;
  replay.playback.setup.candidateSelection =
      saved.score.provenance.stages.front().candidateSelection;
  replay.playback.setup.judgeWindowScalePercent =
      saved.score.provenance.judgeWindowScalePercent;
  replay.playback.setup.startingGaugePercent = 20.0F;
  replay.playback.input = {
      {.songTimeMicros = -1'000,
       .control = {.kind = LogicalControlKind::Lane, .player = 1, .lane = 0},
       .pressed = true},
      {.songTimeMicros = 1'000,
       .control = {.kind = LogicalControlKind::Lane, .player = 1, .lane = 0},
       .pressed = false},
  };
  return replay;
}

ChartReplayPersistenceAttempt validAttempt(bool withReplay = true) {
  ChartReplayPersistenceAttempt attempt{.result = result()};
  std::string diagnostic;
  attempt.irSnapshot =
      ir::captureIrSubmissionSnapshot(attempt.result, diagnostic);
  expect(attempt.irSnapshot.has_value(), "IR snapshot fixture captures");
  if (withReplay) {
    attempt.replay = replayDocument(attempt.result);
  }
  return attempt;
}

void testCompletionCaptureBuildsIndependentResultSnapshotAndReplay() {
  auto completed = result();
  ChartReplayCapture capture{
      .result = completed,
      .setupFacts =
          {.chart = {.md5 = completed.score.chartMd5,
                     .sha256 = completed.score.chartSha256,
                     .keyMode = completed.keyMode},
           .longNoteMode = completed.score.longNoteMode,
           .initialLaneCoverPercent = 37,
           .laneCoverEnabled = true},
      .acceptedInput =
          std::vector<InputTransition>{
              {.songTimeMicros = -1'000,
               .control = {.kind = LogicalControlKind::Lane,
                           .player = 1,
                           .lane = 0},
               .pressed = true},
              {.songTimeMicros = 1'000,
               .control = {.kind = LogicalControlKind::Lane,
                           .player = 1,
                           .lane = 0},
               .pressed = false}},
      .touchSamples = {{.action = replay::ReplayTouchAction::Down,
                        .fingerId = 4,
                        .songTimeMicros = 0,
                        .x = 0.25F,
                        .y = 0.75F}},
      .laneCoverEvents = {{.songTimeMicros = 0,
                           .noteStartPositionPercent = 37}},
      .timeBounds = {.completionSongTimeMicros = 5'000'000},
  };
  std::string diagnostic;
  const auto captured =
      captureChartReplayPersistenceAttempt(capture, diagnostic);
  expect(captured && captured->result == completed && captured->irSnapshot &&
             captured->replay &&
             captured->replay->playback.input == *capture.acceptedInput &&
             captured->replay->playback.touchSamples == capture.touchSamples &&
             captured->replay->playback.laneCoverEvents ==
                 capture.laneCoverEvents,
         "completion captures compact result, IR snapshot, and raw BRD facts");

  capture.acceptedInput.reset();
  const auto replayless =
      captureChartReplayPersistenceAttempt(capture, diagnostic);
  expect(replayless && replayless->result == completed &&
             replayless->irSnapshot && !replayless->replay,
         "lost raw capture drops only replay attachment");

  capture.acceptedInput = std::vector<InputTransition>{};
  capture.touchSamples.front().x = 2.0F;
  const auto malformed =
      captureChartReplayPersistenceAttempt(capture, diagnostic);
  expect(malformed && malformed->result == completed &&
             malformed->irSnapshot && !malformed->replay &&
             !diagnostic.empty(),
         "malformed raw detail cannot discard result or postponed IR facts");
}

ReplayFileMetadata metadata(const ReplayPathIdentity &identity,
                            char hash = 'c') {
  return {.relativePath = identity.relativePath,
          .sha256 = repeated(hash, 64),
          .compressedSize = 1,
          .codecVersion = BeatorajaReplayCodec::kCodecVersion};
}

struct Harness {
  ChartReplayPersistenceAttempt attempt = validAttempt();
  std::vector<std::string> events;
  ModernChartResultReadOutcome existing{
      .status = ModernChartResultReadStatus::NotFound};
  ModernReplayReservationOutcome reserved;
  ModernReplayReservationReleaseOutcome released{
      .status = ModernReplayReservationReleaseStatus::Released};
  bool encodeSucceeds = true;
  bool fileReservationSucceeds = true;
  ReplayInstallOutcome installed;
  ReplayFileInspection inspected{.state = ReplayFileState::Missing};
  bool cleanupSucceeds = true;
  ModernChartStageOutcome staged;
  result_persistence::PendingReadOutcome pending;
  result_persistence::ProjectionOutcome projected{
      .status = result_persistence::ProjectionStatus::Inserted};
  result_persistence::AcknowledgeOutcome acknowledged{
      .status = result_persistence::AcknowledgeStatus::Acknowledged};
  result_persistence::PendingBatchOutcome batch{.storageAvailable = true};
  result_persistence::RecoveryMarkOutcome marked{
      .status = result_persistence::RecoveryMarkStatus::Recorded};
  std::int64_t nextHistory = 0;

  Harness() {
    std::string diagnostic;
    const auto stem =
        chartStem(attempt.result.score.chartSha256,
                  attempt.result.score.longNoteMode, false, diagnostic);
    const auto identity = pathForStem(*stem, 0, diagnostic);
    reserved = {
        .status = ModernReplayReservationStatus::Reserved,
        .reservation =
            ModernReplayPathReservation{.attemptId = attempt.result.attemptId,
                                        .identity = *identity,
                                        .createdAtUnixMillis =
                                            attempt.result.playedAtUnixMillis},
    };
    const auto file = metadata(*identity);
    installed = {
        .state = ReplayInstallState::InstalledVerified,
        .file =
            ReplayInstalledFile{
                .metadata = file,
                .attemptToken = attempt.result.attemptId,
                .lifecycle =
                    {.state = ReplayFileLifecycleState::InstalledUnassociated,
                     .attemptToken = attempt.result.attemptId,
                     .receipt =
                         ReplayFileOwnershipReceipt{
                             .attemptToken = attempt.result.attemptId,
                             .metadata = file}}},
    };
    staged = {
        .status = ModernChartStageStatus::Staged,
        .receipt =
            ModernChartStageReceipt{.attemptId = attempt.result.attemptId,
                                    .resultId = 17,
                                    .createdAt = "2026-07-27 12:00:00"},
    };
    pending = {
        .status = result_persistence::PendingReadStatus::Found,
        .value =
            result_persistence::PendingChartScoreWrite{
                .attemptId = attempt.result.attemptId,
                .modernResultId = 17,
                .createdAt = "2026-07-27 12:00:00",
                .score = attempt.result.score},
    };
  }

  ChartReplayPersistenceDependencies dependencies() {
    return {
        .loadResult =
            [this](std::string_view) {
              events.emplace_back("load-result");
              return existing;
            },
        .fileAssociation =
            {.reservePath =
                 [this](std::string_view attemptId, std::string_view stem,
                        std::int64_t playedAt) {
                   events.emplace_back("reserve-path");
                   if (nextHistory == 0) {
                     ++nextHistory;
                     return reserved;
                   }
                   std::string diagnostic;
                   const auto identity =
                       pathForStem(stem, nextHistory++, diagnostic);
                   return ModernReplayReservationOutcome{
                       .status = ModernReplayReservationStatus::Reserved,
                       .reservation = ModernReplayPathReservation{
                           .attemptId = std::string(attemptId),
                           .identity = *identity,
                           .createdAtUnixMillis = playedAt}};
                 },
             .releasePath =
                 [this](const auto &) {
                   events.emplace_back("release-path");
                   return released;
                 },
             .reserveFile =
                 [this](const auto &identity, auto,
                        std::string_view token) {
                   events.emplace_back("reserve-file");
                   if (!fileReservationSucceeds) {
                     return ReplayReservationOutcome{
                         .diagnostic = "file reserve failed"};
                   }
                   return ReplayReservationOutcome{
                       .reservation = ReplayFileReservation{
                           .identity = identity,
                           .attemptToken = std::string(token),
                           .expectedMetadata = metadata(identity),
                           .temporaryRelativePath = "replay/private.tmp"}};
                 },
             .installFile =
                 [this](const auto &reservation, auto) {
                   events.emplace_back("install");
                   auto outcome = installed;
                   if (outcome.file) {
                     outcome.file->metadata = reservation.expectedMetadata;
                     if (outcome.file->lifecycle.receipt) {
                       outcome.file->lifecycle.receipt->metadata =
                           reservation.expectedMetadata;
                     }
                   }
                   return outcome;
                 },
             .recordInstalledOwnership =
                 [this](const ModernReplayPathReservation &reservation,
                        const ReplayFileOwnershipReceipt &receipt) {
                   events.emplace_back("record-ownership");
                   auto owned = reservation;
                   owned.ownedFile = receipt.metadata;
                   return ModernReplayOwnershipRecordOutcome{
                       .status = ModernReplayOwnershipRecordStatus::Recorded,
                       .reservation = std::move(owned)};
                 },
             .inspectFile =
                 [this](const auto &) {
                   events.emplace_back("inspect");
                   return inspected;
                 },
             .removeIfMatches =
                 [this](const auto &, std::string &diagnostic) {
                   events.emplace_back("cleanup");
                   if (!cleanupSucceeds) {
                     diagnostic = "cleanup failed";
                   }
                   return cleanupSucceeds;
                 }},
        .encode = [this](const auto &, std::int64_t, std::string &diagnostic)
            -> std::optional<std::vector<std::byte>> {
          events.emplace_back("encode");
          if (!encodeSucceeds) {
            diagnostic = "encode failed";
            return std::nullopt;
          }
          return std::vector<std::byte>{std::byte{0x42}};
        },
        .stage =
            [this](const auto &, const auto &, const auto &attachment, auto) {
              events.emplace_back(attachment ? "stage-file" : "stage-summary");
              return staged;
            },
        .loadPending =
            [this](std::string_view) {
              events.emplace_back("load-pending");
              return pending;
            },
        .listPending =
            [this](std::size_t) {
              events.emplace_back("list-pending");
              return batch;
            },
        .project =
            [this](const auto &) {
              events.emplace_back("project");
              return projected;
            },
        .acknowledge =
            [this](std::string_view, int) {
              events.emplace_back("ack");
              return acknowledged;
            },
        .recordRecoveryAttempt =
            [this](std::string_view, auto) {
              events.emplace_back("mark");
              return marked;
            },
    };
  }
};

void testFileFirstSuccessAndExactRetry() {
  Harness harness;
  ChartReplayPersistence persistence(harness.dependencies());
  const auto saved = persistence.persist(harness.attempt);
  expect(saved.state == ChartReplayPersistenceState::SavedWithReplay &&
             saved.saved() && saved.durable() && !saved.retryable() &&
             saved.replayAttached,
         "complete file-first pipeline saves a replay-backed result");
  expect(harness.events ==
             std::vector<std::string>({"load-result", "reserve-path", "encode",
                                       "reserve-file", "install",
                                       "record-ownership", "stage-file",
                                       "load-pending", "project", "ack"}),
         "file bytes install before the result transaction and projection");

  Harness retry;
  auto stored = retry.attempt.result;
  stored.resultId = 17;
  retry.existing = {
      .status = ModernChartResultReadStatus::Loaded,
      .record = ModernChartResultRecord{
          .result = stored,
          .replayFile = ModernReplayFileReference{
              .id = 1,
              .resultId = 17,
              .identity = retry.reserved.reservation->identity,
              .metadata = metadata(retry.reserved.reservation->identity)}}};
  retry.staged.status = ModernChartStageStatus::AlreadyStaged;
  ChartReplayPersistence retryPersistence(retry.dependencies());
  const auto repeated = retryPersistence.persist(retry.attempt);
  expect(repeated.state == ChartReplayPersistenceState::SavedWithReplay &&
             retry.events ==
                 std::vector<std::string>({"load-result", "stage-file",
                                           "load-pending", "project", "ack"}),
         "exact retry trusts the durable reference and performs no file write");
}

void testReplayFailuresPreserveIndependentResult() {
  {
    Harness harness;
    harness.encodeSucceeds = false;
    ChartReplayPersistence persistence(harness.dependencies());
    const auto saved = persistence.persist(harness.attempt);
    expect(saved.state == ChartReplayPersistenceState::SavedWithoutReplay &&
               saved.durable() && !saved.replayAttached &&
               harness.events ==
                   std::vector<std::string>(
                       {"load-result", "reserve-path", "encode", "release-path",
                        "stage-summary", "load-pending", "project", "ack"}),
           "encode failure releases its reservation and saves summary facts");
  }
  {
    Harness harness;
    harness.installed = {.state = ReplayInstallState::Failed,
                         .diagnostic = "temporary write failed"};
    harness.inspected = {.state = ReplayFileState::Missing};
    ChartReplayPersistence persistence(harness.dependencies());
    const auto saved = persistence.persist(harness.attempt);
    expect(
        saved.state == ChartReplayPersistenceState::SavedWithoutReplay &&
            std::ranges::find(harness.events, "release-path") !=
                harness.events.end(),
        "definitely absent file failure releases ownership and saves result");
  }
  {
    Harness harness;
    harness.attempt.replay->playback.setup.chart.sha256 = repeated('d', 64);
    ChartReplayPersistence persistence(harness.dependencies());
    const auto saved = persistence.persist(harness.attempt);
    expect(saved.state == ChartReplayPersistenceState::SavedWithoutReplay &&
               harness.events ==
                   std::vector<std::string>({"load-result", "stage-summary",
                                             "load-pending", "project", "ack"}),
           "mismatched replay disables only replay attachment");
  }
  {
    Harness harness;
    harness.attempt.replay.reset();
    ChartReplayPersistence persistence(harness.dependencies());
    const auto saved = persistence.persist(harness.attempt);
    expect(saved.state == ChartReplayPersistenceState::SavedWithoutReplay &&
               saved.receipt,
           "absent capture still stores the modern result and IR snapshot");
  }
}

void testAmbiguousInstallOccupiedSlotAndCleanupBoundaries() {
  {
    Harness harness;
    harness.installed = {.state = ReplayInstallState::RetryableAmbiguous,
                         .diagnostic = "lost acknowledgement"};
    harness.inspected = {.state = ReplayFileState::Available};
    ChartReplayPersistence persistence(harness.dependencies());
    const auto saved = persistence.persist(harness.attempt);
    expect(saved.state == ChartReplayPersistenceState::SavedWithReplay &&
               saved.replayAttached,
           "verified bytes reconcile a lost install acknowledgement");
  }
  {
    Harness harness;
    int installs = 0;
    auto dependencies = harness.dependencies();
    const auto baseInstall = dependencies.fileAssociation.installFile;
    dependencies.fileAssociation.installFile =
        [&, baseInstall](const auto &reservation, auto bytes) {
      if (installs++ == 0) {
        harness.events.emplace_back("install");
        return ReplayInstallOutcome{.state = ReplayInstallState::Occupied,
                                    .diagnostic = "occupied"};
      }
      return baseInstall(reservation, bytes);
    };
    ChartReplayPersistence persistence(std::move(dependencies));
    const auto saved = persistence.persist(harness.attempt);
    expect(saved.state == ChartReplayPersistenceState::SavedWithReplay &&
               saved.replayAttached && installs == 2 &&
               std::ranges::count(harness.events, "reserve-path") == 2 &&
               std::ranges::count(harness.events, "release-path") == 1,
           "occupied Beatoraja slot advances to a new durable history index");
  }
  {
    Harness harness;
    harness.staged = {.status = ModernChartStageStatus::StorageFailure,
                      .diagnostic = "commit acknowledgement lost"};
    ChartReplayPersistence persistence(harness.dependencies());
    const auto pending = persistence.persist(harness.attempt);
    expect(pending.state == ChartReplayPersistenceState::Retryable &&
               !pending.durable() &&
               std::ranges::find(harness.events, "cleanup") ==
                   harness.events.end() &&
               std::ranges::find(harness.events, "release-path") ==
                   harness.events.end(),
           "ambiguous database stage retains installed ownership for retry");
  }
  {
    Harness harness;
    harness.staged = {.status = ModernChartStageStatus::Invalid,
                      .diagnostic = "invalid stage"};
    ChartReplayPersistence persistence(harness.dependencies());
    const auto invalid = persistence.persist(harness.attempt);
    expect(invalid.state == ChartReplayPersistenceState::InvalidAttempt &&
               std::ranges::find(harness.events, "cleanup") !=
                   harness.events.end() &&
               std::ranges::find(harness.events, "release-path") !=
                   harness.events.end(),
           "definitive stage rejection cleans only exact installed bytes");
  }
  {
    Harness harness;
    harness.installed.existingIdenticalFile = true;
    harness.staged = {.status = ModernChartStageStatus::Invalid,
                      .diagnostic = "invalid stage"};
    ChartReplayPersistence persistence(harness.dependencies());
    const auto invalid = persistence.persist(harness.attempt);
    expect(invalid.state == ChartReplayPersistenceState::InvalidAttempt &&
               std::ranges::find(harness.events, "cleanup") ==
                   harness.events.end() &&
               std::ranges::find(harness.events, "release-path") !=
                   harness.events.end(),
           "definitive rejection releases the reservation without deleting "
           "an identical pre-existing BRD");
  }
}

void testProjectionAcknowledgementAndRecoveryStates() {
  {
    Harness harness;
    harness.staged.receipt->attemptId = "123e4567-e89b-42d3-a456-426614174099";
    ChartReplayPersistence persistence(harness.dependencies());
    const auto conflict = persistence.persist(harness.attempt);
    expect(conflict.state == ChartReplayPersistenceState::IntegrityConflict &&
               !conflict.durable() && !conflict.receipt,
           "an inconsistent success receipt never proves durability");
  }
  {
    Harness harness;
    harness.projected = {
        .status = result_persistence::ProjectionStatus::StorageFailure,
        .diagnostic = "score unavailable"};
    ChartReplayPersistence persistence(harness.dependencies());
    const auto pending = persistence.persist(harness.attempt);
    expect(pending.state == ChartReplayPersistenceState::PendingScore &&
               pending.durable() && pending.retryable() && pending.receipt,
           "score failure preserves the durable result for recovery");
  }
  {
    Harness harness;
    harness.acknowledged = {
        .status = result_persistence::AcknowledgeStatus::StorageFailure,
        .diagnostic = "ack unavailable"};
    ChartReplayPersistence persistence(harness.dependencies());
    const auto pending = persistence.persist(harness.attempt);
    expect(pending.state ==
                   ChartReplayPersistenceState::PendingAcknowledgement &&
               pending.durable() && pending.retryable(),
           "acknowledgement failure is distinct from projection failure");
  }
  {
    Harness harness;
    harness.batch = {
        .storageAvailable = true,
        .entries = {{.status = result_persistence::PendingReadStatus::Found,
                     .attemptId = harness.attempt.result.attemptId,
                     .value = harness.pending.value}}};
    ChartReplayPersistence persistence(harness.dependencies());
    const auto recovered = persistence.recoverAll();
    expect(recovered.attempted == 1 && recovered.saved == 1 &&
               recovered.pending == 0 && recovered.conflicts == 0,
           "recovery uses the same projection and acknowledgement authority");
  }
  {
    Harness harness;
    harness.batch = {
        .storageAvailable = true,
        .entries = {
            {.status = result_persistence::PendingReadStatus::StorageFailure,
             .attemptId = harness.attempt.result.attemptId,
             .diagnostic = "temporary read failure"}}};
    ChartReplayPersistence persistence(harness.dependencies());
    const auto recovered = persistence.recoverAll();
    expect(recovered.attempted == 1 && recovered.saved == 0 &&
               recovered.pending == 1 && recovered.conflicts == 0,
           "modern recovery retains storage failures as pending work");
  }
}

void testInvalidSummaryInputsHaveNoSideEffects() {
  {
    Harness harness;
    harness.attempt.result.resultFingerprint = repeated('0', 64);
    ChartReplayPersistence persistence(harness.dependencies());
    const auto invalid = persistence.persist(harness.attempt);
    expect(invalid.state == ChartReplayPersistenceState::InvalidAttempt &&
               harness.events.empty(),
           "invalid result stops before file or database effects");
  }
  {
    Harness harness;
    harness.attempt.irSnapshot->fingerprint = repeated('0', 64);
    ChartReplayPersistence persistence(harness.dependencies());
    const auto invalid = persistence.persist(harness.attempt);
    expect(invalid.state == ChartReplayPersistenceState::InvalidAttempt &&
               harness.events.empty(),
           "snapshot disagreement stops before file or database effects");
  }
}

void writeBytes(const std::filesystem::path &path, std::string_view bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void testRealRepositoriesPersistBrdAndAdvanceOccupiedSlot() {
  TemporaryDirectory profile;
  ReplayRepository repository(profile.path / "replays.db");
  ScoreRepository score(profile.path / "scores.db");
  expect(repository.EnsureSchema() && score.EnsureSchema(),
         "integration repositories initialize");
  ChartReplayPersistence persistence(score, repository);
  const auto attempt = validAttempt();
  const auto saved = persistence.persist(attempt);
  expect(saved.state == ChartReplayPersistenceState::SavedWithReplay &&
             saved.receipt && saved.replayAttached,
         "real persistence stores result, score, and BRD");
  const auto loaded =
      repository.LoadModernChartResultByAttempt(attempt.result.attemptId);
  expect(loaded.status == ModernChartResultReadStatus::Loaded &&
             loaded.record && loaded.record->replayFile &&
             std::filesystem::is_regular_file(
                 profile.path /
                 loaded.record->replayFile->metadata.relativePath) &&
             repository.LoadPendingModernChartScore(attempt.result.attemptId)
                     .status == result_persistence::PendingReadStatus::NotFound,
         "real result owns a contained BRD and no acknowledged pending row");
  const auto retry = persistence.persist(attempt);
  expect(retry.state == ChartReplayPersistenceState::SavedWithReplay &&
             retry.receipt == saved.receipt,
         "real exact retry reconciles to the same result and file");

  auto occupiedAttempt = validAttempt();
  occupiedAttempt.result.attemptId = "123e4567-e89b-42d3-a456-426614174001";
  occupiedAttempt.result.playedAtUnixMillis += 1;
  occupiedAttempt.result.resultFingerprint =
      result_persistence::modernResultFingerprint(occupiedAttempt.result);
  occupiedAttempt.replay = replayDocument(occupiedAttempt.result);
  std::string diagnostic;
  occupiedAttempt.irSnapshot =
      ir::captureIrSubmissionSnapshot(occupiedAttempt.result, diagnostic);
  const auto firstStem =
      chartStem(occupiedAttempt.result.score.chartSha256,
                occupiedAttempt.result.score.longNoteMode, false, diagnostic);
  const auto occupiedPath = pathForStem(*firstStem, 1, diagnostic);
  std::filesystem::create_directories(profile.path / "replay");
  writeBytes(profile.path / occupiedPath->relativePath, "user brd");
  const auto occupiedSaved = persistence.persist(occupiedAttempt);
  const auto occupiedLoaded = repository.LoadModernChartResultByAttempt(
      occupiedAttempt.result.attemptId);
  expect(occupiedSaved.state == ChartReplayPersistenceState::SavedWithReplay &&
             occupiedLoaded.record && occupiedLoaded.record->replayFile &&
             occupiedLoaded.record->replayFile->identity.historyIndex == 2,
         "physical collision advances without overwriting the occupied slot");
  std::ifstream original(profile.path / occupiedPath->relativePath,
                         std::ios::binary);
  const std::string contents(std::istreambuf_iterator<char>(original), {});
  expect(contents == "user brd", "occupied user BRD remains unchanged");
}

} // namespace

int main() {
  testCompletionCaptureBuildsIndependentResultSnapshotAndReplay();
  testFileFirstSuccessAndExactRetry();
  testReplayFailuresPreserveIndependentResult();
  testAmbiguousInstallOccupiedSlotAndCleanupBoundaries();
  testProjectionAcknowledgementAndRecoveryStates();
  testInvalidSummaryInputsHaveNoSideEffects();
  testRealRepositoriesPersistBrdAndAdvanceOccupiedSlot();
  if (failures != 0) {
    std::cerr << failures << " chart replay persistence test(s) failed\n";
    return 1;
  }
  std::cout << "chart replay persistence tests passed\n";
  return 0;
}
