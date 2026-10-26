#pragma once
#include "StreakModel.h"
#include <algorithm>
#include <array>
#include <optional>

namespace streak {
inline constexpr std::array comboMilestones{7, 14, 30, 50, 100, 365};
enum class ActivityKind { NotStarted, NoPlay, Played, GoalReached, Frozen };
struct ActivityDay { DayId day; ActivityKind kind; };

inline std::array<ActivityDay, 7> recentActivity(const State &state, DayId today) {
  std::array<ActivityDay, 7> result;
  for (int i = 0; i < 7; ++i) {
    const auto day = today - 6 + i;
    auto kind = ActivityKind::NotStarted;
    if (!state.configurations.empty() && day >= state.configurations.begin()->first) {
      kind = ActivityKind::NoPlay;
      if (const auto found = state.days.find(day); found != state.days.end()) {
        const auto &saved = found->second;
        if (saved.points >= saved.config.dailyGoal) kind = ActivityKind::GoalReached;
        else if (saved.frozen) kind = ActivityKind::Frozen;
        else if (saved.played) kind = ActivityKind::Played;
      }
    }
    result[i] = {day, kind};
  }
  return result;
}
struct MilestoneProgress { int target, remaining; };
inline std::optional<MilestoneProgress> nextMilestone(const State &state) {
  for (const int target : comboMilestones)
    if (target > state.currentStreak && !state.milestones.contains("streak:" + std::to_string(target)))
      return MilestoneProgress{target, target - state.currentStreak};
  return std::nullopt;
}
} // namespace streak
