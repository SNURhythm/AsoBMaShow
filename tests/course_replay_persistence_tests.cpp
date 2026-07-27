#include "replay/CourseReplayPersistence.h"
#include "replay/CourseResultPersistence.h"
#include "replay/ReplayFileStore.h"
#include "replay/ReplaySetupProvenance.h"
#include "repositories/SqliteRAII.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
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
           ("asobmashow-course-replay-persistence-" +
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

int queryInt(const std::filesystem::path &databasePath,
             const std::string &query) {
  sqlite3 *raw = nullptr;
  expect(sqlite3_open(databasePath.string().c_str(), &raw) == SQLITE_OK,
         "test database opens");
  SqliteConnectionHandle database(raw);
  SqliteStatementHandle statement;
  expect(prepareSqliteStatement(database.get(), query, statement) == SQLITE_OK,
         "test query prepares");
  expect(sqlite3_step(statement.get()) == SQLITE_ROW, "test query returns");
  return sqlite3_column_int(statement.get(), 0);
}

void execute(const std::filesystem::path &databasePath,
             const std::string &query) {
  sqlite3 *raw = nullptr;
  expect(sqlite3_open(databasePath.string().c_str(), &raw) == SQLITE_OK,
         "test database opens for mutation");
  SqliteConnectionHandle database(raw);
  expect(sqlite3_exec(database.get(), query.c_str(), nullptr, nullptr,
                      nullptr) == SQLITE_OK,
         "test mutation succeeds");
}

ScoreProvenance provenance(char hash, int keyMode) {
  ScoreProvenanceBuildInput input;
  input.chartMeta.MD5 = repeated(hash, 32);
  input.chartMeta.SHA256 = repeated(hash, 64);
  input.chartMeta.KeyMode = keyMode;
  input.chartMeta.Rank = 2;
  input.chartMeta.TotalNotes = 5;
  input.chartMeta.HasTotal = true;
  input.chartMeta.Total = 200.0;
  input.longNoteMode = 1;
  input.sourceJudgeRank = 2;
  input.effectiveJudgeWindows = {
      {PGreat, {-10'000, 10'000}}, {Great, {-30'000, 30'000}},
      {Good, {-75'000, 75'000}},   {Bad, {-200'000, 200'000}},
      {Kpoor, {-1'000'000, 0}},
  };
  input.totalNotes = 5;
  input.authoredGaugeTotal = 200.0;
  input.effectiveGaugeTotal = 200.0;
  input.gaugeType = GaugeType::Hard;
  input.inputDevices = {InputDeviceCategory::Keyboard};
  return makeScoreProvenance(input);
}

result_persistence::ModernCourseStageResult
stage(int index, char hash, int keyMode, int maximumCombo, float gauge) {
  result_persistence::ModernCourseStageResult value;
  value.stageIndex = index;
  value.score.chartPath = "library/stage.bms";
  value.score.chartMd5 = repeated(hash, 32);
  value.score.chartSha256 = repeated(hash, 64);
  value.score.chartTitle = "Stage";
  value.score.chartArtist = "Artist";
  value.score.longNoteMode = 1;
  value.score.score = 7;
  value.score.maxScore = 10;
  value.score.maxCombo = maximumCombo;
  value.score.comboBreak = 1;
  value.score.pGreat = 3;
  value.score.great = 1;
  value.score.good = 1;
  value.score.finalGauge = gauge;
  value.score.clearType = kClearTypeHardClearRank;
  value.score.provenance = provenance(hash, keyMode);
  value.keyMode = keyMode;
  value.adoptedGaugeType = GaugeType::Hard;
  value.adoptedGaugeHistory = {80.0F, gauge};
  return value;
}

CapturedCourseReplayAttempt attempt(bool withReplay = true,
                                    bool complete = false) {
  result_persistence::ModernCourseResultCapture capture{
      .attemptId = "123e4567-e89b-42d3-a456-426614174000",
      .courseKey = "course:v1:" + repeated('c', 64),
      .legacyCourseId = 17,
      .courseName = "Persistence Course",
      .courseGroupName = "Tests",
      .constraintJson = "[\"CLASS\"]",
      .initialGaugeType = GaugeType::Hard,
      .gaugeProfile = GaugeProfile::Standard,
      .gaugeAutoShift = GaugeAutoShiftMode::None,
      .gaugeAutoShiftLowerBound = GaugeType::AssistedEasy,
      .longNoteMode = 1,
      .clearType = kClearTypeHardClearRank,
      .stages = {stage(0, 'a', 7, 4, 76.0F), stage(1, 'b', 14, 8, 62.5F)},
      .entryFacts = {{.totalNotes = 5, .playLengthMicros = 1'000'000},
                     {.totalNotes = 5, .playLengthMicros = 2'000'000},
                     {.totalNotes = 5, .playLengthMicros = 3'000'000}},
      .playedAtUnixMillis = 1'700'000'100'000LL,
  };
  if (complete) {
    capture.entryFacts.resize(2);
  }
  std::string diagnostic;
  auto result =
      result_persistence::captureModernCourseResult(capture, diagnostic);
  expect(result.has_value(), "course result fixture captures");
  CapturedCourseReplayAttempt value{
      .result = *result,
      .pathInput = {.stageSha256 = {repeated('a', 64), repeated('b', 64)},
                    .longNoteMode = 1,
                    .beatorajaConstraintIds = {4}},
  };
  if (!withReplay) {
    return value;
  }
  ReplayCourseDocument document;
  for (const auto &saved : value.result.stages) {
    LocalReplaySetupFacts facts{
        .chart = {.md5 = saved.score.chartMd5,
                  .sha256 = saved.score.chartSha256,
                  .keyMode = saved.keyMode},
        .longNoteMode = saved.score.longNoteMode,
    };
    auto setup =
        captureLocalReplaySetup(facts, saved.score.provenance, diagnostic);
    expect(setup.has_value(), "course stage setup fixture captures");
    ReplayPlaybackData playback;
    playback.setup = *setup;
    playback.input = {
        {.songTimeMicros = 0,
         .control = {.kind = LogicalControlKind::Lane, .player = 1, .lane = 0},
         .pressed = true},
        {.songTimeMicros = 1,
         .control = {.kind = LogicalControlKind::Lane, .player = 1, .lane = 0},
         .pressed = false},
    };
    document.playback.stages.push_back(std::move(playback));
    document.playback.restMicrosAfterStage.push_back(0);
    document.timeBounds.push_back({.completionSongTimeMicros = 5'000'000});
  }
  value.replay = std::move(document);
  return value;
}

struct Harness {
  CapturedCourseReplayAttempt value = attempt();
  std::vector<std::string> events;
  ModernCourseResultReadOutcome existing{
      .status = ModernCourseResultReadStatus::NotFound};
  ModernCourseStageOutcome staged{
      .status = ModernCourseStageStatus::Staged,
      .receipt = ModernCourseStageReceipt{.attemptId = value.result.attemptId,
          .resultId = 9,
          .createdAt = "2026-07-27 12:00:00"}};
  bool encodeSucceeds = true;
  ReplayInstallOutcome installed;

  Harness() {
    std::string diagnostic;
    const auto stem = courseStem(value.pathInput, diagnostic);
    const auto identity = pathForStem(*stem, 0, diagnostic);
    const ReplayFileMetadata metadata{
        .relativePath = identity->relativePath,
        .sha256 = repeated('d', 64),
        .compressedSize = 1,
        .codecVersion = BeatorajaReplayCodec::kCodecVersion,
    };
    installed = {
        .state = ReplayInstallState::InstalledVerified,
        .file =
            ReplayInstalledFile{
            .metadata = metadata,
            .attemptToken = value.result.attemptId,
            .lifecycle =
                {.state = ReplayFileLifecycleState::InstalledUnassociated,
                 .attemptToken = value.result.attemptId,
                     .receipt =
                         ReplayFileOwnershipReceipt{.attemptToken =
                                                        value.result.attemptId,
                     .metadata = metadata}}},
    };
  }

  CourseReplayPersistenceDependencies dependencies() {
    return {
        .loadResult =
            [this](std::string_view) {
              events.emplace_back("load-result");
              return existing;
            },
        .encode = [this](const ReplayCourseDocument &, std::int64_t,
                   std::string &diagnostic)
                -> std::optional<std::vector<std::byte>> {
              events.emplace_back("encode");
              if (!encodeSucceeds) {
                diagnostic = "encode failed";
                return std::nullopt;
              }
              return std::vector<std::byte>{std::byte{0x42}};
            },
        .fileAssociation =
            {.reservePath =
                 [this](std::string_view attemptId, std::string_view stem,
                        std::int64_t playedAt) {
                   events.emplace_back("reserve-path");
                   std::string diagnostic;
                   const auto identity = pathForStem(stem, 0, diagnostic);
                   return ModernReplayReservationOutcome{
                       .status = ModernReplayReservationStatus::Reserved,
                       .reservation = ModernReplayPathReservation{
                           .attemptId = std::string(attemptId),
                           .identity = *identity,
                           .createdAtUnixMillis = playedAt}};
                 },
             .releasePath =
                 [this](const ModernReplayPathReservation &) {
                   events.emplace_back("release-path");
                   return ModernReplayReservationReleaseOutcome{
                       .status =
                           ModernReplayReservationReleaseStatus::Released};
                 },
             .reserveFile =
                 [this](const ReplayPathIdentity &identity,
                        std::span<const std::byte>, std::string_view token) {
                   events.emplace_back("reserve-file");
                   return ReplayReservationOutcome{
                       .reservation = ReplayFileReservation{
                           .identity = identity,
                           .attemptToken = std::string(token),
                           .expectedMetadata =
                               {.relativePath = identity.relativePath,
                                .sha256 = repeated('d', 64),
                                .compressedSize = 1,
                                .codecVersion =
                                    BeatorajaReplayCodec::kCodecVersion},
                           .temporaryRelativePath = "replay/private.tmp"}};
                 },
             .installFile =
                 [this](const ReplayFileReservation &reservation,
                        std::span<const std::byte>) {
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
             .inspectFile =
                 [this](const ReplayFileMetadata &) {
                   events.emplace_back("inspect");
                   return ReplayFileInspection{.state =
                                                   ReplayFileState::Missing};
                 },
             .removeIfMatches =
                 [this](const ReplayFileMetadata &, std::string &) {
                   events.emplace_back("cleanup");
                   return true;
                 }},
        .stage =
            [this](const result_persistence::ModernCourseResult &,
                   const std::optional<ModernReplayFileAttachment> &file,
                   const std::optional<CoursePathInput> &) {
              events.emplace_back(file ? "stage-file" : "stage-summary");
              return staged;
            },
    };
  }
};

void testReplayBackedSummaryOnlyAndExactRetry() {
  Harness harness;
  CourseReplayPersistence persistence(harness.dependencies());
  const auto saved = persistence.persist(harness.value);
  expect(saved.state == CourseReplayPersistenceState::SavedWithReplay &&
             saved.saved() && saved.replayAttached && saved.receipt &&
             harness.events == std::vector<std::string>(
                                   {"load-result", "reserve-path", "encode",
                                    "reserve-file", "install", "stage-file"}),
         "course persistence associates one BRD with its strict result");

  Harness replayless;
  replayless.value.replay.reset();
  CourseReplayPersistence noReplay(replayless.dependencies());
  const auto summary = noReplay.persist(replayless.value);
  expect(summary.state == CourseReplayPersistenceState::SavedWithoutReplay &&
             summary.receipt &&
             replayless.events ==
                 std::vector<std::string>({"load-result", "stage-summary"}),
         "missing raw capture saves the course result without a file");

  Harness retry;
  auto stored = retry.value.result;
  stored.resultId = 9;
  std::string diagnostic;
  const auto stem = courseStem(retry.value.pathInput, diagnostic);
  const auto identity = pathForStem(*stem, 0, diagnostic);
  retry.existing = {
      .status = ModernCourseResultReadStatus::Loaded,
      .record = ModernCourseResultRecord{
          .result = stored,
          .replayFile = ModernReplayFileReference{
              .id = 1,
              .resultId = 9,
              .identity = *identity,
              .metadata = {.relativePath = identity->relativePath,
                           .sha256 = repeated('d', 64),
                           .compressedSize = 1,
                           .codecVersion =
                               BeatorajaReplayCodec::kCodecVersion}}}};
  retry.staged.status = ModernCourseStageStatus::AlreadyStaged;
  CourseReplayPersistence exact(retry.dependencies());
  const auto repeated = exact.persist(retry.value);
  expect(repeated.state == CourseReplayPersistenceState::SavedWithReplay &&
             retry.events ==
                 std::vector<std::string>({"load-result", "stage-file"}),
         "exact course retry reuses durable ownership without rewriting bytes");
}

void testReplayFailuresAndDatabaseAmbiguity() {
  Harness encodeFailure;
  encodeFailure.encodeSucceeds = false;
  CourseReplayPersistence persistence(encodeFailure.dependencies());
  const auto summary = persistence.persist(encodeFailure.value);
  expect(summary.state == CourseReplayPersistenceState::SavedWithoutReplay &&
             std::ranges::find(encodeFailure.events, "release-path") !=
                 encodeFailure.events.end() &&
             encodeFailure.events.back() == "stage-summary",
         "course encode failure drops only the replay attachment");

  Harness mismatch;
  mismatch.value.replay->playback.stages[1].setup.chart.sha256 =
      repeated('e', 64);
  CourseReplayPersistence invalidReplay(mismatch.dependencies());
  const auto saved = invalidReplay.persist(mismatch.value);
  expect(saved.state == CourseReplayPersistenceState::SavedWithoutReplay &&
             mismatch.events ==
                 std::vector<std::string>({"load-result", "stage-summary"}),
         "course stage identity mismatch cannot block result history");

  Harness ambiguous;
  ambiguous.staged = {.status = ModernCourseStageStatus::StorageFailure,
                      .diagnostic = "commit acknowledgement lost"};
  CourseReplayPersistence uncertain(ambiguous.dependencies());
  const auto pending = uncertain.persist(ambiguous.value);
  expect(pending.state == CourseReplayPersistenceState::Retryable &&
             std::ranges::find(ambiguous.events, "cleanup") ==
                 ambiguous.events.end() &&
             std::ranges::find(ambiguous.events, "release-path") ==
                 ambiguous.events.end(),
         "ambiguous database result retains installed ownership for retry");

  Harness rejected;
  rejected.staged = {.status = ModernCourseStageStatus::Invalid,
                     .diagnostic = "definitive rejection"};
  CourseReplayPersistence definitive(rejected.dependencies());
  const auto invalid = definitive.persist(rejected.value);
  expect(invalid.state == CourseReplayPersistenceState::InvalidAttempt &&
             std::ranges::find(rejected.events, "cleanup") !=
                 rejected.events.end() &&
             std::ranges::find(rejected.events, "release-path") !=
                 rejected.events.end(),
         "definitive course rejection cleans only exact attempt bytes");
}

void testRealRepositoryPersistsPartialCourseBrdWithoutLegacyRows() {
  TemporaryDirectory profile;
  const auto databasePath = profile.path / "replay.db";
  ReplayRepository repository(databasePath);
  expect(repository.EnsureSchema(),
         "course integration repository initializes");
  CourseReplayPersistence persistence(repository);
  const auto value = attempt();
  const auto saved = persistence.persist(value);
  expect(saved.state == CourseReplayPersistenceState::SavedWithReplay &&
             saved.receipt && saved.replayAttached,
         "real course persistence saves a partial result and BRD");

  const auto loaded =
      repository.LoadModernCourseResultByAttempt(value.result.attemptId);
  expect(loaded.status == ModernCourseResultReadStatus::Loaded &&
             loaded.record && loaded.record->result.completedCharts == 2 &&
             loaded.record->result.totalCharts == 3 &&
             loaded.record->replayFile,
         "partial result facts and file ownership load independently");
  if (loaded.record && loaded.record->replayFile) {
    ReplayFileStore store(profile.path);
    const auto bytes = store.readVerified(loaded.record->replayFile->metadata);
    expect(bytes.state == ReplayFileState::Available && bytes.bytes,
           "installed course BRD verifies by saved metadata");
    if (bytes.bytes) {
      BeatorajaReplayCodec codec;
      ReplayDecodeContext context{
          .stageKeyModes = {7, 14},
          .stageTimeBounds = value.replay->timeBounds,
      };
      const auto decoded = codec.decode(*bytes.bytes, context);
      expect(decoded.course && *decoded.course == *value.replay,
             "installed course BRD round-trips its ordered completed prefix");
    }
  }

  for (const std::string_view table :
       {"replay_events", "replay_touch_samples", "replay_lane_cover_events",
                                       "course_replay_stages"}) {
    expect(queryInt(databasePath,
                    "SELECT COUNT(*) FROM " + std::string(table)) == 0,
           "modern course persistence writes no legacy raw rows");
  }
  const auto retried = persistence.persist(value);
  expect(retried.state == CourseReplayPersistenceState::SavedWithReplay &&
             retried.receipt == saved.receipt,
         "real exact course retry reuses the same result and BRD ownership");
}

void testResultFirstCoordinatorProjectsScoreOnlyAfterDurableResult() {
  const auto value = attempt();
  int resultCalls = 0;
  int projectionCalls = 0;
  CourseResultPersistence coordinator(CourseResultPersistenceDependencies{
      .persistResult =
          [&](const CapturedCourseReplayAttempt &received) {
            ++resultCalls;
            expect(received.result == value.result,
                   "result-first coordinator preserves captured facts");
            return CourseReplayPersistenceOutcome{
                .state = CourseReplayPersistenceState::SavedWithReplay,
                .receipt =
                    ModernCourseStageReceipt{
                    .attemptId = value.result.attemptId,
                    .resultId = 91,
                    .createdAt = "2026-07-27 13:00:00"},
                .replayAttached = true};
          },
      .projectScore =
          [&](const result_persistence::PendingCourseScoreWrite &pending) {
            ++projectionCalls;
            expect(pending.attemptId == value.result.attemptId &&
                       pending.modernResultId == 91 &&
                       pending.createdAt == "2026-07-27 13:00:00" &&
                       pending.result == value.result,
                   "score projection receives exact result/receipt agreement");
            return result_persistence::ProjectionOutcome{
                .status = result_persistence::ProjectionStatus::Inserted};
          },
  });

  const auto saved = coordinator.persist(value);
  expect(saved.state == CourseResultPersistenceState::SavedWithReplay &&
             saved.saved() && resultCalls == 1 && projectionCalls == 1,
         "one course completion invokes one result-first modern path");

  CourseResultPersistence unavailable(CourseResultPersistenceDependencies{
      .persistResult =
          [&](const CapturedCourseReplayAttempt &) {
            ++resultCalls;
            return CourseReplayPersistenceOutcome{
                .state = CourseReplayPersistenceState::Retryable,
                .diagnostic = "database unavailable"};
          },
      .projectScore =
          [&](const result_persistence::PendingCourseScoreWrite &) {
            ++projectionCalls;
            return result_persistence::ProjectionOutcome{
                .status = result_persistence::ProjectionStatus::Inserted};
          },
  });
  const auto pending = unavailable.persist(value);
  expect(pending.state == CourseResultPersistenceState::Retryable &&
             projectionCalls == 1,
         "score projection never runs before a durable modern result");

  int pendingProjectionCalls = 0;
  CourseResultPersistence pendingScore(CourseResultPersistenceDependencies{
      .persistResult =
          [&](const CapturedCourseReplayAttempt &) {
            return CourseReplayPersistenceOutcome{
                .state = CourseReplayPersistenceState::SavedWithoutReplay,
                .receipt = ModernCourseStageReceipt{
                    .attemptId = value.result.attemptId,
                    .resultId = 91,
                    .createdAt = "2026-07-27 13:00:00"}};
          },
      .projectScore =
          [&](const result_persistence::PendingCourseScoreWrite &) {
            ++pendingProjectionCalls;
            return result_persistence::ProjectionOutcome{
                .status =
                    pendingProjectionCalls == 1
                        ? result_persistence::ProjectionStatus::StorageFailure
                        : result_persistence::ProjectionStatus::AlreadyPresent};
          },
  });
  expect(pendingScore.persist(value).state ==
                 CourseResultPersistenceState::PendingScore &&
             pendingScore.persist(value).state ==
                 CourseResultPersistenceState::SavedWithoutReplay &&
             pendingProjectionCalls == 2,
         "retryable score projection reuses the exact durable course result");
}

void testRealResultFirstPersistenceIsIdempotentAndReplayIndependent() {
  TemporaryDirectory profile;
  const auto replayPath = profile.path / "replay.db";
  const auto scorePath = profile.path / "score.db";
  ReplayRepository replayRepository(replayPath);
  ScoreRepository scoreRepository(scorePath);
  expect(replayRepository.EnsureSchema() && scoreRepository.EnsureSchema(),
         "result-first integration repositories initialize");

  CourseResultPersistence persistence(scoreRepository, replayRepository);
  const auto partial = attempt();
  const auto saved = persistence.persist(partial);
  expect(saved.state == CourseResultPersistenceState::SavedWithReplay &&
             saved.saved() && saved.receipt,
         "failed-partial course saves result, score projection, and BRD");
  expect(queryInt(replayPath, "SELECT COUNT(*) FROM modern_course_results") ==
                 1 &&
             queryInt(scorePath, "SELECT COUNT(*) FROM course_scores") == 1,
         "one final course result creates one modern result and score row");
  expect(queryInt(scorePath,
                  "SELECT COUNT(*) FROM course_scores WHERE attempt_id = '" +
                      partial.result.attemptId +
                      "' AND modern_result_id > 0") == 1,
         "course score row retains exact attempt/result ownership");
  for (const std::string_view table :
       {"replay_events", "replay_touch_samples", "replay_lane_cover_events",
                                       "course_replay_stages"}) {
    expect(queryInt(replayPath, "SELECT COUNT(*) FROM " + std::string(table)) ==
               0,
           "result-first course persistence writes no legacy raw rows");
  }

  const auto retried = persistence.persist(partial);
  expect(retried.state == CourseResultPersistenceState::SavedWithReplay &&
             retried.receipt == saved.receipt &&
             queryInt(replayPath,
                      "SELECT COUNT(*) FROM modern_course_results") == 1 &&
             queryInt(scorePath, "SELECT COUNT(*) FROM course_scores") == 1,
         "exact final-result retry is idempotent across both databases");

  execute(scorePath, "UPDATE course_scores SET score = score + 1");
  const auto conflictingProjection = persistence.persist(partial);
  expect(conflictingProjection.state ==
             CourseResultPersistenceState::IntegrityConflict,
         "exact retry rejects a score row that disagrees with modern facts");

  TemporaryDirectory completeProfile;
  ReplayRepository completeReplay(completeProfile.path / "replay.db");
  ScoreRepository completeScores(completeProfile.path / "score.db");
  expect(completeReplay.EnsureSchema() && completeScores.EnsureSchema(),
         "complete-course integration repositories initialize");
  CourseResultPersistence completePersistence(completeScores, completeReplay);
  const auto complete = completePersistence.persist(attempt(true, true));
  expect(complete.state == CourseResultPersistenceState::SavedWithReplay &&
             complete.saved() &&
             queryInt(completeProfile.path / "score.db",
                      "SELECT COUNT(*) FROM course_scores") == 1 &&
             queryInt(completeProfile.path / "replay.db",
                      "SELECT COUNT(*) FROM modern_replay_files") == 1,
         "complete course saves its modern result, score, and one BRD");

  TemporaryDirectory summaryProfile;
  ReplayRepository summaryReplay(summaryProfile.path / "replay.db");
  ScoreRepository summaryScores(summaryProfile.path / "score.db");
  expect(summaryReplay.EnsureSchema() && summaryScores.EnsureSchema(),
         "summary-only integration repositories initialize");
  CourseResultPersistence summaryPersistence(summaryScores, summaryReplay);
  const auto summary = summaryPersistence.persist(attempt(false, true));
  expect(summary.state == CourseResultPersistenceState::SavedWithoutReplay &&
             summary.saved() &&
             queryInt(summaryProfile.path / "score.db",
                      "SELECT COUNT(*) FROM course_scores") == 1 &&
             queryInt(summaryProfile.path / "replay.db",
                      "SELECT COUNT(*) FROM modern_replay_files") == 0,
         "missing raw capture preserves complete result and score history "
         "without BRD");
}

} // namespace

int main() {
  testReplayBackedSummaryOnlyAndExactRetry();
  testReplayFailuresAndDatabaseAmbiguity();
  testRealRepositoryPersistsPartialCourseBrdWithoutLegacyRows();
  testResultFirstCoordinatorProjectsScoreOnlyAfterDurableResult();
  testRealResultFirstPersistenceIsIdempotentAndReplayIndependent();
  if (failures != 0) {
    std::cerr << failures << " course replay persistence test(s) failed\n";
    return 1;
  }
  std::cout << "course replay persistence tests passed\n";
  return 0;
}
