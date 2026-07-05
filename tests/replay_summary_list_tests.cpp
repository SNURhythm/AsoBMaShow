#include "../src/ReplaySummaryFormatting.h"

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

  return 0;
}
