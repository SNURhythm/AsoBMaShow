#pragma once
#include "MainMenuProfileSelections.h"
#include "../goals/GoalChallenge.h"

namespace chart_goals {
inline void applyChallengeSettings(AppSettings &settings, const Goal &goal) {
  auto selections = main_menu_profile::Selections::fromSettings(settings);
  selections.gaugeType = challengeGauge(selections.gaugeType, selections.gaugeAutoShift, goal.target.lamp);
  selections.pacemakerTarget = pacemaker::kTargetGoal;
  if (goal.longNoteMode > 0) selections.longNoteMode = long_note_mode::idFromValue(goal.longNoteMode);
  selections.applyTo(settings);
  settings.selectedPacemakerGoalId = std::max<std::int64_t>(0, goal.id);
}
} // namespace chart_goals
