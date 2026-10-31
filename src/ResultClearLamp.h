#pragma once

#include "ModernResult.h"

// Effective display/projection lamps. Never use these values to rewrite the
// canonical result payload or fingerprint.
namespace result_clear_lamp {

inline int effectiveRank(
    const result_persistence::ChartScoreWrite &score) {
  const bool fullCombo = score.comboBreak == 0 && score.maxScore > 0 &&
      score.maxScore % 2 == 0 && score.maxCombo >= score.maxScore / 2;
  const int rank = clear_policy::fullComboRankForPlayback(
      score.clearType, fullCombo, score.provenance.playback);
  if (!fullCombo || !(score.finalGauge > 0.0F) ||
      static_cast<long long>(score.pGreat) * 2 + score.great != score.score) return rank;
  return clear_policy::completedClearRank(
      rank, score.maxScore / 2, score.pGreat, score.great, score.good,
      score.bad, score.poor, score.comboBreak, score.provenance.playback);
}

inline int effectiveRank(const result_persistence::ModernCourseResult &result) {
  const int rank = normalizeClearLamp(result.clearType);
  // Old aggregate captures did not retain the stage assist flags. Stage lamps
  // are still authenticated evidence and must cap the course before promotion.
  bool lightAssist = false;
  bool assist = false;
  for (const auto &stage : result.stages) {
    const int stageRank = normalizeClearLamp(stage.score.clearType);
    lightAssist |= stageRank == kClearTypeLightAssistedEasyClearRank;
    assist |= stageRank == kClearTypeAssistedEasyClearRank &&
              assist_options::isEnabled(stage.score.provenance.assistOption);
  }
  if (rank > kClearTypeFailedRank && (assist || lightAssist)) {
    return clear_policy::capRankForPlayback(assist ? kClearTypeAssistedEasyClearRank
        : kClearTypeLightAssistedEasyClearRank, result.provenance.playback);
  }
  if (result.completedCharts != result.totalCharts || result.totalCharts <= 0 ||
      result.stages.size() != static_cast<std::size_t>(result.totalCharts) ||
      result.maxScore <= 0 || result.maxScore % 2 != 0 ||
      result.maxCombo < result.maxScore / 2 || !(result.finalGauge > 0.0F)) return rank;
  long long pg = 0, gr = 0, gd = 0, notes = 0;
  for (const auto &stage : result.stages) {
    const auto &score = stage.score;
    if (score.maxScore <= 0 || score.maxScore % 2 != 0 || score.comboBreak != 0 ||
        score.bad != 0 || score.poor != 0 || score.pGreat < 0 || score.great < 0 ||
        score.good < 0 || static_cast<long long>(score.pGreat) + score.great +
            score.good != score.maxScore / 2 ||
        static_cast<long long>(score.pGreat) * 2 + score.great != score.score) return rank;
    pg += score.pGreat; gr += score.great; gd += score.good;
    notes += score.maxScore / 2;
  }
  if (notes != result.maxScore / 2 || pg * 2 + gr != result.finalScore) return rank;
  return clear_policy::completedClearRank(rank, result.maxScore / 2,
      static_cast<int>(pg), static_cast<int>(gr), static_cast<int>(gd),
      0, 0, 0, result.provenance.playback);
}

} // namespace result_clear_lamp
