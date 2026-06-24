#pragma once

#include "../rendering/Color.h"
#include "../scene/play/RhythmState.h"

inline bool hasClearLampColor(int clearTypeRank) {
  return clearTypeRank >= kClearTypeFailedRank;
}

inline Color clearLampColorForRank(int clearTypeRank) {
  if (clearTypeRank >= kClearTypeFullComboRank) {
    return Color(102, 218, 255, 242);
  }
  if (clearTypeRank >= kClearTypeExHardClearRank) {
    return Color(255, 205, 37, 242);
  }
  if (clearTypeRank >= kClearTypeHardClearRank) {
    return Color(244, 249, 255, 236);
  }
  if (clearTypeRank >= kClearTypeNormalClearRank) {
    return Color(63, 166, 255, 232);
  }
  if (clearTypeRank >= kClearTypeEasyClearRank) {
    return Color(66, 222, 97, 232);
  }
  if (clearTypeRank >= kClearTypeAssistedEasyClearRank) {
    return Color(184, 83, 255, 232);
  }
  return Color(232, 46, 55, 224);
}
