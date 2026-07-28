#pragma once

#include "ReplaySummaryFormatting.h"
#include "ResultRecordSummary.h"
#include "ScoreRankUtils.h"
#include "scene/play/GameplayGaugeTypes.h"

#include <optional>
#include <string>
#include <utility>

namespace result_record_ui {
namespace detail {

inline void appendSegment(std::string &label, std::string segment) {
  if (!label.empty()) {
    label += "  ";
  }
  label += std::move(segment);
}

} // namespace detail

inline std::string detailLabel(const ResultRecordSummary &summary) {
  if (summary.isLegacyChart() || summary.isLegacyCourse()) {
    std::string label;
    if (summary.finalGauge.has_value()) {
      detail::appendSegment(
          label, "Gauge " + replay_summary_ui::formatGauge(
                                static_cast<float>(*summary.finalGauge)));
    }
    if (summary.maxCombo.has_value()) {
      detail::appendSegment(label,
                            "Combo " + std::to_string(*summary.maxCombo));
    }
    if (summary.completedCharts.has_value() &&
        summary.totalCharts.has_value()) {
      detail::appendSegment(
          label, "Course " + std::to_string(*summary.completedCharts) + "/" +
                     std::to_string(*summary.totalCharts));
    }
    return label.empty() ? "—" : label;
  }

  if (summary.autoPlayReplay.has_value()) {
    return replay_summary_ui::detailLabel(*summary.autoPlayReplay);
  }

  std::string label = "IR";
  if (summary.playOption.has_value() && !summary.playOption->empty()) {
    label += "  " + *summary.playOption;
  }
  return label;
}

inline std::string scoreLabel(const ResultRecordSummary &summary) {
  if (summary.autoPlay) {
    return "AUTO";
  }
  return summary.scoreAvailable ? std::to_string(summary.score) : "—";
}

inline std::optional<std::string>
scoreRank(const ResultRecordSummary &summary) {
  if (!summary.scoreAvailable || !summary.maxScoreAvailable ||
      summary.maxScore <= 0) {
    return std::nullopt;
  }
  return score_rank::labelForScore(summary.score, summary.maxScore);
}

inline std::string secondaryScoreLabel(const ResultRecordSummary &summary) {
  if (const auto rank = scoreRank(summary); rank.has_value()) {
    return score_rank::displayLabel(*rank);
  }
  return summary.clearRankAvailable ? clearTypeRankToLabel(summary.clearRank)
                                    : "—";
}

} // namespace result_record_ui
