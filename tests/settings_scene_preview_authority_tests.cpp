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

void testPreviewWaitsForRenderedEndAnimation() {
  settings_scene::PreviewEndAnimation animation;
  skin::SkinGameplayTiming timing;
  timing.finishMarginMillis = 500;
  timing.fadeoutMillis = 250;
  for (const auto elapsed : {27'500'000LL, 31'500'000LL, 36'500'000LL})
    animation.observeRenderedFrame(elapsed, 31'500'000, timing);
  expect(!animation.musicEndMicros && !animation.complete,
         "the preview does not finish during notes or at the exact play deadline");
  animation.observeRenderedFrame(37'000'000, 31'500'000, timing);
  animation.observeRenderedFrame(37'500'000, 31'500'000, timing);
  expect(!animation.fadeoutMicros && !animation.complete,
         "a delayed finish frame still receives its complete authored finish margin");
  animation.observeRenderedFrame(37'501'000, 31'500'000, timing);
  animation.observeRenderedFrame(37'751'000, 31'500'000, timing);
  expect(animation.fadeoutMicros == 37'501'000 && !animation.complete,
         "the preview retains the exact final fadeout frame");
  animation.observeRenderedFrame(37'752'000, 31'500'000, timing);
  expect(animation.complete, "preview loops only after its fadeout has been shown");
  animation = {};
  timing.finishMarginMillis = 0;
  timing.fadeoutMillis = 0;
  animation.observeRenderedFrame(37'000'000, 31'500'000, timing);
  expect(!animation.complete && !animation.fadeoutMicros,
         "zero-length margins still render the finish frame");
  animation.observeRenderedFrame(37'001'000, 31'500'000, timing);
  expect(!animation.complete && animation.fadeoutMicros.has_value(),
         "zero-length fadeout is shown before restarting");
  animation.observeRenderedFrame(37'002'000, 31'500'000, timing);
  expect(animation.complete, "zero-length end animations restart on the following frame");
}

} // namespace

int main() {
  testPreviewLaneCoverAuthorityMirrorsConfiguredEnablement();
  testPreviewPublishesActivePlayerConfiguration();
  testPreviewWaitsForRenderedEndAnimation();
  if (failures != 0) {
    std::cerr << failures << " settings scene preview authority test(s) failed\n";
    return 1;
  }
  std::cout << "settings scene preview authority tests passed\n";
  return 0;
}
