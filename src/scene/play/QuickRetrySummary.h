#pragma once

#include "QuickRetry.h"
#include "../../goals/ChartGoal.h"
#include <optional>
#include <string>
#include <vector>

namespace quick_retry {
struct GoalTargets {
  Targets targets;
  std::optional<chart_goals::Target> scoreLabel;
};

inline GoalTargets compileGoalTargets(std::span<const chart_goals::GoalProgress> goals,
                                     int longNoteMode, int totalNotes) {
  GoalTargets result;
  for (const auto &entry : goals) {
    if (entry.goal.longNoteMode != longNoteMode || entry.progress.achieved) continue;
    const auto &target = entry.goal.target;
    if (!entry.progress.scoreMet && target.scoreNumerator > 0 && target.scoreDenominator > 0) {
      const int score = static_cast<int>((2LL * totalNotes * target.scoreNumerator +
                                         target.scoreDenominator - 1) / target.scoreDenominator);
      if (result.targets.score < 0 || score < result.targets.score) result.scoreLabel = target;
      result.targets.add({.score = score});
    }
    if (!entry.progress.lampMet && target.lamp >= 0) result.targets.add({.lamp = target.lamp});
  }
  return result;
}

// A clause describes hurdles that must all remain reachable. Multiple clauses
// are alternative ways to keep the attempt. Gauge is separate from Clear when
// auto-shift or assists prevent equating physical gauge survival with a lamp.
enum class HurdleKind { Score, Clear, Gauge };
struct Hurdle {
  HurdleKind kind;
  int value;
  std::string label;
};
using HurdleClause = std::vector<Hurdle>;
using HurdleAlternatives = std::vector<HurdleClause>;

inline HurdleClause combineHurdles(HurdleClause left, const HurdleClause &right) {
  for (const auto &hurdle : right) {
    const auto same = std::find_if(left.begin(), left.end(), [&](const auto &other) {
      return other.kind == hurdle.kind;
    });
    if (same == left.end()) left.push_back(hurdle);
    else if (hurdle.value > same->value) *same = hurdle;
  }
  return left;
}

inline bool implies(const HurdleClause &strict, const HurdleClause &easy) {
  return std::all_of(easy.begin(), easy.end(), [&](const auto &hurdle) {
    return std::any_of(strict.begin(), strict.end(), [&](const auto &other) {
      return other.kind == hurdle.kind && other.value >= hurdle.value;
    });
  });
}

inline HurdleAlternatives summarizeHurdles(const std::vector<HurdleAlternatives> &groups,
                                         bool allConditionsFail) {
  HurdleAlternatives result;
  for (const auto &group : groups) {
    if (group.empty()) continue;
    if (allConditionsFail || result.empty()) result.insert(result.end(), group.begin(), group.end());
    else {
      HurdleAlternatives combined;
      for (const auto &left : result)
        for (const auto &right : group) combined.push_back(combineHurdles(left, right));
      result = std::move(combined);
    }
  }
  // In an OR, an easier alternative subsumes a stricter one. This also removes
  // goal alternatives already satisfied by an independent score requirement.
  HurdleAlternatives reduced;
  for (auto &clause : result) {
    if (std::any_of(reduced.begin(), reduced.end(), [&](const auto &other) { return implies(clause, other); }))
      continue;
    std::erase_if(reduced, [&](const auto &other) { return implies(other, clause); });
    reduced.push_back(std::move(clause));
  }
  return reduced;
}

inline std::string hurdleText(const HurdleAlternatives &alternatives, const std::string &orWord) {
  std::string text;
  for (const auto &clause : alternatives) {
    if (!text.empty()) text += " " + orWord + " ";
    const bool grouped = alternatives.size() > 1 && clause.size() > 1;
    if (grouped) text += '(';
    for (std::size_t i = 0; i < clause.size(); ++i) {
      if (i) text += " + ";
      text += clause[i].label;
    }
    if (grouped) text += ')';
  }
  return text;
}
} // namespace quick_retry
