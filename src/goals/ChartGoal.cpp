#include "ChartGoal.h"
#include <algorithm>
#include <array>
#include <stdexcept>

namespace chart_goals {
void validate(const Target &target) {
  constexpr std::array lamps{-1, 100, 150, 200, 300, 400, 500, 600};
  if (std::find(lamps.begin(), lamps.end(), target.lamp) == lamps.end() ||
      target.scoreDenominator < 1 || target.scoreDenominator > 10000 ||
      target.scoreNumerator < 0 || target.scoreNumerator > target.scoreDenominator ||
      (target.lamp == -1 && target.scoreNumerator == 0))
    throw std::invalid_argument("Choose a valid lamp or score target.");
}
std::string normalizedHash(std::string_view hash) {
  if (hash.size() != 64) throw std::invalid_argument("A chart goal needs a SHA-256 identity.");
  std::string result(hash);
  for (auto &c : result) {
    if (c >= 'A' && c <= 'F') c += 'a' - 'A';
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
      throw std::invalid_argument("Invalid chart SHA-256 identity.");
  }
  return result;
}
Progress evaluate(const Target &target, std::optional<ScoreBestSnapshot> bestScore,
                  std::optional<ScoreBestSnapshot> bestClear) {
  validate(target);
  Progress result{.bestScore = std::move(bestScore), .bestClear = std::move(bestClear)};
  result.lampMet = target.lamp < 0 || (result.bestClear && result.bestClear->clearType >= target.lamp);
  result.scoreMet = target.scoreNumerator == 0;
  if (target.scoreNumerator > 0 && result.bestScore && result.bestScore->maxScore > 0 &&
      result.bestScore->score >= 0 && result.bestScore->score <= result.bestScore->maxScore) {
    const auto required = (std::int64_t(result.bestScore->maxScore) * target.scoreNumerator +
        target.scoreDenominator - 1) / target.scoreDenominator;
    result.remainingEx = static_cast<int>(std::max<std::int64_t>(0, required - result.bestScore->score));
    result.scoreMet = *result.remainingEx == 0;
  }
  result.achieved = result.lampMet && result.scoreMet;
  return result;
}
} // namespace chart_goals
