#pragma once

#include "PlayOptionUtils.h"
#include "ReplayDBHelper.h"

#include <iomanip>
#include <sstream>
#include <string>

namespace replay_summary_ui {

inline std::string formatGauge(float gauge) {
  std::ostringstream stream;
  stream << std::fixed << std::setprecision(1) << gauge << "%";
  return stream.str();
}

inline std::string gaugeLabel(GaugeType gaugeType, bool autoShift) {
  return autoShift ? "GAS" : gaugeTypeToShortLabel(gaugeType);
}

inline play_options::PlayModeDisplayLabel
playModeDisplayLabel(const ReplaySummary &summary) {
  if (summary.chartMeta.has_value()) {
    return play_options::formatPlayModeDisplayLabel(
        *summary.chartMeta, summary.playOption, summary.playOptionSeed,
        summary.playOption2, summary.playOption2Seed);
  }
  return {.mode = play_options::formatPlayOptionLabel(
              summary.playOption, summary.playOptionSeed, summary.playOption2,
              summary.playOption2Seed)};
}

inline std::string detailLabel(const ReplaySummary &summary) {
  std::string detail =
      gaugeLabel(summary.initialGaugeType, summary.gaugeAutoShift) +
      "  Gauge " + formatGauge(summary.finalGauge);

  if (summary.courseReplay) {
    detail += "  Course " + std::to_string(summary.completedCharts) + "/" +
              std::to_string(summary.totalCharts);
  }
  if (summary.autoPlay) {
    detail += "  Automated";
  }

  const play_options::PlayModeDisplayLabel display =
      playModeDisplayLabel(summary);
  if (!display.mode.empty() && display.mode != "NORMAL") {
    detail += "  " + display.mode;
  }
  if (!display.laneOrder.empty()) {
    detail += "  Lane " + display.laneOrder;
  }
  if (assist_options::isEnabled(summary.assistOption)) {
    detail += "  Assist " + assist_options::normalize(summary.assistOption);
  }
  return detail;
}

} // namespace replay_summary_ui
