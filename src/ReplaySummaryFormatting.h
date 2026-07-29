#pragma once

#include "PlayOptionUtils.h"
#include "repositories/ReplayRepository.h"

#include <iomanip>
#include <optional>
#include <sstream>
#include <string>

namespace replay_summary_ui {

struct DetailFacts {
  GaugeType initialGaugeType = GaugeType::Normal;
  GaugeAutoShiftMode gaugeAutoShift = GaugeAutoShiftMode::None;
  float finalGauge = 0.0F;
  std::optional<int> completedCharts;
  std::optional<int> totalCharts;
  bool automated = false;
  play_options::PlayModeDisplayLabel playMode;
  std::string assistOption = assist_options::kOff;
};

inline std::string formatGauge(float gauge) {
  std::ostringstream stream;
  stream << std::fixed << std::setprecision(1) << gauge << "%";
  return stream.str();
}

inline std::string gaugeLabel(GaugeType gaugeType,
                              GaugeAutoShiftMode autoShift) {
  return gaugeAutoShiftEnabled(autoShift)
             ? gaugeAutoShiftShortLabel(autoShift)
             : gaugeTypeToShortLabel(gaugeType);
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

inline std::string detailLabel(const DetailFacts &facts) {
  std::string detail =
      gaugeLabel(facts.initialGaugeType, facts.gaugeAutoShift) + "  Gauge " +
      formatGauge(facts.finalGauge);

  if (facts.completedCharts.has_value() && facts.totalCharts.has_value()) {
    detail += "  Course " + std::to_string(*facts.completedCharts) + "/" +
              std::to_string(*facts.totalCharts);
  }
  if (facts.automated) {
    detail += "  Automated";
  }

  if (!facts.playMode.mode.empty() && facts.playMode.mode != "NORMAL") {
    detail += "  " + facts.playMode.mode;
  }
  if (!facts.playMode.laneOrder.empty()) {
    detail += "  Lane " + facts.playMode.laneOrder;
  }
  if (assist_options::isEnabled(facts.assistOption)) {
    detail += "  Assist " + assist_options::normalize(facts.assistOption);
  }
  return detail;
}

inline std::string detailLabel(const ReplaySummary &summary) {
  return detailLabel({
      .initialGaugeType = summary.initialGaugeType,
      .gaugeAutoShift = summary.gaugeAutoShift,
      .finalGauge = summary.finalGauge,
      .completedCharts = summary.courseReplay
                             ? std::optional<int>(summary.completedCharts)
                             : std::nullopt,
      .totalCharts = summary.courseReplay
                         ? std::optional<int>(summary.totalCharts)
                         : std::nullopt,
      .automated = summary.autoPlay,
      .playMode = playModeDisplayLabel(summary),
      .assistOption = summary.assistOption,
  });
}

} // namespace replay_summary_ui
