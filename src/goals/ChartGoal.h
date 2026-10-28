#pragma once
#include "../repositories/ScoreRepositoryModels.h"

namespace chart_goals {
struct Target {
  int lamp = -1;
  int scoreNumerator = 0;
  int scoreDenominator = 27;
};
struct Goal {
  bms_parser::ChartMeta chart;
  int longNoteMode = 0;
  Target target;
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
inline constexpr int pageSize = 20;
void validate(const Target &);
std::string normalizedHash(std::string_view);
Progress evaluate(const Target &, std::optional<ScoreBestSnapshot> bestScore,
                  std::optional<ScoreBestSnapshot> bestClear);
} // namespace chart_goals
