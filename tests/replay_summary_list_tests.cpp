#include "../src/ReplaySummaryFormatting.h"
#include "../src/ReplayResultStateBuilder.h"
#include "../src/ReplayAutoPlay.h"

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
  ReplayData practiceGaugeReplay;
  practiceGaugeReplay.initialGaugeType = GaugeType::Normal;
  practiceGaugeReplay.provenance.startingGaugePercent = 37;
  const RhythmState practiceGaugeState =
      replay_result::BuildResultState(chart, practiceGaugeReplay);
  if (practiceGaugeState.currentGauge != 37.0f) {
    std::cerr << "export result state must restore the recorded starting gauge"
              << std::endl;
    return 1;
  }

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

  const ReplayData autoExport = replay_autoplay::BuildReplayData(
      chart, GaugeType::Normal, GaugeAutoShiftMode::None, {.percent = 200},
      std::nullopt, std::nullopt, std::nullopt, std::nullopt,
      assist_options::kOff, true, GaugeType::Hard);
  if (autoExport.provenance.playback.percent != 200) {
    std::cerr << "synthetic Auto export retains the selected playback rate"
              << std::endl;
    return 1;
  }
  if (!autoExport.provenance.clubMode) {
    std::cerr << "synthetic Auto export retains Club Beat" << std::endl;
    return 1;
  }
  if (autoExport.gaugeAutoShiftLowerBound != GaugeType::Hard) {
    std::cerr << "synthetic Auto export retains the GAS lower bound"
              << std::endl;
    return 1;
  }

  const ReplaySummary assistedAutoSummary = replay_autoplay::BuildSummary(
      chart.Meta, GaugeType::Normal, GaugeAutoShiftMode::None, std::nullopt,
      std::nullopt, std::nullopt, std::nullopt, assist_options::kOff,
      {.percent = 200});
  if (assistedAutoSummary.playback.percent != 200 ||
      assistedAutoSummary.clearType != kClearTypeAssistedEasyClearRank) {
    std::cerr << "synthetic Auto summary must retain playback assist limits"
              << std::endl;
    return 1;
  }

  return 0;
}
