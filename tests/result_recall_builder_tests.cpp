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

} // namespace

int main() {
  testMatchingAttemptEnablesHistoricalIr();
  testInvalidIntegrityMetadataSuppressesOnlyIr();
  return 0;
}
