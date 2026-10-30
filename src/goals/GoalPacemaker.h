#pragma once
#include "GoalChallenge.h"
#include "GoalTargetPresentation.h"
#include "../repositories/ScoreRepository.h"
#include <tuple>

namespace chart_goals {
inline std::optional<Target> loadPacemakerTarget(ScoreRepository &scores,
    const bms_parser::ChartMeta &meta, int selectedMode, std::int64_t preferredId,
    std::string &error) {
  if (meta.SHA256.empty()) return {};
  const auto goals = scores.LoadChartGoalsForChart(meta, error);
  if (!goals) return {};
  return scoreTargetForGoals(*goals, scoreLongNoteModeForClearLamp(meta, selectedMode), preferredId);
}

class PacemakerCache {
public:
  std::string label(ScoreRepository &scores, const bms_parser::ChartMeta &meta,
                    int selectedMode, std::int64_t preferredId, const std::string &profile) {
    const auto key = std::tuple{profile, meta.SHA256,
        scoreLongNoteModeForClearLamp(meta, selectedMode), preferredId, scores.GetRevision()};
    if (!key_ || *key_ != key) {
      std::string error;
      const auto target = loadPacemakerTarget(scores, meta, selectedMode, preferredId, error);
      label_ = target ? targetLabels(*target).score : "BEST";
      key_ = key;
    }
    return label_;
  }
private:
  std::optional<std::tuple<std::string, std::string, int, std::int64_t, std::uint64_t>> key_;
  std::string label_ = "BEST";
};
} // namespace chart_goals
