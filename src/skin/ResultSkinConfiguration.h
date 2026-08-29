#pragma once

#include "../AssistOptionUtils.h"
#include "SkinTypes.h"

#include <limits>
#include <string>
#include <string_view>

// AbstractResult retains MainState.resource, so the result property factories
// continue to read PlayerConfig and PlayConfig. Build that shared source-facing
// snapshot once for interactive results and every export surface.
inline ResultSkinConfigurationData
makeResultSkinConfiguration(const AppSettings &settings) {
  const int judgeAlgorithmImageIndex =
      settings.notePriorityMode == AppSettings::NotePriorityMode::Combo
          ? 0
      : settings.notePriorityMode == AppSettings::NotePriorityMode::Duration
          ? 1
      : settings.notePriorityMode == AppSettings::NotePriorityMode::Lowest
          ? 2
          : std::numeric_limits<int>::min();
  const int gaugeAutoShiftImageIndex =
      settings.selectedGaugeAutoShiftMode == "continue"             ? 1
      : settings.selectedGaugeAutoShiftMode == "survival_to_groove" ? 2
      : settings.selectedGaugeAutoShiftMode == "best_clear"         ? 3
      : settings.selectedGaugeAutoShiftMode == "select_to_under"    ? 4
                                                                       : 0;
  const int bottomShiftableGaugeImageIndex =
      settings.selectedGaugeAutoShiftLowerBound == "easy"   ? 1
      : settings.selectedGaugeAutoShiftLowerBound == "normal" ? 2
                                                               : 0;
  return {
      .gameplayHispeed = settings.gameplayHispeed,
      .notesDisplayTimingMilliseconds =
          settings.notesDisplayTimingMilliseconds,
      .visibleTimeDurationMilliseconds =
          settings.visibleTimeDurationMilliseconds,
      .hispeedFixMode = static_cast<int>(settings.hispeedFixMode),
      .bgaEnabled = settings.bgaEnabled,
      .bpmGuideEnabled =
          settings.selectedAssistOption == assist_options::kBpmGuide,
      .customJudge = settings.customJudge,
      .showJudgeArea = settings.showJudgeArea,
      .markProcessedNotes = settings.markProcessedNotes,
      .notesDisplayTimingAutoAdjust = settings.notesDisplayTimingAutoAdjust,
      .autoSaveReplay = settings.autoSaveReplay,
      .guideSoundEffects = settings.guideSoundEffects,
      .extraNoteDepth = settings.extraNoteDepth,
      .mineMode = settings.mineMode,
      .scrollMode = settings.scrollMode,
      .longNoteModifierMode = settings.longNoteModifierMode,
      .sevenToNinePattern = settings.sevenToNinePattern,
      .sevenToNineType = settings.sevenToNineType,
      .laneCoverEnabled = settings.laneCoverEnabled,
      .liftEnabled = settings.liftEnabled,
      .hiddenEnabled = settings.hiddenEnabled,
      .hispeedAutoAdjust = settings.hispeedAutoAdjust,
      .judgeAlgorithmImageIndex = judgeAlgorithmImageIndex,
      .gaugeAutoShiftImageIndex = gaugeAutoShiftImageIndex,
      .bottomShiftableGaugeImageIndex = bottomShiftableGaugeImageIndex,
      .modeFilterName = settings.skinModeFilterName,
      .sortId = settings.skinSortId,
      .difficultyFilterName = settings.skinDifficultyFilterName,
      .chartReplicationMode = settings.skinChartReplicationMode,
      .irName = settings.irProviders.empty()
                    ? std::string{}
                    : settings.irProviders.begin()->first,
      .skinTargetId = settings.skinTargetId,
      .skinTargetList = settings.skinTargetList,
  };
}
