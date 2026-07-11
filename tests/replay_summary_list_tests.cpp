#include "../src/ReplaySummaryFormatting.h"
#include "../src/ReplayResultStateBuilder.h"

#include <iostream>

#define ASSERT_CONTAINS(haystack, needle, label)                               \
  if ((haystack).find(needle) == std::string::npos) {                          \
    std::cerr << label << " expected to contain " << (needle) << " in "        \
              << (haystack) << std::endl;                                      \
    return 1;                                                                  \
  }

#define ASSERT_NOT_CONTAINS(haystack, needle, label)                           \
  if ((haystack).find(needle) != std::string::npos) {                          \
    std::cerr << label << " expected not to contain " << (needle) << " in "    \
              << (haystack) << std::endl;                                      \
    return 1;                                                                  \
  }

namespace {
bms_parser::ChartMeta makeSevenKeyMeta() {
  bms_parser::ChartMeta meta;
  meta.KeyMode = 7;
  meta.IsDP = false;
  return meta;
}
} // namespace

int main() {
  ReplaySummary summary;
  summary.initialGaugeType = GaugeType::Hard;
  summary.finalGauge = 78.25f;
  summary.eventCount = 1234;
  summary.touchSampleCount = 5678;
  summary.playOption = "MIRROR";
  summary.chartMeta = makeSevenKeyMeta();

  const std::string detail = replay_summary_ui::detailLabel(summary);

  ASSERT_CONTAINS(detail, "HARD", "gauge type");
  ASSERT_CONTAINS(detail, "Gauge 78.2%", "final gauge");
  ASSERT_CONTAINS(detail, "MIRROR", "play option");
  ASSERT_CONTAINS(detail, "Lane S7654321", "lane order");
  ASSERT_NOT_CONTAINS(detail, "Events", "events count");
  ASSERT_NOT_CONTAINS(detail, "Touches", "touch count");

  ReplaySummary normalSummary;
  normalSummary.initialGaugeType = GaugeType::Normal;
  normalSummary.finalGauge = 12.0f;
  normalSummary.chartMeta = makeSevenKeyMeta();
  const std::string normalDetail =
      replay_summary_ui::detailLabel(normalSummary);
  ASSERT_NOT_CONTAINS(normalDetail, "Lane", "normal lane order");

  bms_parser::Chart chart;
  chart.Meta.TotalNotes = 1;
  chart.Meta.Total = 100.0;
  ReplayData assistedReplay;
  assistedReplay.initialGaugeType = GaugeType::Hard;
  assistedReplay.maxCombo = 1;
  assistedReplay.finalGauge = 100.0f;
  assistedReplay.clearType = kClearTypeFullComboRank;
  assistedReplay.provenance.playback = {
      .percent = 75, .mode = audio::PlaybackMode::PitchShift};
  assistedReplay.provenance.eligibility = ScoreEligibility::Modified;
  assistedReplay.events.push_back({.action = ReplayEventAction::Release,
                                   .judgement = PGreat,
                                   .gauge = 100.0f,
                                   .gaugeType = GaugeType::Hard,
                                   .combo = 1,
                                   .score = 2});
  const RhythmState assistedState =
      replay_result::BuildResultState(chart, assistedReplay);
  if (assistedState.getClearTypeRank() != kClearTypeAssistedEasyClearRank) {
    std::cerr << "export result state must retain playback assist cap"
              << std::endl;
    return 1;
  }

  ReplayData neutralReplay = assistedReplay;
  neutralReplay.provenance = ScoreProvenance::Legacy();
  const RhythmState neutralState =
      replay_result::BuildResultState(chart, neutralReplay);
  if (neutralState.getClearTypeRank() != kClearTypeHardClearRank) {
    std::cerr << "legacy neutral export result state remains unassisted"
              << std::endl;
    return 1;
  }

  return 0;
}
