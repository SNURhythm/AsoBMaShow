#pragma once

#include "../../AppSettings.h"
#include "GameplayJudgeRules.h"

[[nodiscard]] inline gameplay::CandidateSelectionMode
candidateSelectionForNotePriority(AppSettings::NotePriorityMode mode) {
  switch (mode) {
  case AppSettings::NotePriorityMode::Combo:
    return gameplay::CandidateSelectionMode::Combo;
  case AppSettings::NotePriorityMode::Duration:
    return gameplay::CandidateSelectionMode::Duration;
  case AppSettings::NotePriorityMode::Score:
    return gameplay::CandidateSelectionMode::Score;
  case AppSettings::NotePriorityMode::Lowest:
  default:
    return gameplay::CandidateSelectionMode::Lowest;
  }
}

[[nodiscard]] inline AppSettings::NotePriorityMode
notePriorityForCandidateSelection(gameplay::CandidateSelectionMode mode) {
  switch (mode) {
  case gameplay::CandidateSelectionMode::Combo:
    return AppSettings::NotePriorityMode::Combo;
  case gameplay::CandidateSelectionMode::Duration:
    return AppSettings::NotePriorityMode::Duration;
  case gameplay::CandidateSelectionMode::Score:
    return AppSettings::NotePriorityMode::Score;
  case gameplay::CandidateSelectionMode::LR2:
  case gameplay::CandidateSelectionMode::Lowest:
    return AppSettings::NotePriorityMode::Lowest;
  }
  return AppSettings::NotePriorityMode::Lowest;
}
