#pragma once
#include "../repositories/ScoreRepositoryModels.h"
#include <map>

namespace chart_goals {
struct Target {
  int lamp = -1;
  int scoreNumerator = 0;
  int scoreDenominator = 27;
  bool operator==(const Target &) const = default;
};
struct Goal {
  bms_parser::ChartMeta chart;
  int longNoteMode = 0;
  Target target;
  std::int64_t setAtUnixMillis = 0;
  std::int64_t achievedAtUnixMillis = 0;
};
struct Progress {
  std::optional<ScoreBestSnapshot> bestScore;
  std::optional<ScoreBestSnapshot> bestClear;
  bool lampMet = false;
  bool scoreMet = false;
  bool achieved = false;
  std::optional<int> remainingEx;
};
struct GoalProgress { Goal goal; Progress progress; };
struct ProgressPage { std::vector<GoalProgress> goals; bool hasMore = false; };
// Identity-only selector cache: one mode mask per chart, without score projection.
struct Index {
  std::map<std::string, unsigned> modes;
  std::vector<std::string> hashes() const;
  std::optional<int> modeFor(std::string_view sha256, int preferredMode) const;
};
inline constexpr int pageSize = 20;
std::int64_t nowUnixMillis();
std::string localDateTime(std::int64_t unixMillis);
// Percentage input to exact hundredths: 99.99 -> 9999/10000.
std::optional<int> parseRate(std::string_view);
void validate(const Target &);
std::string normalizedHash(std::string_view);
Progress evaluate(const Target &, std::optional<ScoreBestSnapshot> bestScore,
                  std::optional<ScoreBestSnapshot> bestClear);
} // namespace chart_goals
