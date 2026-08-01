#pragma once

#include "repositories/ReplayRepository.h"
#include "scene/play/RhythmState.h"

#include <algorithm>

namespace replay_clear_mark {

inline int effectiveClearRank(
    int clearTypeRank, int maxCombo, int maxScore,
    const audio::PlaybackRate &playback = audio::PlaybackRate{}) {
  bool fullCombo = clearTypeRank >= kClearTypeFullComboRank;
  if (!fullCombo && clearTypeRank >= kClearTypeAssistedEasyClearRank &&
      maxScore > 0) {
    const int totalNotes = maxScore / 2;
    if (totalNotes > 0 && maxCombo >= totalNotes) {
      fullCombo = true;
    }
  }
  return clear_policy::fullComboRankForPlayback(clearTypeRank, fullCombo,
                                                playback);
}

inline int effectiveClearRank(const ReplaySummary &summary) {
  return effectiveClearRank(summary.clearType, summary.maxCombo,
                            summary.maxScore, summary.playback);
}

inline int effectiveClearRank(
    const result_persistence::ChartScoreWrite &score) {
  const bool fullCombo = score.comboBreak == 0 && score.maxScore > 0 &&
                         score.maxScore % 2 == 0 &&
                         score.maxCombo >= score.maxScore / 2;
  return clear_policy::fullComboRankForPlayback(
      score.clearType, fullCombo, score.provenance.playback);
}

} // namespace replay_clear_mark
