#pragma once

#include "scene/play/GameplayGaugeTypes.h"

// Song lamps map only the no-play sentinel to 0; other ranks use thresholds.
// Result-skin image indices and ranking fallbacks follow different policies.
[[nodiscard]] inline int beatorajaSongClearType(int rank) noexcept {
  if (rank == kNoClearTypeRank) return 0;
  if (rank >= kClearTypeFullComboRank) return 8;
  if (rank >= kClearTypeExHardClearRank) return 7;
  if (rank >= kClearTypeHardClearRank) return 6;
  if (rank >= kClearTypeNormalClearRank) return 5;
  if (rank >= kClearTypeEasyClearRank) return 4;
  if (rank >= kClearTypeLightAssistedEasyClearRank) return 3;
  if (rank >= kClearTypeAssistedEasyClearRank) return 2;
  return 1;
}
