#include "scene/SettingsScenePreviewAuthority.h"

#include <iostream>
#include <string_view>

namespace {

int failures = 0;

void expect(bool value, std::string_view message) {
  if (!value) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void testPreviewLaneCoverAuthorityMirrorsConfiguredEnablement() {
  AppSettings enabled;
  enabled.presentation().noteStartPositionPercent = 63;
  enabled.presentation().laneCoverEnabled = true;
  const auto enabledAuthority =
      settings_scene::previewLaneCoverAuthority(enabled);
  expect(enabledAuthority.percent == 63 && enabledAuthority.enabled,
         "lane preview authority keeps an enabled cover at its configured "
         "position");

  AppSettings disabled;
  disabled.presentation().noteStartPositionPercent = 37;
  disabled.presentation().laneCoverEnabled = false;
  const auto disabledAuthority =
      settings_scene::previewLaneCoverAuthority(disabled);
  expect(disabledAuthority.percent == 37 && !disabledAuthority.enabled,
         "lane preview authority keeps a disabled cover disabled");
}

void testPreviewPublishesActivePlayerConfiguration() {
  AppSettings settings;
  settings.audioVideo.audio.masterVolume = 0.25F;
  settings.audioVideo.audio.keysoundVolume = 0.5F;
  settings.audioVideo.audio.bgmVolume = 0.75F;
  settings.bgaEnabled = false;
  settings.customJudge = true;
  settings.showJudgeArea = true;
  settings.notesDisplayTimingMilliseconds = -18;
  settings.notesDisplayTimingAutoAdjust = true;
  settings.autoSaveReplay = {4, 3, 2, 1};
  settings.guideSoundEffects = true;
  settings.extraNoteDepth = 6;
  settings.mineMode = 2;
  settings.scrollMode = 1;
  settings.longNoteModifierMode = 3;
  settings.sevenToNinePattern = 5;
  settings.sevenToNineType = 4;
  settings.constantScroll = true;
  settings.constantFadeInMilliseconds = 250;
  settings.hispeedAutoAdjust = true;
  settings.showPastNotes = true;
  settings.notePriorityMode = AppSettings::NotePriorityMode::Duration;
  PlayfieldPresentationConfig config;
  config.judgementTextY = 0.25F;
  settings_scene::applyPreviewPlayerConfiguration(config, settings);
  expect(config.masterVolume == 0.25F && config.keysoundVolume == 0.5F &&
             config.bgmVolume == 0.75F && !config.bgaEnabled &&
             config.customJudge && config.showJudgeArea &&
             config.notesDisplayTimingMilliseconds == -18 &&
             config.notesDisplayTimingAutoAdjust &&
             config.autoSaveReplay == settings.autoSaveReplay &&
             config.guideSoundEffects && config.extraNoteDepth == 6 &&
             config.mineMode == 2 && config.scrollMode == 1 &&
             config.longNoteModifierMode == 3 && config.sevenToNinePattern == 5 &&
             config.sevenToNineType == 4 && config.constantScroll &&
             config.constantFadeInMilliseconds == 250 &&
             config.hispeedAutoAdjust && config.showPastNotes &&
             config.judgeAlgorithmImageIndex == 1 && config.judgementTextY == 0.25F,
         "preview skins read the active player settings alongside the selected HUD layout");
}

} // namespace

int main() {
  testPreviewLaneCoverAuthorityMirrorsConfiguredEnablement();
  testPreviewPublishesActivePlayerConfiguration();
  if (failures != 0) {
    std::cerr << failures << " settings scene preview authority test(s) failed\n";
    return 1;
  }
  std::cout << "settings scene preview authority tests passed\n";
  return 0;
}
