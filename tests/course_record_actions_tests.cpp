// Keep the initial contract runnable before the shared implementation exists.
#if __has_include("scene/CourseRecordActions.h")
#include "scene/CourseRecordActions.h"
#include "CoursePlaySession.h"
#include "PlayOptionUtils.h"
#include "repositories/ReplayRepository.h"
#include "replay/ReplayFileStore.h"
#include "replay/ReplaySetupProvenance.h"
#define HAS_COURSE_RECORD_ACTIONS 1
#else
#define HAS_COURSE_RECORD_ACTIONS 0
#endif

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
void require(bool condition, std::string_view message) {
  if (!condition) throw std::runtime_error(std::string(message));
}

#if HAS_COURSE_RECORD_ACTIONS
struct Fixture {
  std::filesystem::path directory;
  std::vector<ChartMetaRecord> records;
  result_persistence::ModernCourseResult result;

  Fixture() {
    directory = std::filesystem::temp_directory_path() /
        ("asobmashow-course-record-actions-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    result_persistence::ModernCourseResultCapture capture{
        .attemptId = "123e4567-e89b-42d3-a456-426614174000",
        .courseKey = "course:v1:" + std::string(64, 'c'),
        .legacyCourseId = 42,
        .courseName = "Saved course",
        .courseGroupName = "Saved group",
        .constraintJson = R"(["no_speed"])",
        .requestedPlayOption = "NORMAL",
        .assistOption = "OFF",
        .initialGaugeType = GaugeType::Hard,
        .gaugeProfile = GaugeProfile::Standard,
        .gaugeAutoShift = GaugeAutoShiftMode::Continue,
        .gaugeAutoShiftLowerBound = GaugeType::Easy,
        .longNoteMode = 1,
        .clearType = kClearTypeFailedRank,
        .playedAtUnixMillis = 1'700'000'001'234LL,
    };
    for (int index = 0; index < 3; ++index) {
      const auto path = directory / ("stage-" + std::to_string(index) + ".bms");
      {
        std::ofstream output(path);
        output << "#PLAYER 1\n#TITLE Stage " << index
               << "\n#ARTIST Test\n#BPM 120\n#PLAYLEVEL 1\n#RANK 2\n"
                  "#TOTAL 200\n#WAV01 test.wav\n#00111:0101010101\n";
      }
      std::atomic_bool cancelled{false};
      auto chart = play_options::parseChart(path, cancelled, "course records test");
      require(chart != nullptr && chart->Meta.TotalNotes == 5,
              "disk fixture must contain five playable notes");
      records.push_back({.meta = chart->Meta});
      capture.entryFacts.push_back({.totalNotes = 5,
                                    .playLengthMicros = chart->Meta.PlayLength});
      if (index == 2) continue;
      result_persistence::ModernCourseStageResult stage;
      stage.stageIndex = index;
      stage.score.chartPath = (directory / "old-location.bms").string();
      stage.score.chartMd5 = chart->Meta.MD5;
      stage.score.chartSha256 = chart->Meta.SHA256;
      stage.score.chartTitle = "Saved stage " + std::to_string(index);
      stage.score.chartArtist = "Saved artist";
      stage.score.longNoteMode = 0;
      stage.score.score = 7;
      stage.score.maxScore = 10;
      stage.score.maxCombo = index == 0 ? 4 : 8;
      stage.score.comboBreak = 1;
      stage.score.pGreat = 3;
      stage.score.great = 1;
      stage.score.good = 1;
      stage.score.finalGauge = index == 0 ? 76.0F : 62.5F;
      stage.score.clearType = kClearTypeHardClearRank;
      ScoreProvenanceBuildInput provenance;
      provenance.chartMeta = chart->Meta;
      provenance.longNoteMode = 1;
      provenance.sourceJudgeRank = 2;
      provenance.effectiveJudgeWindows = {
          {PGreat, {-10'000, 10'000}}, {Great, {-30'000, 30'000}},
          {Good, {-75'000, 75'000}}, {Bad, {-200'000, 200'000}},
          {Kpoor, {-1'000'000, 0}},
      };
      provenance.totalNotes = 5;
      provenance.authoredGaugeTotal = 200.0;
      provenance.effectiveGaugeTotal = 200.0;
      provenance.gaugeType = GaugeType::Hard;
      provenance.gaugeAutoShift = GaugeAutoShiftMode::Continue;
      provenance.gaugeAutoShiftLowerBound = GaugeType::Easy;
      stage.score.provenance = makeScoreProvenance(provenance);
      stage.keyMode = chart->Meta.KeyMode;
      stage.adoptedGaugeType = GaugeType::Hard;
      stage.adoptedGaugeHistory = {80.0F, stage.score.finalGauge};
      capture.stages.push_back(std::move(stage));
    }
    std::string diagnostic;
    auto captured = result_persistence::captureModernCourseResult(capture, diagnostic);
    require(captured.has_value(), "fixture captures a valid saved course: " + diagnostic);
    result = std::move(*captured);
  }
  ~Fixture() {
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }
  auto selection() const {
    return course_records::currentCourseSelectionFor(result.courseKey, records, result);
  }
};

void testCompletedPrefixValidation() {
  Fixture fixture;
  auto selected = fixture.selection();
  require(selected && selected->completeCourse &&
              selected->completedChartPaths.size() == 2,
          "a partial saved result may retry when all current course entries exist");
  require(!course_records::currentCourseSelectionFor("other-course", fixture.records,
                                                    fixture.result),
          "a different course cannot supply result chart paths");
  for (int unsafe = 0; unsafe < 3; ++unsafe) {
    auto records = fixture.records;
    if (unsafe == 0) records[1].unavailable = true;
    if (unsafe == 1) records[1].solidArchive = true;
    if (unsafe == 2) records[1].meta.BmsPath.clear();
    require(!course_records::currentCourseSelectionFor(fixture.result.courseKey,
                                                       records, fixture.result),
            "each unsafe completed-stage path must reject rich recall");
  }
  fixture.records.resize(1);
  require(!fixture.selection(), "a missing completed stage rejects rich recall");
}

void testPartialCourseRetryAndOwnedState() {
  Fixture fixture;
  ReplayRepository repository(fixture.directory / "replay.db");
  require(repository.EnsureSchema(), "repository schema initializes");
  require(repository.StageModernCourseResult(fixture.result, std::nullopt).status ==
              ModernCourseStageStatus::Staged, "real repository stages the result");
  std::atomic_bool cancelled{false};
  auto prepare = [&](bool retry) {
    return course_records::prepareCourseResult(repository, fixture.result.attemptId,
                                               fixture.selection(), retry, cancelled);
  };
  auto prepared = prepare(true);
  require(prepared.session != nullptr, "saved course prepares: " + prepared.diagnostic);
  auto session = prepared.session;
  require(session->modernCourseRetrySameAllowed && session->entries.size() == 3 &&
              session->completedResults.size() == 2,
          "full current course allows retry of a partial saved result");
  require(session->constraints.noSpeed && session->gaugeType == GaugeType::Hard &&
              session->gaugeAutoShift == GaugeAutoShiftMode::Continue &&
              session->gaugeAutoShiftLowerBound == GaugeType::Easy &&
              session->maxCombo == 8 && session->carriedGauge &&
              session->carriedGauge->currentGauge == 62.5F &&
              session->finalClearTypeForPresentation(kClearTypeHardClearRank) ==
                  kClearTypeFailedRank,
          "saved course constraints, gauge, combo and aggregate lamp survive preparation");
  require(session->modernCourseAttemptId == fixture.result.attemptId &&
              session->modernCoursePlayedAtUnixMillis == 1'700'000'001'234LL &&
              session->stageProvenance[1] == fixture.result.stages[1].score.provenance,
          "result identity, time and authenticated stage provenance are preserved");
  require(!session->resultBrowseReplayData && !session->resultBrowseStageReplay(0),
          "results without replay files still browse without fabricated replay state");
  require(!prepare(false).session->modernCourseRetrySameAllowed,
          "caller denial of retry must be preserved for complete courses");
  fixture.records[2].unavailable = true;
  require(!prepare(true).session->modernCourseRetrySameAllowed,
          "an unavailable unplayed stage permits recall but disables retry");
  fixture.records.resize(2);
  auto partial = prepare(true);
  require(partial.session && !partial.session->modernCourseRetrySameAllowed &&
              partial.session->entries[2].meta.TotalNotes == 5,
          "missing unplayed entries retain saved course facts but cannot retry");
  fixture.records.clear();
  prepared = {};
  require(session->ownedResultBrowseCharts.size() == 2 &&
              session->ownedResultBrowseCharts[1]->Meta.Title == "Saved stage 1" &&
              session->completedResults[1].state.getScore() == 7 &&
              session->completedResults[1].state.gaugeHistoryFor(GaugeType::Hard) ==
                  std::vector<float>({80.0F, 62.5F}),
          "session owns recalled charts and remains usable after preparation inputs expire");
}

void testVerifiedReplayOwnershipAndMissingFileFallback() {
  Fixture fixture;
  ReplayRepository repository(fixture.directory / "replay.db");
  require(repository.EnsureSchema(), "repository schema initializes");
  replay::ReplayCourseDocument document;
  replay::CoursePathInput pathInput{
      .longNoteMode = 1,
      .beatorajaConstraintIds = {4},
  };
  std::string diagnostic;
  for (const auto &stage : fixture.result.stages) {
    auto setup = replay::captureLocalReplaySetup(
        {.chart = {.md5 = stage.score.chartMd5,
                   .sha256 = stage.score.chartSha256,
                   .keyMode = stage.keyMode},
         .longNoteMode = 1}, stage.score.provenance, diagnostic);
    require(setup.has_value(), "real replay setup captures: " + diagnostic);
    document.playback.stages.push_back({.setup = *setup});
    document.playback.restMicrosAfterStage.push_back(stage.stageIndex == 0 ? 1'000'000 : 0);
    document.timeBounds.push_back({.completionSongTimeMicros = 5'000'000});
    pathInput.stageSha256.push_back(stage.score.chartSha256);
  }
  replay::BeatorajaReplayCodec codec;
  const auto bytes = codec.encodeCourse(document, fixture.result.playedAtUnixMillis,
                                         diagnostic);
  require(bytes.has_value(), "real replay codec encodes: " + diagnostic);
  const auto stem = replay::courseStem(pathInput, diagnostic);
  require(stem.has_value(), "course replay path derives");
  const auto reserved = repository.ReserveModernReplayPath(
      fixture.result.attemptId, *stem, fixture.result.playedAtUnixMillis);
  require(reserved.reservation.has_value(), "repository reserves replay attachment");
  replay::ReplayFileStore store(repository.GetResolvedProfileRoot());
  const auto fileReservation = store.reserve(reserved.reservation->identity, *bytes,
                                               fixture.result.attemptId);
  require(fileReservation.reservation.has_value(), "file store reserves replay bytes");
  const auto installed = store.install(*fileReservation.reservation, *bytes,
      [&](const replay::ReplayFileOwnershipReceipt &receipt, std::string &) {
        return repository.RecordModernReplayInstallIntent(*reserved.reservation, receipt)
                   .status == ModernReplayOwnershipRecordStatus::Recorded;
      });
  require(installed.file.has_value(), "file store installs verified replay bytes");
  const ModernReplayFileAttachment attachment{
      .identity = reserved.reservation->identity,
      .metadata = installed.file->metadata,
  };
  require(repository.StageModernCourseResult(fixture.result, attachment, pathInput).status ==
              ModernCourseStageStatus::Staged, "real result and replay attachment stage");
  std::atomic_bool cancelled{false};
  auto prepared = course_records::prepareCourseResult(repository, fixture.result.attemptId,
                                                       fixture.selection(), true, cancelled);
  require(prepared.session != nullptr, "attached replay result prepares");
  auto session = prepared.session;
  require(session->resultBrowseStageReplay(0) && session->resultBrowseReplayChart(0) &&
              session->resultBrowseStageReplay(1) && session->resultBrowseReplayChart(1),
          "verified completed replay prefix is attached to the owned result session");
  require(session->completedResults[0].gameplayGraph.chart &&
              session->completedResults[0].gameplayGraph.dynamic,
          "completed stages retain graph sources for rich result browsing");
  const auto replayPath = repository.GetResolvedProfileRoot() /
                         installed.file->metadata.relativePath;
  std::filesystem::remove(replayPath);
  prepared = {};
  require(session->resultBrowseStageReplay(1)->provenance ==
              fixture.result.stages[1].score.provenance &&
              session->resultBrowseReplayChart(1)->Meta.TotalNotes == 5,
          "attached replay charts and provenance outlive worker output and file deletion");
  auto fallback = course_records::prepareCourseResult(repository, fixture.result.attemptId,
                                                       fixture.selection(), true, cancelled);
  require(fallback.session && !fallback.session->resultBrowseReplayData &&
              fallback.session->completedResults[0].gameplayGraph.chart &&
              fallback.session->completedResults[0].state.getScore() == 7,
          "an absent optional replay preserves saved result state and chart graphs");
}

void testFailureAndCancellation() {
  Fixture fixture;
  ReplayRepository repository(fixture.directory / "replay.db");
  require(repository.EnsureSchema(), "repository schema initializes");
  std::atomic_bool cancelled{false};
  auto missing = course_records::prepareCourseResult(repository, fixture.result.attemptId,
                                                     fixture.selection(), true, cancelled);
  require(!missing.session && !missing.diagnostic.empty(),
          "missing exact attempts return a diagnostic without publishing a session");
  require(repository.StageModernCourseResult(fixture.result, std::nullopt).status ==
              ModernCourseStageStatus::Staged, "real repository stages the result");
  auto unavailable = course_records::prepareCourseResult(repository, fixture.result.attemptId,
                                                         std::nullopt, true, cancelled);
  require(!unavailable.session && !unavailable.diagnostic.empty(),
          "an invalid selection cannot prepare a stored course");
  std::filesystem::remove(fixture.records[1].meta.BmsPath);
  auto deleted = course_records::prepareCourseResult(repository, fixture.result.attemptId,
                                                     fixture.selection(), true, cancelled);
  require(!deleted.session && !deleted.diagnostic.empty(),
          "a completed stage removed after selection must fail without partial publication");
  cancelled.store(true);
  auto stopped = course_records::prepareCourseResult(repository, fixture.result.attemptId,
                                                     fixture.selection(), true, cancelled);
  require(!stopped.session && stopped.diagnostic.empty(),
          "cancelled work publishes neither a session nor a failure notification");
}
#endif
} // namespace

int main() {
  try {
#if HAS_COURSE_RECORD_ACTIONS
    testCompletedPrefixValidation();
    testPartialCourseRetryAndOwnedState();
    testFailureAndCancellation();
    testVerifiedReplayOwnershipAndMissingFileFallback();
#else
    require(false, "shared course record actions are not implemented");
#endif
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
  std::cout << "course record actions tests passed\n";
}
