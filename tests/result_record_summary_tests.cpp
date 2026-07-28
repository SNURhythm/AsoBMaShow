#include "../src/ResultRecordSummary.h"
#include "../src/replay/ReplayFileActionSelection.h"

#include <climits>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

template <typename Callable>
void expectInvalid(Callable &&callable, const char *message) {
  try {
    std::forward<Callable>(callable)();
  } catch (const std::invalid_argument &) {
    return;
  } catch (...) {
    std::cerr << "FAIL: " << message << " (wrong exception type)\n";
    ++failures;
    return;
  }
  std::cerr << "FAIL: " << message << " (accepted invalid input)\n";
  ++failures;
}

result_persistence::ModernChartResult validModernResult();
result_persistence::ModernCourseResult validModernCourseResult();

void testReplayFileActionsUseModernIdentityAndCapabilitiesOnly() {
  ModernChartResultRecord chartRecord{.result = validModernResult()};
  const auto chart = makeModernChartResultRecord(
      chartRecord, replay::ReplayState::Verified, false);
  const auto chartActions = replay::replayFileActionSelection(chart, true);
  expect(chartActions.request.has_value() && chartActions.shareVisible &&
             chartActions.deleteVisible && chartActions.enabled &&
             chartActions.request->owner ==
                 ModernReplayOwnerKind::ChartResult &&
             chartActions.request->attemptId == chartRecord.result.attemptId,
         "verified chart actions use modern attempt identity and capabilities");

  ModernCourseResultRecord courseRecord{.result = validModernCourseResult()};
  const auto course =
      makeModernCourseResultRecord(courseRecord, replay::ReplayState::Corrupt);
  const auto courseActions = replay::replayFileActionSelection(course, true);
  expect(courseActions.request.has_value() && !courseActions.shareVisible &&
             courseActions.deleteVisible &&
             courseActions.request->owner ==
                 ModernReplayOwnerKind::CourseResult,
         "corrupt course remains deletable but not shareable");

  LegacyChartResultSummary legacySummary;
  legacySummary.legacyReplayId = chartRecord.result.resultId;
  auto legacy = makeLegacyChartResultRecord(legacySummary);
  legacy.capabilities.shareOrCopy = true;
  legacy.capabilities.deleteReplayFile = true;
  legacy.modern = chartRecord;
  const auto legacyActions = replay::replayFileActionSelection(legacy, true);
  expect(!legacyActions.request && !legacyActions.shareVisible &&
             !legacyActions.deleteVisible,
         "legacy identities cannot enter modern replay file actions");

  const auto busy = replay::replayFileActionSelection(chart, false);
  expect(busy.shareVisible && busy.deleteVisible && !busy.enabled,
         "busy UI preserves action visibility while disabling interaction");
}

void testRecordActionsRequireTypedIdentityAndPayloadAgreement() {
  ModernChartResultRecord chartRecord{.result = validModernResult()};
  auto modernChart = makeModernChartResultRecord(
      chartRecord, replay::ReplayState::Verified, false);
  expect(resultRecordActionTarget(modernChart, ResultRecordAction::Watch) ==
             ResultRecordActionTarget::ModernChart &&
             resultRecordActionTarget(modernChart,
                                      ResultRecordAction::GBattle) ==
                 ResultRecordActionTarget::ModernChart,
         "verified modern chart dispatches through its typed identity");

  ModernCourseResultRecord courseRecord{.result = validModernCourseResult()};
  auto modernCourse = makeModernCourseResultRecord(
      courseRecord, replay::ReplayState::Verified);
  expect(resultRecordActionTarget(modernCourse, ResultRecordAction::Watch) ==
             ResultRecordActionTarget::ModernCourse &&
             resultRecordActionTarget(modernCourse,
                                      ResultRecordAction::RetrySame) ==
                 ResultRecordActionTarget::ModernCourse,
         "verified modern course dispatches through its typed identity");

  LegacyChartResultSummary legacyChartSummary;
  legacyChartSummary.legacyReplayId = chartRecord.result.resultId;
  auto legacyChart = makeLegacyChartResultRecord(legacyChartSummary);
  legacyChart.capabilities = modernChart.capabilities;
  legacyChart.modern = chartRecord;
  expect(resultRecordActionTarget(legacyChart, ResultRecordAction::Watch) ==
             ResultRecordActionTarget::None &&
             resultRecordActionTarget(legacyChart,
                                      ResultRecordAction::ResultRecall) ==
                 ResultRecordActionTarget::None,
         "legacy chart ID collision cannot dispatch modern chart actions");

  LegacyCourseResultSummary legacyCourseSummary;
  legacyCourseSummary.legacyCourseReplayId = courseRecord.result.resultId;
  auto legacyCourse = makeLegacyCourseResultRecord(legacyCourseSummary);
  legacyCourse.capabilities = modernCourse.capabilities;
  legacyCourse.modernCourse = courseRecord;
  expect(resultRecordActionTarget(legacyCourse, ResultRecordAction::Watch) ==
             ResultRecordActionTarget::None &&
             resultRecordActionTarget(legacyCourse,
                                      ResultRecordAction::RetrySame) ==
                 ResultRecordActionTarget::None,
         "legacy course ID collision cannot dispatch modern course actions");

  ReplaySummary autoPlaySummary;
  autoPlaySummary.id = -1;
  autoPlaySummary.autoPlay = true;
  autoPlaySummary.maxScore = 2;
  auto autoPlay = makeAutoPlayResultRecord(autoPlaySummary);
  auto legacyAutoplayCollision = legacyChart;
  legacyAutoplayCollision.autoPlay = true;
  legacyAutoplayCollision.autoPlayReplay = autoPlay.autoPlayReplay;
  expect(resultRecordActionTarget(autoPlay, ResultRecordAction::Watch) ==
             ResultRecordActionTarget::AutoPlay &&
             resultRecordActionTarget(legacyAutoplayCollision,
                                      ResultRecordAction::Watch) ==
                 ResultRecordActionTarget::None,
         "legacy identity cannot impersonate the autoplay action target");

  modernChart.modern->result.attemptId =
      "123e4567-e89b-42d3-a456-426614174099";
  expect(resultRecordActionTarget(modernChart, ResultRecordAction::Watch) ==
             ResultRecordActionTarget::None,
         "modern payload disagreement disables every replay action");
}

ir::IrRemoteScore validRemoteScore() {
  return {
      .remoteUserId = 42,
      .game = "bms-7k",
      .remoteScoreId = "remote-score-42",
      .remoteChartId = "remote-chart-42",
      .chartMd5 = std::string(32, 'a'),
      .chartSha256 = std::string(64, 'b'),
      .title = "Remote title",
      .artist = "Remote artist",
      .service = "Bokutachi",
      .difficulty = "ANOTHER",
      .level = "12",
      .levelNumber = 12.4,
      .noteCount = 1'234,
      .score = 2'100,
      .lampRank = kClearTypeHardClearRank,
      .timeAchievedUnixMillis = 1'704'164'645'123LL,
      .timeAddedUnixMillis = 1'704'164'700'456LL,
      .judgements =
          {.pGreat = 900, .great = 200, .good = 50, .bad = 40, .poor = 44},
      .timing = {.earlyPGreat = 450,
                 .latePGreat = 450,
                 .earlyGreat = 100,
                 .lateGreat = 100,
                 .earlyGood = 25,
                 .lateGood = 25,
                 .earlyBad = 20,
                 .lateBad = 20,
                 .earlyPoor = 22,
                 .latePoor = 22},
      .fast = 617,
      .slow = 617,
      .maxCombo = 777,
      .badPoints = 84,
      .finalGauge = 78.5F,
      .gaugeHistory = {20.0F, std::nullopt, 78.5F},
      .random = "RANDOM",
      .gauge = "HARD",
      .inputDevice = "Keyboard",
      .client = "AsoBMaShow",
  };
}

result_persistence::ModernChartResult validModernResult() {
  result_persistence::ModernChartResult result;
  result.resultId = 91;
  result.attemptId = "123e4567-e89b-42d3-a456-426614174000";
  result.score.chartPath = "BMS/example/chart.bms";
  result.score.chartMd5 = std::string(32, 'a');
  result.score.chartSha256 = std::string(64, 'b');
  result.score.chartTitle = "Modern title";
  result.score.chartArtist = "Modern artist";
  result.score.longNoteMode = 1;
  result.score.score = 1'900;
  result.score.maxScore = 2'000;
  result.score.maxCombo = 900;
  result.score.comboBreak = 2;
  result.score.pGreat = 900;
  result.score.great = 100;
  result.score.finalGauge = 78.5F;
  result.score.clearType = kClearTypeHardClearRank;
  ScoreProvenanceBuildInput provenance;
  provenance.chartMeta.MD5 = result.score.chartMd5;
  provenance.chartMeta.SHA256 = result.score.chartSha256;
  provenance.chartMeta.KeyMode = 7;
  provenance.chartMeta.Rank = 2;
  provenance.chartMeta.TotalNotes = 1'000;
  provenance.chartMeta.HasTotal = true;
  provenance.chartMeta.Total = 200.0;
  provenance.longNoteMode = result.score.longNoteMode;
  provenance.sourceJudgeRank = 2;
  provenance.effectiveJudgeWindows = {
      {PGreat, {-10'000, 10'000}}, {Great, {-30'000, 30'000}},
      {Good, {-75'000, 75'000}},   {Bad, {-200'000, 200'000}},
      {Kpoor, {-1'000'000, 0}},
  };
  provenance.totalNotes = 1'000;
  provenance.authoredGaugeTotal = 200.0;
  provenance.effectiveGaugeTotal = 200.0;
  provenance.player1.option = "RANDOM";
  provenance.player1.seed = 42;
  provenance.inputDevices = {InputDeviceCategory::Keyboard};
  result.score.provenance = makeScoreProvenance(provenance);
  result.keyMode = 7;
  result.adoptedGaugeType = GaugeType::Hard;
  result.adoptedGaugeHistory = {100.0F, 78.5F};
  result.playedAtUnixMillis = 1'704'164'645'123LL;
  result.resultFingerprint = std::string(64, 'c');
  return result;
}

result_persistence::ModernCourseResult validModernCourseResult() {
  const auto chart = validModernResult();
  result_persistence::ModernCourseResult result;
  result.resultId = 92;
  result.attemptId = "123e4567-e89b-42d3-a456-426614174001";
  result.courseKey = "course:v1:" + std::string(64, 'd');
  result.legacyCourseId = 18;
  result.courseName = "Modern course";
  result.courseGroupName = "Modern group";
  result.constraintJson = "{}";
  result.completedCharts = 1;
  result.totalCharts = 1;
  result.requestedPlayOption = "RANDOM";
  result.assistOption = assist_options::kOff;
  result.initialGaugeType = GaugeType::Hard;
  result.gaugeProfile = GaugeProfile::Standard;
  result.gaugeAutoShift = GaugeAutoShiftMode::None;
  result.gaugeAutoShiftLowerBound = GaugeType::AssistedEasy;
  result.longNoteMode = chart.score.longNoteMode;
  result.finalScore = chart.score.score;
  result.maxScore = chart.score.maxScore;
  result.maxCombo = chart.score.maxCombo;
  result.finalGauge = chart.score.finalGauge;
  result.clearType = chart.score.clearType;
  result.provenance = chart.score.provenance;
  result.stages = {{.stageIndex = 0,
                    .score = chart.score,
                    .keyMode = chart.keyMode,
                    .adoptedGaugeType = chart.adoptedGaugeType,
                    .adoptedGaugeHistory = chart.adoptedGaugeHistory,
                    .judgementTiming = chart.judgementTiming}};
  result.entryFacts = {
      {.totalNotes = chart.score.maxScore / 2, .playLengthMicros = 3'000'000}};
  result.playedAtUnixMillis = chart.playedAtUnixMillis + 1'000;
  result.resultFingerprint = std::string(64, 'd');
  return result;
}

void testModernConversionUsesSharedReplayCapabilities() {
  ModernChartResultRecord record{.result = validModernResult()};
  const auto absent =
      makeModernChartResultRecord(record, replay::ReplayState::Missing, true);
  expect(absent.isLocal() && absent.isModernChart() && !absent.isRemote() &&
             absent.modernAttemptId() == record.result.attemptId,
         "modern result has a durable tagged attempt identity");
  expect(absent.capabilities.resultRecall && absent.capabilities.irUpload &&
             !absent.capabilities.watch && !absent.capabilities.gBattle &&
             !absent.capabilities.videoExport,
         "missing BRD keeps result and IR while disabling replay actions");
  expect(absent.modern && absent.modern->result == record.result &&
             absent.score == record.result.score.score &&
             absent.maxScore == record.result.score.maxScore &&
             absent.playOption ==
                 record.result.score.provenance.player1.option &&
             absent.stableKey() == "m:" + record.result.attemptId,
         "modern projection reads display facts only from the strict result");

  const auto verified =
      makeModernChartResultRecord(record, replay::ReplayState::Verified, false);
  expect(verified.capabilities.watch && verified.capabilities.gBattle &&
             verified.capabilities.resultRecall &&
             verified.capabilities.videoExport &&
             !verified.capabilities.irUpload,
         "verified BRD enables projected replay consumers via the matrix");

  const auto corrupt =
      makeModernChartResultRecord(record, replay::ReplayState::Corrupt, false);
  expect(!corrupt.capabilities.watch && corrupt.capabilities.deleteReplayFile,
         "invalid present BRD remains deletable but not playable");
}

void testModernCourseConversionKeepsResultWithoutReplay() {
  ModernCourseResultRecord record{.result = validModernCourseResult()};
  const auto missing =
      makeModernCourseResultRecord(record, replay::ReplayState::Missing);
  expect(missing.isLocal() && missing.isModernCourse() &&
             !missing.isModernChart() && !missing.isRemote() &&
             missing.modernAttemptId() == record.result.attemptId,
         "modern course result has a distinct durable identity");
  expect(missing.course && missing.capabilities.resultRecall &&
             !missing.capabilities.watch && !missing.capabilities.retrySame &&
             !missing.capabilities.videoExport &&
             !missing.capabilities.gBattle &&
             !missing.capabilities.practiceGhost,
         "missing course BRD leaves only result-domain actions");
  expect(missing.modernCourse &&
             missing.modernCourse->result == record.result && !missing.modern &&
             missing.score == record.result.finalScore &&
             missing.maxScore == record.result.maxScore &&
             missing.maxCombo == record.result.maxCombo &&
             missing.playOption == record.result.requestedPlayOption &&
             missing.stableKey() == "c:" + record.result.attemptId,
         "course projection reads display facts only from its strict row");

  const auto verified =
      makeModernCourseResultRecord(record, replay::ReplayState::Verified);
  expect(verified.capabilities.watch && verified.capabilities.retrySame &&
             verified.capabilities.videoExport &&
             verified.capabilities.resultRecall &&
             !verified.capabilities.gBattle &&
             !verified.capabilities.practiceGhost &&
             !verified.capabilities.irUpload,
         "verified course BRD enables only supported replay actions");

  for (const auto state :
       {replay::ReplayState::Corrupt, replay::ReplayState::Mismatched,
        replay::ReplayState::UnsupportedExtension}) {
    const auto invalid = makeModernCourseResultRecord(record, state);
    expect(invalid.capabilities.resultRecall && !invalid.capabilities.watch &&
               !invalid.capabilities.retrySame &&
               !invalid.capabilities.videoExport &&
               invalid.capabilities.deleteReplayFile,
           "invalid course BRD disables playback but remains diagnosable");
  }
}

void testLegacySummariesExposeRecordsOnly() {
  LegacyChartResultSummary chart;
  chart.legacyReplayId = 11;
  chart.chartTitle = "Legacy chart";
  chart.createdAt = "2026-07-20 01:02:03";
  chart.partial = true;
  const auto chartRecord = makeLegacyChartResultRecord(chart);
  expect(chartRecord.isLocal() && chartRecord.legacyChart == chart &&
             !chartRecord.legacyCourse,
         "legacy chart has an explicit non-replay identity");
  expect(chartRecord.capabilities == ResultRecordCapabilities{} &&
             chartRecord.stableKey() == "lc:11",
         "legacy chart exposes Records without replay or result actions");
  expect(!chartRecord.scoreAvailable && !chartRecord.maxScoreAvailable &&
             !chartRecord.clearRankAvailable && !chartRecord.maxCombo,
         "unknown legacy chart facts remain unavailable");

  LegacyCourseResultSummary course;
  course.legacyCourseReplayId = 21;
  course.finalScore = 2100;
  course.maxCombo = 321;
  course.clearType = kClearTypeHardClearRank;
  course.partial = true;
  const auto courseRecord = makeLegacyCourseResultRecord(course);
  expect(courseRecord.course && courseRecord.legacyCourse == course &&
             !courseRecord.legacyChart,
         "legacy course has an explicit summary identity");
  expect(courseRecord.capabilities == ResultRecordCapabilities{} &&
             courseRecord.stableKey() == "lco:21" &&
             courseRecord.scoreAvailable && !courseRecord.maxScoreAvailable &&
             courseRecord.clearRankAvailable && courseRecord.maxCombo == 321,
         "legacy course projects only independently stored header facts");
}

void testAutoPlayIsTheOnlyReplaySummaryBackedRecord() {
  ReplaySummary replay;
  replay.id = -1;
  replay.autoPlay = true;
  replay.finalScore = 1'000;
  replay.maxScore = 1'000;
  replay.maxCombo = 500;
  replay.clearType = kClearTypeNormalClearRank;
  replay.createdAt = "AUTO PLAY";
  replay.playOption = "MIRROR";
  replay.irRecordState = ir::IrRecordState::Eligible;

  const ResultRecordSummary result = makeAutoPlayResultRecord(replay);
  expect(result.isLocal() && !result.isRemote() && result.autoPlay &&
             std::holds_alternative<AutoPlayRecordId>(result.identity) &&
             result.stableKey() == "a:auto-play",
         "Auto Play has a dedicated non-integer Records identity");
  expect(result.capabilities.watch && result.capabilities.videoExport &&
             !result.capabilities.gBattle &&
             !result.capabilities.resultRecall &&
             !result.capabilities.irUpload,
         "Auto Play exposes only its synthetic playback actions");
  expect(result.autoPlayReplay && result.autoPlayReplay->id == -1 &&
             result.irState == ir::IrRecordState::Hidden && !result.remote,
         "Auto Play retains only the synthetic playback payload");

  replay.autoPlay = false;
  replay.id = 73;
  expectInvalid(
      [&] { static_cast<void>(makeAutoPlayResultRecord(replay)); },
      "positive legacy replay summaries cannot enter modern action routing");
  replay.autoPlay = true;
  replay.id = 0;
  expectInvalid(
      [&] { static_cast<void>(makeAutoPlayResultRecord(replay)); },
      "noncanonical Auto Play sentinel IDs are rejected");
  replay.id = -1;
  replay.courseReplay = true;
  expectInvalid(
      [&] { static_cast<void>(makeAutoPlayResultRecord(replay)); },
      "course summaries cannot impersonate Auto Play");
}

void testRemoteConversionIsReadOnlyAndRetainsOptionalValues() {
  const ir::IrRemoteScore score = validRemoteScore();
  const ResultRecordSummary result =
      makeRemoteResultRecord("tachi", "https://boku.tachi.ac", score);

  expect(result.isRemote() && !result.isLocal(),
         "remote conversion keeps the remote tag");
  expect(result.remoteScoreId() == score.remoteScoreId,
         "remote conversion exposes only the remote score identity");
  const auto &identity = std::get<IrRemoteRecordId>(result.identity);
  expect(identity.providerId == "tachi" &&
             identity.serverOrigin == "https://boku.tachi.ac" &&
             identity.remoteScoreId == score.remoteScoreId,
         "remote identity is scoped by provider and normalized origin");
  expect(!result.capabilities.watch && !result.capabilities.gBattle &&
             result.capabilities.resultRecall &&
             !result.capabilities.videoExport && !result.capabilities.irUpload,
         "remote records expose View Result only");
  expect(!result.course && !result.autoPlay && result.score == score.score &&
             result.maxScore == 2'468 && result.maxCombo == score.maxCombo &&
             result.clearRank == score.lampRank,
         "remote score conversion preserves supplied values and derives max");
  expect(result.displayedTimeUnixMillis == *score.timeAchievedUnixMillis &&
             result.displayedTime == "2024-01-02 03:04:05.123",
         "remote display time prefers timeAchieved");
  expect(result.playOption == score.random &&
             result.irState == ir::IrRecordState::Uploaded,
         "remote option remains optional and IR is read-only uploaded");
  expect(!result.autoPlayReplay && result.remote &&
             result.remote->remoteScoreId == score.remoteScoreId,
         "remote conversion nests only the validated stored score model");

  ir::IrRemoteScore fallback = score;
  fallback.remoteScoreId = "fallback-time";
  fallback.timeAchievedUnixMillis.reset();
  const ResultRecordSummary fallbackResult =
      makeRemoteResultRecord("tachi", "https://boku.tachi.ac", fallback);
  expect(fallbackResult.displayedTimeUnixMillis ==
                 fallback.timeAddedUnixMillis &&
             fallbackResult.displayedTime == "2024-01-02 03:05:00.456",
         "remote display time falls back to timeAdded");

  fallback.remoteScoreId = "missing-optionals";
  fallback.maxCombo.reset();
  fallback.random.reset();
  const ResultRecordSummary missing =
      makeRemoteResultRecord("tachi", "https://boku.tachi.ac", fallback);
  expect(!missing.maxCombo && !missing.playOption && missing.remote &&
             !missing.remote->maxCombo && !missing.remote->random,
         "remote nullable fields remain absent instead of becoming sentinels");
}

void testRemoteConversionFailsClosed() {
  const ir::IrRemoteScore valid = validRemoteScore();
  expectInvalid(
      [&] {
        static_cast<void>(
            makeRemoteResultRecord("Tachi", "https://boku.tachi.ac", valid));
      },
      "noncanonical provider ID is rejected");
  expectInvalid(
      [&] {
        static_cast<void>(makeRemoteResultRecord(
            "tachi", "HTTPS://BOKU.TACHI.AC:443/", valid));
      },
      "non-normalized origin is rejected");

  ir::IrRemoteScore invalidIdentity = valid;
  invalidIdentity.remoteScoreId = std::string("score\nsecret", 12);
  expectInvalid(
      [&] {
        static_cast<void>(makeRemoteResultRecord(
            "tachi", "https://boku.tachi.ac", invalidIdentity));
      },
      "invalid remote score identity is rejected");

  ir::IrRemoteScore overflowing = valid;
  overflowing.noteCount = INT_MAX;
  overflowing.score = 0;
  expectInvalid(
      [&] {
        static_cast<void>(makeRemoteResultRecord(
            "tachi", "https://boku.tachi.ac", overflowing));
      },
      "note-count doubling overflow is rejected before multiplication");
}

void testIdentityEqualityHashAndStableKeys() {
  const ResultRecordIdentity autoPlayA = AutoPlayRecordId{};
  const ResultRecordIdentity autoPlayB = AutoPlayRecordId{};
  const ResultRecordIdentity modernA =
      ModernChartRecordId{.attemptId = "attempt-42"};
  const ResultRecordIdentity modernOther =
      ModernChartRecordId{.attemptId = "attempt-43"};
  const ResultRecordIdentity remoteA = IrRemoteRecordId{
      .providerId = "tachi",
      .serverOrigin = "https://boku.tachi.ac",
      .remoteScoreId = "score:42",
  };
  const ResultRecordIdentity remoteB = remoteA;
  const ResultRecordIdentity remoteOther = IrRemoteRecordId{
      .providerId = "tachi",
      .serverOrigin = "https://scores.example.test",
      .remoteScoreId = "score:42",
  };

  expect(autoPlayA == autoPlayB && autoPlayA != modernA &&
             modernA != modernOther && autoPlayA != remoteA &&
             remoteA == remoteB && remoteA != remoteOther,
         "tagged identity equality includes type and every remote scope");
  std::unordered_set<ResultRecordIdentity, ResultRecordIdentityHash> values;
  values.insert(autoPlayA);
  values.insert(autoPlayB);
  values.insert(modernA);
  values.insert(modernOther);
  values.insert(remoteA);
  values.insert(remoteB);
  values.insert(remoteOther);
  expect(values.size() == 5,
         "identity hash agrees with tagged identity equality");

  ReplaySummary replay;
  replay.id = -1;
  replay.autoPlay = true;
  const ResultRecordSummary autoPlay = makeAutoPlayResultRecord(replay);
  ir::IrRemoteScore score = validRemoteScore();
  score.remoteScoreId = "auto-play";
  const ResultRecordSummary remote =
      makeRemoteResultRecord("tachi", "https://boku.tachi.ac", score);
  expect(autoPlay.stableKey() != remote.stableKey(),
         "Auto Play and remote display keys cannot collide");

  score.remoteScoreId = "bc";
  const ResultRecordSummary componentA =
      makeRemoteResultRecord("tachi", "https://a.example", score);
  score.remoteScoreId = "c";
  const ResultRecordSummary componentB =
      makeRemoteResultRecord("tachi", "https://a.exampleb", score);
  expect(componentA.stableKey() != componentB.stableKey(),
         "remote display keys frame identity components unambiguously");

  score.remoteScoreId.assign(ir::kMaximumIrRemoteScoreIdBytes, 's');
  const ResultRecordSummary maximumId =
      makeRemoteResultRecord("tachi", "https://boku.tachi.ac", score);
  expect(maximumId.stableKey().size() <= kMaximumResultRecordStableKeyBytes,
         "validated display keys have an explicit UI-safe size bound");
}

ReplaySummary autoPlayRecord() {
  ReplaySummary replay;
  replay.id = -1;
  replay.autoPlay = true;
  replay.finalScore = 2'000;
  replay.maxScore = 2'000;
  replay.maxCombo = 1'000;
  replay.clearType = kClearTypeFullComboRank;
  replay.createdAt = "AUTO PLAY";
  return replay;
}

ResultRecordSummary modernRecord(std::string attemptId,
                                 std::int64_t playedAtUnixMillis) {
  auto result = validModernResult();
  result.attemptId = std::move(attemptId);
  result.playedAtUnixMillis = playedAtUnixMillis;
  return makeModernChartResultRecord(
      ModernChartResultRecord{.result = std::move(result)},
      replay::ReplayState::Missing, false);
}

void testMergeIncludesModernResultsWithoutChangingTheirCapabilities() {
  auto modernResult = validModernResult();
  modernResult.playedAtUnixMillis = 1'704'164'646'000LL;
  ModernChartResultRecord modernRecord{.result = modernResult};
  LegacyChartResultSummary legacy;
  legacy.legacyReplayId = 77;
  legacy.createdAt = "2024-01-02 03:04:30";
  const std::vector<ResultRecordSummary> modern{
      makeModernChartResultRecord(modernRecord, replay::ReplayState::Missing,
                                  true),
      makeLegacyChartResultRecord(legacy)};
  const std::vector<ReplaySummary> autoPlay{autoPlayRecord()};
  const std::vector<ir::IrRemoteScore> remote;

  const auto merged = mergeResultRecords(autoPlay, modern, remote, "tachi",
                                         "https://boku.tachi.ac");
  const auto mergedModern = std::ranges::find_if(
      merged, [](const ResultRecordSummary &record) {
        return record.isModernChart();
      });
  expect(merged.size() == 3 && mergedModern != merged.end() &&
             mergedModern->modernAttemptId() == modernResult.attemptId,
         "Records merge includes modern results and legacy summaries");
  expect(std::ranges::any_of(merged,
                             [](const ResultRecordSummary &record) {
                               return record.isLegacyChart() &&
                                      record.capabilities ==
                                          ResultRecordCapabilities{};
                             }),
         "merged legacy summary remains Records-only");
  expect(mergedModern != merged.end() &&
             mergedModern->capabilities.resultRecall &&
             mergedModern->capabilities.irUpload &&
             !mergedModern->capabilities.watch,
         "Records merge preserves file-independent modern capabilities");

  ReplaySummary legacyReplay;
  legacyReplay.id = 44;
  expectInvalid(
      [&] {
        static_cast<void>(mergeResultRecords(
            std::span<const ReplaySummary>(&legacyReplay, 1), modern, remote,
            "tachi", "https://boku.tachi.ac"));
      },
      "Records merge rejects positive replay summaries at its public boundary");
}

void testMergeSortsNewestWithAutoPlayFirstAndStableTies() {
  const std::vector<ReplaySummary> autoPlay{autoPlayRecord()};
  const std::vector<ResultRecordSummary> projected{
      modernRecord("newest", 1'704'164'701'000LL),
      modernRecord("tied-b", 1'704'164'700'000LL),
      modernRecord("tied-a", 1'704'164'700'000LL),
      modernRecord("oldest", 1'704'164'644'000LL)};

  ir::IrRemoteScore achieved = validRemoteScore();
  achieved.remoteScoreId = "achieved";
  achieved.timeAchievedUnixMillis = 1'704'164'700'123LL;
  achieved.timeAddedUnixMillis = 1'704'164'900'000LL;
  ir::IrRemoteScore fallback = validRemoteScore();
  fallback.remoteScoreId = "fallback";
  fallback.timeAchievedUnixMillis.reset();
  fallback.timeAddedUnixMillis = 1'704'164'645'456LL;

  const std::vector<ir::IrRemoteScore> remote{fallback, achieved};
  const auto merged = mergeResultRecords(
      autoPlay, projected, remote, "tachi", "https://boku.tachi.ac");

  expect(merged.size() == 7 && merged[0].autoPlay,
         "Auto Play remains first under newest sorting");
  expect(merged[1].modernAttemptId() == "newest" &&
             merged[2].remoteScoreId() == "achieved",
         "remote achieved time sorts alongside newer modern timestamps");
  expect(merged[3].stableKey() < merged[4].stableKey() &&
             merged[3].displayedTimeUnixMillis ==
                 merged[4].displayedTimeUnixMillis,
         "equal newest timestamps use deterministic stable-key ordering");
  expect(merged[5].remoteScoreId() == "fallback" &&
             merged[6].modernAttemptId() == "oldest",
         "remote added-time fallback sorts alongside older modern timestamps");
}

} // namespace

int main() {
  testRecordActionsRequireTypedIdentityAndPayloadAgreement();
  testReplayFileActionsUseModernIdentityAndCapabilitiesOnly();
  testAutoPlayIsTheOnlyReplaySummaryBackedRecord();
  testModernConversionUsesSharedReplayCapabilities();
  testModernCourseConversionKeepsResultWithoutReplay();
  testLegacySummariesExposeRecordsOnly();
  testRemoteConversionIsReadOnlyAndRetainsOptionalValues();
  testRemoteConversionFailsClosed();
  testIdentityEqualityHashAndStableKeys();
  testMergeIncludesModernResultsWithoutChangingTheirCapabilities();
  testMergeSortsNewestWithAutoPlayFirstAndStableTies();

  if (failures != 0) {
    std::cerr << failures << " result record summary assertion(s) failed\n";
    return 1;
  }
  std::cout << "result record summary tests passed\n";
  return 0;
}
