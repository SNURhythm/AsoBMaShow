#pragma once
#include "ChartGoal.h"
#include "../kombo/KomboModel.h"
#include <iomanip>
#include <sstream>

namespace chart_goals {
struct TargetLabels {
  int lampRank = -1;
  std::string lamp;
  std::string score;
};
inline TargetLabels targetLabels(const Target &target) {
  TargetLabels result;
  result.lampRank = target.lamp;
  if (target.lamp >= 0) result.lamp = kombo::lampLabel(target.lamp);
  if (target.scoreNumerator > 0) {
    if (target.scoreDenominator == 27) result.score = kombo::scoreRankLabel(target.scoreNumerator);
    else {
      std::ostringstream rate;
      rate << std::fixed << std::setprecision(2)
           << 100.0 * target.scoreNumerator / target.scoreDenominator << '%';
      result.score = rate.str();
    }
  }
  return result;
}
} // namespace chart_goals
