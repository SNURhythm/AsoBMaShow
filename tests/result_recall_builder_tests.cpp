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
  record.replay.chartMeta.MD5 = "0123456789abcdef0123456789abcdef";
  record.replay.chartMeta.SHA256 =
      "0123456789abcdef0123456789abcdef"
      "0123456789abcdef0123456789abcdef";
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
  assert(outcome.value->historicalIr->saveOutcome.saved());
}

void testInvalidIntegrityMetadataSuppressesOnlyIr() {
  for (int variant = 0; variant < 4; ++variant) {
    auto record = validRecord();
    if (variant == 0) {
      record.attemptId.reset();
    }
    if (variant == 1) {
      record.attemptFingerprint.reset();
    }
    if (variant == 2) {
      record.attemptFingerprint = std::string(64, 'f');
    }
    if (variant == 3) {
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
    replay.stages.push_back({.replay = std::move(stage),
                             .restMicrosAfterStage = 500000});
  }
  replay.completedCharts = 3;
  replay.totalCharts = 3;
  replay.provenance = replay.stages.back().replay.provenance;

  std::atomic_bool cancelled = false;
  auto outcome =
      result_recall::BuildCourseResult(replay, cancelled, chartLoader());
  assert(outcome.value.has_value());
  const auto &session = outcome.value->session;
  assert(session->currentIndex == 0);
  assert(session->entries.size() == 3);
  assert(session->completedResults.size() == 3);
  assert(session->ownedResultBrowseCharts.size() == 3);
  assert(session->courseReplayData != nullptr);
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

} // namespace

int main() {
  testMatchingAttemptEnablesHistoricalIr();
  testInvalidIntegrityMetadataSuppressesOnlyIr();
  testCourseBuildPreparesEveryStage();
  testCourseBuildDoesNotPublishPartialSession();
  return 0;
}
