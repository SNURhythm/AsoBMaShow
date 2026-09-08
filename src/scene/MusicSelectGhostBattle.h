#pragma once

#include "MainMenuProfileSelections.h"
#include "play/GamePlayStartOptions.h"

inline StartOptions musicSelectGhostBattleOptions(
    const std::shared_ptr<ReplayData> &replay,
    const result_persistence::ChartScoreWrite &score,
    const main_menu_profile::Selections &selections, bool autoKeySound,
    audio::PlaybackRate playback, Scene *returnScene) {
  return {
      .startPosition = 0,
      .autoKeySound = autoKeySound,
      .autoPlay = false,
      .gaugeType = selections.gaugeType,
      .gaugeAutoShift = selections.gaugeAutoShift,
      .gaugeAutoShiftLowerBound = selections.gaugeAutoShiftLowerBound,
      .gbattleRecordData = replay,
      .targetScore = score,
      .playOption = replay->playOption,
      .playOptionSeed = replay->playOptionSeed,
      .playOption2 = replay->playOption2,
      .playOption2Seed = replay->playOption2Seed,
      .longNoteMode = normalizeChartLongNoteModeValue(replay->chartMeta.LnMode),
      .assistOption = replay->assistOption,
      .pacemakerTarget = pacemaker::kTargetOff,
      .playback = playback,
      .returnScene = returnScene,
      .replayGhostRenderingEnabled = false,
      .ruleset = selections.ruleset,
  };
}
