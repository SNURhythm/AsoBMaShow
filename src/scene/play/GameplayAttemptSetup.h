#pragma once

#include <cmath>

namespace gameplay {

inline constexpr int kMinimumJudgeWindowScalePercent = 25;
inline constexpr int kMaximumJudgeWindowScalePercent = 200;
inline constexpr int kJudgeWindowScaleStepPercent = 5;
inline constexpr int kMaximumStartingGaugePercent = 120;

[[nodiscard]] constexpr bool
validJudgeWindowScalePercent(int percent) noexcept {
  return percent >= kMinimumJudgeWindowScalePercent &&
         percent <= kMaximumJudgeWindowScalePercent &&
         percent % kJudgeWindowScaleStepPercent == 0;
}

[[nodiscard]] inline bool
validStartingGaugePercent(double percent) noexcept {
  return std::isfinite(percent) && percent >= 0.0 &&
         percent <= static_cast<double>(kMaximumStartingGaugePercent);
}

} // namespace gameplay
