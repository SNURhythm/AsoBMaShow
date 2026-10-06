#pragma once

namespace skin {

// Beatoraja's five-key SkinNote arrays are compact (six lanes per player).
// The parser, projections and input keep BMS channel IDs, with scratches at
// 7/15 and player two starting at 8. Other supported layouts share lane IDs.
[[nodiscard]] constexpr int skinNoteLaneForChartLane(int skinType,
                                                    int chartLane) noexcept {
  if (skinType != 1 && skinType != 3) return chartLane;
  if (chartLane >= 0 && chartLane < 5) return chartLane;
  if (chartLane == 7) return 5;
  if (skinType == 3) {
    if (chartLane >= 8 && chartLane < 13) return chartLane - 2;
    if (chartLane == 15) return 11;
  }
  return -1;
}

[[nodiscard]] constexpr int chartLaneForSkinNoteLane(int skinType,
                                                    int skinLane) noexcept {
  if (skinType != 1 && skinType != 3) return skinLane;
  if (skinLane >= 0 && skinLane < 5) return skinLane;
  if (skinLane == 5) return 7;
  if (skinType == 3) {
    if (skinLane >= 6 && skinLane < 11) return skinLane + 2;
    if (skinLane == 11) return 15;
  }
  return -1;
}

} // namespace skin
