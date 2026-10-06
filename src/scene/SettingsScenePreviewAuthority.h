#pragma once

#include "../AppSettings.h"
#include "SettingsPreviewChart.h"
#include "SettingsPreviewPlayback.h"
#include "play/PlayfieldVisualState.h"

namespace settings_scene {

[[nodiscard]] inline GameplayLaneCoverAuthority
previewLaneCoverAuthority(const AppSettings &settings) noexcept {
  return gameplayLaneCoverAuthority(settings.presentation().noteStartPositionPercent,
                                    settings.presentation().laneCoverEnabled);
}

inline void applyPreviewPlayerConfiguration(PlayfieldPresentationConfig &config,
                                            const AppSettings &settings) {
  config.showPastNotes = settings.showPastNotes;
  config.liftEnabled = settings.presentation().liftEnabled;
  config.liftRatio = settings.presentation().liftRatio;
  config.hiddenEnabled = settings.presentation().hiddenEnabled;
  config.hiddenRatio = settings.presentation().hiddenRatio;
  config.masterVolume = settings.audioVideo.audio.masterVolume;
  config.keysoundVolume = settings.audioVideo.audio.keysoundVolume;
  config.bgmVolume = settings.audioVideo.audio.bgmVolume;
  config.bgaEnabled = settings.bgaEnabled;
  config.hispeedAutoAdjust = settings.hispeedAutoAdjust;
  config.customJudge = settings.customJudge;
  config.showJudgeArea = settings.showJudgeArea;
  config.notesDisplayTimingMilliseconds = settings.notesDisplayTimingMilliseconds;
  config.notesDisplayTimingAutoAdjust = settings.notesDisplayTimingAutoAdjust;
  config.autoSaveReplay = settings.autoSaveReplay;
  config.guideSoundEffects = settings.guideSoundEffects;
  config.extraNoteDepth = settings.extraNoteDepth;
  config.mineMode = settings.mineMode;
  config.scrollMode = settings.scrollMode;
  config.longNoteModifierMode = settings.longNoteModifierMode;
  config.sevenToNinePattern = settings.sevenToNinePattern;
  config.sevenToNineType = settings.sevenToNineType;
  config.constantScroll = settings.constantScroll;
  config.constantFadeInMilliseconds = settings.constantFadeInMilliseconds;
  config.judgeAlgorithmImageIndex =
      beatorajaJudgeAlgorithmImageIndex(settings.notePriorityMode);
}

[[nodiscard]] inline PlayfieldFrameClock
previewFrameClock(std::uint64_t serial, long long elapsedMicros,
                  long long lastNoteMicros) noexcept {
  return {.serial = serial,
          .visualTimeMicros = elapsedMicros,
          .gameplayTimeMicros = elapsedMicros,
          .replayTouchTimeMicros = elapsedMicros,
          .bgaTimeMicros = elapsedMicros,
          // Beatoraja playtime includes five seconds after the final note.
          .playTimer = {.active = true,
                        .startMicros = 0,
                        .elapsedMillisExact = true,
                        .playtimeMillis = previewPlaytimeMillis(lastNoteMicros)}};
}

} // namespace settings_scene
