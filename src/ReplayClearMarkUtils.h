#pragma once

#include "ReplayDBHelper.h"
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

} // namespace replay_clear_mark
