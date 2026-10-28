#include "ChartGoal.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <stdexcept>

namespace chart_goals {
std::int64_t nowUnixMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string localDateTime(std::int64_t unixMillis) {
  if (unixMillis <= 0) return {};
  const auto seconds = static_cast<std::time_t>(unixMillis / 1000);
  std::tm local{};
#ifdef _WIN32
  if (localtime_s(&local, &seconds) != 0) return {};
#else
  if (localtime_r(&seconds, &local) == nullptr) return {};
#endif
  char text[32]{};
  if (!std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &local)) return {};
  return text;
}
std::vector<std::string> Index::hashes() const {
  std::vector<std::string> result;
  result.reserve(modes.size());
  for (const auto &[hash, mask] : modes) result.push_back(hash);
  return result;
}
std::vector<std::string> Index::activeHashes() const {
  std::vector<std::string> result;
  for (const auto &[hash, mask] : modes)
    if (!allAchieved(hash)) result.push_back(hash);
  return result;
}
bool Index::allAchieved(std::string_view sha256) const {
  std::string hash;
  try { hash = normalizedHash(sha256); }
  catch (const std::invalid_argument &) { return false; }
  const auto saved = modes.find(hash), achieved = achievedModes.find(hash);
  return saved != modes.end() && achieved != achievedModes.end() &&
      (saved->second & achieved->second) == saved->second;
}
std::optional<int> Index::modeFor(std::string_view sha256, int preferredMode) const {
  std::string hash;
  try { hash = normalizedHash(sha256); }
  catch (const std::invalid_argument &) { return {}; }
  const auto found = modes.find(hash);
  if (found == modes.end()) return {};
  if (preferredMode >= 0 && preferredMode <= 3 && (found->second & (1u << preferredMode)))
    return preferredMode;
  for (int mode = 0; mode <= 3; ++mode)
    if (found->second & (1u << mode)) return mode;
  return {};
}
std::optional<int> parseRate(std::string_view text) {
  int whole = 0, fraction = 0, decimals = -1;
  bool digit = false;
  for (char c : text) {
    if (c == '.' && decimals == -1 && digit) { decimals = 0; continue; }
    if (c < '0' || c > '9') return {};
    digit = true;
    if (decimals < 0) {
      whole = whole * 10 + c - '0';
      if (whole > 100) return {};
    } else {
      if (++decimals > 2) return {};
      fraction = fraction * 10 + c - '0';
    }
  }
  if (!digit || decimals == 0) return {};
  if (decimals == 1) fraction *= 10;
  const int value = whole * 100 + fraction;
  return value > 0 && value <= 10000 ? std::optional<int>(value) : std::nullopt;
}
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
