#pragma once

#include "ReplayDBHelper.h"
#include "scene/play/RhythmState.h"

#include <algorithm>

namespace replay_clear_mark {

inline int effectiveClearRank(int clearTypeRank, int maxCombo, int maxScore) {
  if (clearTypeRank >= kClearTypeFullComboRank) {
    return clearTypeRank;
  }
  if (clearTypeRank < kClearTypeAssistedEasyClearRank || maxScore <= 0) {
    return clearTypeRank;
  }

  const int totalNotes = maxScore / 2;
  if (totalNotes > 0 && maxCombo >= totalNotes) {
    return kClearTypeFullComboRank;
  }
  return clearTypeRank;
}

inline int effectiveClearRank(const ReplaySummary &summary) {
  return effectiveClearRank(summary.clearType, summary.maxCombo,
                            summary.maxScore);
}

} // namespace replay_clear_mark
