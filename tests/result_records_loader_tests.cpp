#include "scene/ResultRecordsLoader.h"

#include "repositories/ChartRepository.h"
#include "repositories/SqliteRAII.h"
#include "replay/ReplayFileStore.h"
#include "ReplayAutoPlay.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

struct Fixture {
  std::filesystem::path path = std::filesystem::temp_directory_path() /
      ("asobmashow-records-loader-" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()));
  ReplayRepository repository{path / "replay.db"};
  ChartMetaRecord chart;
  ResultRecordsLoadOptions options;

  Fixture() {
    assert(std::filesystem::create_directories(path));
    assert(repository.EnsureSchema());
    chart.meta.MD5 = std::string(32, 'b');
    chart.meta.SHA256 = std::string(64, 'a');
    options.irServerOrigin = "https://example.invalid";
  }
  ~Fixture() {
    repository.Shutdown();
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
  void sql(const std::string &sql) {
    sqlite3 *raw = nullptr;
    assert(sqlite3_open(repository.GetDatabasePath().string().c_str(), &raw) ==
           SQLITE_OK);
    SqliteConnectionHandle database(raw);
    char *error = nullptr;
    if (sqlite3_exec(raw, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
      std::cerr << (error ? error : "SQLite failure") << '\n';
      sqlite3_free(error);
      std::abort();
    }
  }
  ResultRecordsLoadOutcome load() {
    return loadResultRecords(repository, chart, options);
  }
};

result_persistence::ModernChartResult result(int suffix) {
  result_persistence::ModernChartResult value;
  value.attemptId = "123e4567-e89b-42d3-a456-42661417400" +
                    std::to_string(suffix);
  value.score.chartPath = "library/chart.bms";
  value.score.chartMd5 = std::string(32, 'b');
  value.score.chartSha256 = std::string(64, 'a');
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
  value.score.provenance = ScoreProvenance::Legacy();
  value.keyMode = 7;
  value.adoptedGaugeType = GaugeType::Normal;
  value.adoptedGaugeHistory = {20.0F, 82.5F};
  value.playedAtUnixMillis = 1'700'000'000'000LL + suffix;
  value.resultFingerprint = result_persistence::modernResultFingerprint(value);
  return value;
}

void stage(Fixture &fixture, int suffix) {
  assert(fixture.repository.StageModernChartResult(
      result(suffix), std::nullopt, std::nullopt).status ==
      ModernChartStageStatus::Staged);
}

void seedRemote(Fixture &fixture) {
  ir::IrRemoteScore remote{
      .remoteUserId = 42,
      .game = "bms-7k",
      .remoteScoreId = "remote-score",
      .remoteChartId = "remote-chart",
      .chartMd5 = fixture.chart.meta.MD5,
      .chartSha256 = fixture.chart.meta.SHA256,
      .title = "Remote title",
      .artist = "Remote artist",
      .service = "Bokutachi",
      .noteCount = 5,
      .score = 7,
      .lampRank = kClearTypeNormalClearRank,
      .timeAddedUnixMillis = 1'700'000'000'000LL,
  };
  assert(fixture.repository.ApplyIrRemoteSnapshot({
      .providerId = "tachi",
      .serverOrigin = *fixture.options.irServerOrigin,
      .synchronizedAtUnixMillis = 1'700'000'000'100LL,
      .scores = {remote},
  }).status == ir::IrRemoteSnapshotApplyOutcome::Status::Applied);
}

void testChartSourcesAndReplayAvailability() {
  Fixture fixture;
  fixture.options.autoPlay = ReplaySummary{};
  fixture.options.autoPlay->autoPlay = true;
  fixture.options.autoPlay->id = replay_autoplay::kReplayId;
  fixture.sql("INSERT INTO legacy_chart_result_summaries(legacy_replay_id,"
              "chart_sha256,final_score,partial) VALUES(91,'" +
              fixture.chart.meta.SHA256 + "',123,1)");
  stage(fixture, 1);
  seedRemote(fixture);
  fixture.options.irEnabled = true;
  const auto loaded = fixture.load();
  expect(loaded.complete && loaded.diagnostics.empty(),
         "healthy chart sources complete without diagnostics");
  expect(loaded.records.size() == 4,
         "chart records include supplied Auto Play, legacy, modern and remote");
  if (loaded.records.size() != 4) return;
  expect(loaded.records.front().autoPlay,
         "synthetic Auto Play remains first after merge");
  const auto modern = std::find_if(loaded.records.begin(), loaded.records.end(),
      [](const auto &row) { return row.isModernChart(); });
  expect(modern != loaded.records.end() && modern->score == 7 &&
             modern->replayState == replay::ReplayState::Missing &&
             modern->capabilities.resultRecall && !modern->capabilities.watch,
         "result-only modern rows retain recall but disable replay actions");
  fixture.options.irEnabled = false;
  fixture.options.autoPlay.reset();
  const auto local = fixture.load();
  expect(local.records.size() == 2 && local.complete,
         "disabled IR and absent Auto Play leave both local result sources");
}

void testOriginDiagnosticsAndRecovery() {
  Fixture fixture;
  stage(fixture, 1);
  fixture.options.irEnabled = true;
  fixture.options.irServerOrigin.reset();
  const auto failed = fixture.load();
  expect(!failed.complete && failed.records.size() == 1 &&
             failed.diagnostics.size() == 2,
         "invalid origin preserves local records and reports incomplete reads");
  if (failed.diagnostics.size() == 2) {
    expect(failed.diagnostics[0].source ==
               ResultRecordsDiagnostic::Source::ModernIr &&
               failed.diagnostics[1].source ==
               ResultRecordsDiagnostic::Source::RemoteHistory,
           "ordered source diagnostics preserve sequential log deduplication");
  }
  fixture.options.irServerOrigin = "https://example.invalid";
  const auto recovered = fixture.load();
  expect(recovered.complete && recovered.diagnostics.empty() &&
             recovered.records.size() == 1,
         "successful retry permits clearing the previous diagnostic");
}

void testInvalidRemoteHistoryKeepsLocalResults() {
  Fixture fixture;
  stage(fixture, 1);
  fixture.options.irEnabled = true;
  seedRemote(fixture);
  fixture.sql("UPDATE ir_remote_scores SET gauge_history_json='broken'");
  const auto loaded = fixture.load();
  expect(!loaded.complete && loaded.records.size() == 1 &&
             std::any_of(loaded.diagnostics.begin(), loaded.diagnostics.end(),
                 [](const auto &diagnostic) {
                   return diagnostic.source ==
                       ResultRecordsDiagnostic::Source::RemoteHistory &&
                       !diagnostic.message.empty();
                 }),
         "invalid remote history preserves modern results and its diagnostic");
}

void testLegacyHistoryIsNotReducedToDefaultPage() {
  Fixture fixture;
  fixture.sql("WITH RECURSIVE rows(n) AS (SELECT 1 UNION ALL SELECT n+1 "
              "FROM rows WHERE n<150) INSERT INTO legacy_chart_result_summaries"
              "(legacy_replay_id,chart_sha256,final_score,partial) SELECT n,'" +
              fixture.chart.meta.SHA256 + "',n,1 FROM rows");
  const auto loaded = fixture.load();
  expect(loaded.records.size() == 150 && loaded.complete,
         "Records requests full legacy history beyond repository default page");
}

void testCourseLookupUsesLegacyIdWithoutChartSources() {
  Fixture fixture;
  stage(fixture, 1);
  seedRemote(fixture);
  fixture.options.irEnabled = true;
  fixture.options.irServerOrigin.reset();
  fixture.options.autoPlay = ReplaySummary{};
  fixture.options.autoPlay->autoPlay = true;
  fixture.options.autoPlay->id = replay_autoplay::kReplayId;
  fixture.options.course = CourseReplayLookup{.legacyCourseId = 17};
  fixture.chart.courseStart = true;
  fixture.sql("INSERT INTO legacy_course_result_summaries("
              "legacy_course_replay_id,legacy_course_id,final_score,partial) "
              "VALUES(92,17,234,1),(93,18,345,1)");
  const auto loaded = fixture.load();
  expect(loaded.records.size() == 1 && loaded.complete &&
             loaded.diagnostics.empty(),
         "legacy course lookup skips chart sources and invalid IR origin");
  if (loaded.records.size() == 1) {
    expect(loaded.records[0].isLegacyCourse() && loaded.records[0].score == 234,
           "legacy course identity selects the requested course only");
  }
}


void testAttemptActivityAndReceiptLinkage() {
  Fixture fixture;
  const auto completed = result(1);
  std::string diagnostic;
  const auto snapshot = ir::captureIrSubmissionSnapshot(completed, diagnostic);
  assert(snapshot);
  const auto staged = fixture.repository.StageModernChartResult(
      completed, snapshot, std::nullopt);
  assert(staged.status == ModernChartStageStatus::Staged && staged.receipt);
  stage(fixture, 2);
  seedRemote(fixture);
  fixture.options.irEnabled = true;
  fixture.options.attemptActivity = [&](std::string_view attempt) {
    return attempt == completed.attemptId ? ir::IrRecordActivity::Submitting
                                         : ir::IrRecordActivity::None;
  };
  auto loaded = fixture.load();
  const auto active = std::find_if(loaded.records.begin(), loaded.records.end(),
      [&](const auto &row) { return row.modernAttemptId() == completed.attemptId; });
  expect(active != loaded.records.end() &&
             active->irState == ir::IrRecordState::Uploading,
         "live submission activity overlays only its matching local attempt");
  const auto remote = fixture.repository.ListIrRemoteScoresForChart(
      "tachi", *fixture.options.irServerOrigin,
      fixture.chart.meta.MD5, fixture.chart.meta.SHA256);
  assert(remote.status == ir::IrRemoteScoreReadOutcome::Status::Loaded);
  const ir::IrSubmissionReceipt receipt{
      .providerId = "tachi",
      .serverOrigin = *fixture.options.irServerOrigin,
      .modernChartResultId = staged.receipt->resultId,
      .attemptId = completed.attemptId,
      .chartMd5 = fixture.chart.meta.MD5,
      .chartSha256 = fixture.chart.meta.SHA256,
      .remoteUserId = 42,
      .remoteChartId = "remote-chart",
      .remoteScoreId = "remote-score",
      .source = ir::IrReceiptConfirmationSource::Snapshot,
      .observedInSnapshot = true,
      .confirmedAtUnixMillis = 1'700'000'000'200LL,
  };
  assert(fixture.repository.ApplyIrRemoteSnapshot({
      .providerId = "tachi",
      .serverOrigin = *fixture.options.irServerOrigin,
      .synchronizedAtUnixMillis = 1'700'000'000'200LL,
      .scores = remote.scores,
      .upsertedReceipts = {receipt},
  }).status == ir::IrRemoteSnapshotApplyOutcome::Status::Applied);
  loaded = fixture.load();
  expect(loaded.records.size() == 2 && loaded.complete,
         "exact receipt suppresses standalone remote while keeping all attempts");
  const auto linked = std::find_if(loaded.records.begin(), loaded.records.end(),
      [&](const auto &row) { return row.modernAttemptId() == completed.attemptId; });
  expect(linked != loaded.records.end() &&
             linked->irState == ir::IrRecordState::Uploaded &&
             linked->linkedRemote &&
             linked->linkedRemote->remoteScoreId == "remote-score",
         "durable receipt takes precedence over still-active submission state");
}

void testReplayFileProbeChangesCapabilities() {
  Fixture fixture;
  const auto completed = result(1);
  replay::ReplayFileStore store(fixture.path);
  const auto reserved = fixture.repository.ReserveModernReplayPath(
      completed.attemptId, completed.score.chartSha256,
      completed.playedAtUnixMillis);
  assert(reserved.reservation);
  const std::vector bytes{std::byte{0x1f}, std::byte{0x8b}, std::byte{0x08}};
  const auto reservation = store.reserve(reserved.reservation->identity, bytes,
                                          completed.attemptId);
  assert(reservation.reservation);
  const auto installed = store.install(*reservation.reservation, bytes);
  assert(installed.file);
  const ModernReplayFileAttachment attachment{
      .identity = reserved.reservation->identity,
      .metadata = installed.file->metadata,
  };
  assert(fixture.repository.StageModernChartResult(
      completed, std::nullopt, attachment).status == ModernChartStageStatus::Staged);
  auto loaded = fixture.load();
  expect(loaded.records.size() == 1 &&
             loaded.records[0].replayState == replay::ReplayState::Verified &&
             loaded.records[0].capabilities.watch,
         "existing replay file enables replay actions after the cheap probe");
  const auto stored = fixture.repository.LoadModernChartResultByAttempt(
      completed.attemptId);
  assert(stored.record && stored.record->replayFile);
  assert(fixture.repository.MarkModernReplayFileUserDeleted(
      ModernReplayOwnerKind::ChartResult, completed.attemptId,
      *stored.record->replayFile).status == ModernReplayFileMutationStatus::Changed);
  loaded = fixture.load();
  expect(loaded.records.size() == 1 &&
             loaded.records[0].replayState == replay::ReplayState::UserDeleted &&
             !loaded.records[0].capabilities.watch &&
             loaded.records[0].capabilities.resultRecall,
         "deleted replay keeps result recall while disabling replay actions");
}

void testModernCourseResultsUseCourseKey() {
  Fixture fixture;
  result_persistence::ModernCourseStageResult courseStage;
  const auto chartResult = result(1);
  courseStage.score = chartResult.score;
  courseStage.keyMode = chartResult.keyMode;
  courseStage.adoptedGaugeType = chartResult.adoptedGaugeType;
  courseStage.adoptedGaugeHistory = chartResult.adoptedGaugeHistory;
  const std::string courseKey = "course:v1:" + std::string(64, 'c');
  const result_persistence::ModernCourseResultCapture capture{
      .attemptId = chartResult.attemptId,
      .courseKey = courseKey,
      .legacyCourseId = 17,
      .courseName = "Course",
      .courseGroupName = "Tests",
      .constraintJson = "[]",
      .requestedPlayOption = "NORMAL",
      .assistOption = "OFF",
      .initialGaugeType = GaugeType::Normal,
      .longNoteMode = 1,
      .clearType = kClearTypeNormalClearRank,
      .stages = {courseStage},
      .entryFacts = {{.totalNotes = 5, .playLengthMicros = 1'000'000}},
      .playedAtUnixMillis = 1'700'000'000'000LL,
  };
  std::string diagnostic;
  const auto captured = result_persistence::captureModernCourseResult(
      capture, diagnostic);
  assert(captured);
  assert(fixture.repository.StageModernCourseResult(*captured, std::nullopt)
             .status == ModernCourseStageStatus::Staged);
  fixture.chart.courseStart = true;
  fixture.options.course = CourseReplayLookup{.courseKey = courseKey,
                                              .legacyCourseId = 17};
  const auto loaded = fixture.load();
  expect(loaded.records.size() == 1 && loaded.complete &&
             loaded.records[0].isModernCourse() &&
             loaded.records[0].capabilities.resultRecall &&
             !loaded.records[0].capabilities.watch,
         "course key loads modern course history with replay availability");
}

} // namespace

int main() {
  testChartSourcesAndReplayAvailability();
  testOriginDiagnosticsAndRecovery();
  testInvalidRemoteHistoryKeepsLocalResults();
  testLegacyHistoryIsNotReducedToDefaultPage();
  testCourseLookupUsesLegacyIdWithoutChartSources();
  testAttemptActivityAndReceiptLinkage();
  testReplayFileProbeChangesCapabilities();
  testModernCourseResultsUseCourseKey();
  return failures == 0 ? 0 : 1;
}
