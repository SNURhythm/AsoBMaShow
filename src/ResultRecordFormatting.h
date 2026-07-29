#pragma once

#include "ReplaySummaryFormatting.h"
#include "ResultRecordSummary.h"
#include "ScoreRankUtils.h"
#include "scene/play/GameplayGaugeTypes.h"

#include <cstdint>
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

inline std::optional<long long>
displaySeed(const std::optional<std::int64_t> &seed) {
  return seed.has_value() ? std::optional<long long>(*seed) : std::nullopt;
}

inline play_options::PlayModeDisplayLabel playMode(
    const std::string &player1Option,
    const std::optional<std::int64_t> &player1Seed,
    const std::string &player2Option,
    const std::optional<std::int64_t> &player2Seed) {
  return {.mode = play_options::formatPlayOptionLabel(
              std::optional<std::string>(player1Option),
              displaySeed(player1Seed),
              std::optional<std::string>(player2Option),
              displaySeed(player2Seed))};
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

  if (summary.modern.has_value()) {
    const auto &result = summary.modern->result;
    const auto &provenance = result.score.provenance;
    return replay_summary_ui::detailLabel({
        .initialGaugeType = provenance.gaugeType,
        .gaugeAutoShift = provenance.gaugeAutoShift,
        .finalGauge = result.score.finalGauge,
        .maxCombo = result.score.maxCombo,
        .playMode = detail::playMode(
            provenance.player1.option, provenance.player1.seed,
            provenance.player2.option, provenance.player2.seed),
        .assistOption = provenance.assistOption,
    });
  }

  if (summary.modernCourse.has_value()) {
    const auto &result = summary.modernCourse->result;
    const auto &provenance = result.provenance;
    return replay_summary_ui::detailLabel({
        .initialGaugeType = result.initialGaugeType,
        .gaugeAutoShift = result.gaugeAutoShift,
        .finalGauge = result.finalGauge,
        .maxCombo = result.maxCombo,
        .completedCharts = result.completedCharts,
        .totalCharts = result.totalCharts,
        .playMode = detail::playMode(
            result.requestedPlayOption, provenance.player1.seed,
            provenance.player2.option, provenance.player2.seed),
        .assistOption = result.assistOption,
    });
  }

  if (summary.isRemote()) {
    std::string label = "IR";
    if (summary.playOption.has_value() && !summary.playOption->empty()) {
      label += "  " + *summary.playOption;
    }
    return label;
  }
  return "—";
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
