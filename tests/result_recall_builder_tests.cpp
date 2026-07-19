#include "../src/ResultRecallBuilder.h"
#include "../src/ResultPersistenceModel.h"
#include "../src/ReplayResultStateBuilder.h"

#include <atomic>
#include <cassert>
#include <memory>

namespace {

constexpr const char *kAttemptId =
    "123e4567-e89b-42d3-a456-426614174000";

ReplayResultRecord validRecord() {
  ReplayResultRecord record;
  record.replay.id = 41;
  record.replay.chartMeta.BmsPath = "recall.bms";
  record.replay.chartMeta.Folder = "charts/recall";
  record.replay.chartMeta.MD5 = "0123456789abcdef0123456789abcdef";
  record.replay.chartMeta.SHA256 =
      "0123456789abcdef0123456789abcdef"
      "0123456789abcdef0123456789abcdef";
  record.replay.chartMeta.Artist = "Recall Artist";
  record.replay.chartMeta.SubArtist = "Recall Subartist";
  record.replay.chartMeta.Bpm = 173.5;
  record.replay.chartMeta.Genre = "Recall Genre";
  record.replay.chartMeta.Title = "Recall Title";
  record.replay.chartMeta.SubTitle = "Recall Subtitle";
  record.replay.chartMeta.Rank = 2;
  record.replay.chartMeta.Total = 165.25;
  record.replay.chartMeta.HasTotal = true;
  record.replay.chartMeta.PlayLength = 1'234'567;
  record.replay.chartMeta.TotalLength = 2'345'678;
  record.replay.chartMeta.Banner = "banner.png";
  record.replay.chartMeta.StageFile = "stage.png";
  record.replay.chartMeta.BackBmp = "back.png";
  record.replay.chartMeta.Preview = "preview.ogg";
  record.replay.chartMeta.Difficulty = 4;
  record.replay.chartMeta.PlayLevel = 11.5;
  record.replay.chartMeta.MinBpm = 86.75;
  record.replay.chartMeta.MaxBpm = 347.0;
  record.replay.chartMeta.KeyMode = 7;
  record.replay.chartMeta.TotalNotes = 1;
  record.replay.initialGaugeType = GaugeType::Normal;
  record.replay.finalScore = 2;
  record.replay.maxCombo = 1;
  record.replay.finalGauge = 100.0F;
  record.replay.clearType = kClearTypeFullComboRank;
  record.replay.provenance = ScoreProvenance::Legacy();
  record.replay.provenance.schemaVersion = ScoreProvenance::kSchemaVersion;
  record.replay.provenance.ruleset =
      RulesetDescriptor::For(GameplayRuleset::LR2);
  record.replay.provenance.gaugeType = GaugeType::Normal;
  record.replay.provenance.eligibility = ScoreEligibility::Verified;
  record.replay.events.push_back(
      {.action = ReplayEventAction::Press,
       .lane = 0,
       .noteTimeMicros = 1000,
       .songTimeMicros = 1000,
       .judgeTimeMicros = 1000,
       .judgement = PGreat,
       .diffMicros = 0,
       .gauge = 100.0F,
       .gaugeType = GaugeType::Normal,
       .combo = 1,
       .score = 2});
  record.attemptId = kAttemptId;
  record.playedAtUnixMillis = 1784420645000LL;

  bms_parser::Chart chart;
  chart.Meta = record.replay.chartMeta;
  RhythmState state = replay_result::BuildResultState(chart, record.replay);
  std::string diagnostic;
  auto attempt = result_persistence::makeChartResultAttempt(
      kAttemptId, chart.Meta, state, record.replay.provenance,
      record.replay.chartMeta.LnMode, record.replay, diagnostic);
  assert(attempt.has_value());
  record.attemptFingerprint = attempt->payloadFingerprint;
  return record;
}

bms_parser::ChartMeta
databaseShapedMeta(const bms_parser::ChartMeta &complete) {
  bms_parser::ChartMeta stored;
  stored.BmsPath = complete.BmsPath;
  stored.MD5 = complete.MD5;
  stored.SHA256 = complete.SHA256;
  stored.Title = complete.Title;
  stored.Artist = complete.Artist;
  stored.KeyMode = complete.KeyMode;
  stored.TotalNotes = complete.TotalNotes;
  stored.LnMode = complete.LnMode;
  return stored;
}

result_recall::ReplayChartLoader chartLoader() {
  return [](const ReplayData &replay, std::atomic_bool &) {
    auto chart = std::make_unique<bms_parser::Chart>();
    chart->Meta = replay.chartMeta;
    return chart;
  };
}

void testMatchingAttemptEnablesHistoricalIr() {
  std::atomic_bool cancelled = false;
  auto outcome = result_recall::BuildChartResult(
      validRecord(), cancelled, chartLoader());
  assert(outcome.value.has_value());
  assert(outcome.value->historicalIr.has_value());
  assert(outcome.value->historicalIr->attempt->attemptId == kAttemptId);
  assert(outcome.value->historicalIr->submission->playedAtUnixMillis ==
         1784420645000LL);
  assert(outcome.value->historicalIr->submission
             ->judgementTimingBreakdownAvailable);
  assert(outcome.value->historicalIr->submission->earlyPGreat == 1);
  assert(outcome.value->historicalIr->submission->latePGreat == 0);
  assert(outcome.value->historicalIr->saveOutcome.saved());
}

void testParsedChartMetadataRestoresHistoricalIr() {
  auto record = validRecord();
  const auto completeMeta = record.replay.chartMeta;
  record.replay.chartMeta = databaseShapedMeta(completeMeta);
  result_recall::ReplayChartLoader parsedChartLoader =
      [completeMeta](const ReplayData &, std::atomic_bool &) {
        auto chart = std::make_unique<bms_parser::Chart>();
        chart->Meta = completeMeta;
        return chart;
      };

  std::atomic_bool cancelled = false;
  auto outcome = result_recall::BuildChartResult(
      std::move(record), cancelled, std::move(parsedChartLoader));
  assert(outcome.value.has_value());
  assert(outcome.value->historicalIr.has_value());
  assert(outcome.value->replay.chartMeta.Genre == completeMeta.Genre);
  assert(outcome.value->replay.chartMeta.Total == completeMeta.Total);
}

void testParsedChartMetadataStillRejectsChangedChart() {
  auto record = validRecord();
  auto changedMeta = record.replay.chartMeta;
  record.replay.chartMeta = databaseShapedMeta(changedMeta);
  changedMeta.Genre = "Changed Genre";
  result_recall::ReplayChartLoader changedChartLoader =
      [changedMeta](const ReplayData &, std::atomic_bool &) {
        auto chart = std::make_unique<bms_parser::Chart>();
        chart->Meta = changedMeta;
        return chart;
      };

  std::atomic_bool cancelled = false;
  auto outcome = result_recall::BuildChartResult(
      std::move(record), cancelled, std::move(changedChartLoader));
  assert(outcome.value.has_value());
  assert(!outcome.value->historicalIr.has_value());
}

void testWellFormedWrongFingerprintSuppressesHistoricalIrSubmission() {
  auto record = validRecord();
  const std::string validFingerprint = *record.attemptFingerprint;
  record.attemptFingerprint = std::string(64, 'f');
  assert(record.attemptFingerprint->size() == 64);
  assert(*record.attemptFingerprint != validFingerprint);

  std::atomic_bool cancelled = false;
  auto outcome = result_recall::BuildChartResult(
      std::move(record), cancelled, chartLoader());

  assert(outcome.value.has_value());
  assert(!outcome.value->historicalIr.has_value());
}

void testChartBuildRejectsMismatchedPersistedOutcome() {
  auto record = validRecord();
  auto changedMeta = record.replay.chartMeta;
  changedMeta.TotalNotes = 2;
  result_recall::ReplayChartLoader changedChartLoader =
      [changedMeta](const ReplayData &, std::atomic_bool &) {
        auto chart = std::make_unique<bms_parser::Chart>();
        chart->Meta = changedMeta;
        return chart;
      };

  std::atomic_bool cancelled = false;
  auto outcome = result_recall::BuildChartResult(
      std::move(record), cancelled, std::move(changedChartLoader));
  assert(!outcome.value.has_value());
  assert(outcome.diagnostic == "saved chart outcome does not match");
}

void testInvalidIntegrityMetadataSuppressesOnlyIr() {
  for (int variant = 0; variant < 3; ++variant) {
    auto record = validRecord();
    if (variant == 0) {
      record.attemptId.reset();
    }
    if (variant == 1) {
      record.attemptFingerprint.reset();
    }
    if (variant == 2) {
      record.playedAtUnixMillis = 0;
    }
    std::atomic_bool cancelled = false;
    auto outcome = result_recall::BuildChartResult(
        std::move(record), cancelled, chartLoader());
    assert(outcome.value.has_value());
    assert(!outcome.value->historicalIr.has_value());
  }
}

void testCourseBuildPreparesEveryStage() {
  CourseReplayData replay;
  replay.id = 9;
  replay.courseName = "Recall Course";
  replay.courseGroupName = "Records";
  replay.constraintJson = "{}";
  replay.gaugeProfile = GaugeProfile::Standard;
  replay.initialGaugeType = GaugeType::Normal;
  for (int index = 0; index < 3; ++index) {
    auto stage = validRecord().replay;
    stage.id = 100 + index;
    stage.chartMeta.Title = "Stage " + std::to_string(index + 1);
    stage.events.front().combo = index + 1;
    stage.maxCombo = index + 1;
    replay.stages.push_back({.replay = std::move(stage),
                             .restMicrosAfterStage = 500000});
  }
  replay.completedCharts = 3;
  replay.totalCharts = 3;
  replay.provenance = replay.stages.back().replay.provenance;

  result_recall::ReplayChartLoader resolvedLoader =
      [](const ReplayData &stage, std::atomic_bool &) {
        auto chart = std::make_unique<bms_parser::Chart>();
        chart->Meta = stage.chartMeta;
        chart->Meta.BmsPath =
            std::filesystem::path("resolved") / stage.chartMeta.BmsPath;
        return chart;
      };
  std::atomic_bool cancelled = false;
  auto outcome = result_recall::BuildCourseResult(
      replay, cancelled, std::move(resolvedLoader));
  assert(outcome.value.has_value());
  const auto &session = outcome.value->session;
  assert(session->currentIndex == 0);
  assert(session->entries.size() == 3);
  assert(session->completedResults.size() == 3);
  assert(session->ownedResultBrowseCharts.size() == 3);
  assert(session->courseReplayData != nullptr);
  assert(session->courseReplayData->stages.front()
             .replay.chartMeta.BmsPath ==
         std::filesystem::path("resolved") / "recall.bms");
  assert(!session->courseReplayPlayback);
}

void testCourseBuildDoesNotPublishPartialSession() {
  CourseReplayData replay;
  replay.courseName = "Broken Course";
  replay.completedCharts = 2;
  replay.totalCharts = 2;
  replay.stages.push_back({.replay = validRecord().replay});
  replay.stages.push_back({.replay = validRecord().replay});
  int calls = 0;
  result_recall::ReplayChartLoader failingLoader =
      [&calls](const ReplayData &stage, std::atomic_bool &) {
        ++calls;
        if (calls == 2) {
          return std::unique_ptr<bms_parser::Chart>{};
        }
        auto chart = std::make_unique<bms_parser::Chart>();
        chart->Meta = stage.chartMeta;
        return chart;
      };
  std::atomic_bool cancelled = false;
  auto outcome = result_recall::BuildCourseResult(
      replay, cancelled, std::move(failingLoader));
  assert(!outcome.value.has_value());
  assert(calls == 2);
}

void testCourseBuildCarriesComboSnapshotBetweenStages() {
  CourseReplayData replay;
  replay.id = 10;
  replay.courseName = "Combo Carry Course";
  replay.courseGroupName = "Records";
  replay.constraintJson = "{}";
  replay.gaugeProfile = GaugeProfile::Standard;
  replay.initialGaugeType = GaugeType::Normal;

  auto first = validRecord().replay;
  first.id = 201;
  auto second = validRecord().replay;
  second.id = 202;
  second.events.clear();
  second.events.push_back(
      {.action = ReplayEventAction::Press,
       .lane = 0,
       .noteTimeMicros = 1000,
       .songTimeMicros = 1000,
       .judgeTimeMicros = 1000,
       .judgement = Bad,
       .diffMicros = 0,
       .gauge = 90.0F,
       .gaugeType = GaugeType::Normal,
       .combo = 0,
       .score = 0});
  second.finalScore = 0;
  second.maxCombo = first.maxCombo;
  second.finalGauge = 90.0F;
  second.clearType = kClearTypeNormalClearRank;

  replay.stages.push_back({.replay = std::move(first)});
  replay.stages.push_back({.replay = std::move(second)});
  replay.completedCharts = 2;
  replay.totalCharts = 2;
  replay.provenance = replay.stages.back().replay.provenance;

  std::atomic_bool cancelled = false;
  auto outcome = result_recall::BuildCourseResult(
      std::move(replay), cancelled, chartLoader());
  assert(outcome.value.has_value());
  assert(outcome.value->session->completedResults.size() == 2);
  assert(outcome.value->session->completedResults.back().state.maxCombo == 1);
}

} // namespace

int main() {
  testMatchingAttemptEnablesHistoricalIr();
  testParsedChartMetadataRestoresHistoricalIr();
  testParsedChartMetadataStillRejectsChangedChart();
  testWellFormedWrongFingerprintSuppressesHistoricalIrSubmission();
  testChartBuildRejectsMismatchedPersistedOutcome();
  testInvalidIntegrityMetadataSuppressesOnlyIr();
  testCourseBuildPreparesEveryStage();
  testCourseBuildDoesNotPublishPartialSession();
  testCourseBuildCarriesComboSnapshotBetweenStages();
  return 0;
}
