#include "../i18n/Localization.h"
#include "SettingsSceneShared.h"
#include "../view/ScrollView.h"
#include "../view/UiTheme.h"

using namespace settings_scene;

namespace {
enum class SettingsButtonTone {
  Neutral,
  Primary,
  Info,
  Success,
  Warning,
  Danger,
  Violet
};

enum class ButtonVisualState { Normal, Hover, Pressed };

View::ThemeColorProvider semanticBackgroundProvider(SettingsButtonTone tone,
                                                    ButtonVisualState state) {
  return [tone, state]() {
    switch (tone) {
    case SettingsButtonTone::Primary:
      switch (state) {
      case ButtonVisualState::Normal:
        return ui_theme::primaryAction();
      case ButtonVisualState::Hover:
        return ui_theme::primaryActionHover();
      case ButtonVisualState::Pressed:
        return ui_theme::primaryActionPressed();
      }
      break;
    case SettingsButtonTone::Info:
      switch (state) {
      case ButtonVisualState::Normal:
        return ui_theme::infoAction();
      case ButtonVisualState::Hover:
        return ui_theme::infoActionHover();
      case ButtonVisualState::Pressed:
        return ui_theme::infoActionPressed();
      }
      break;
    case SettingsButtonTone::Success:
      switch (state) {
      case ButtonVisualState::Normal:
        return ui_theme::successAction();
      case ButtonVisualState::Hover:
        return ui_theme::successActionHover();
      case ButtonVisualState::Pressed:
        return ui_theme::successActionPressed();
      }
      break;
    case SettingsButtonTone::Warning:
      switch (state) {
      case ButtonVisualState::Normal:
        return ui_theme::warningAction();
      case ButtonVisualState::Hover:
        return ui_theme::warningActionHover();
      case ButtonVisualState::Pressed:
        return ui_theme::warningActionPressed();
      }
      break;
    case SettingsButtonTone::Danger:
      switch (state) {
      case ButtonVisualState::Normal:
        return ui_theme::dangerAction();
      case ButtonVisualState::Hover:
        return ui_theme::dangerActionHover();
      case ButtonVisualState::Pressed:
        return ui_theme::dangerActionPressed();
      }
      break;
    case SettingsButtonTone::Violet:
      switch (state) {
      case ButtonVisualState::Normal:
        return ui_theme::violetAction();
      case ButtonVisualState::Hover:
        return ui_theme::violetActionHover();
      case ButtonVisualState::Pressed:
        return ui_theme::violetActionPressed();
      }
      break;
    case SettingsButtonTone::Neutral:
      switch (state) {
      case ButtonVisualState::Normal:
        return ui_theme::control();
      case ButtonVisualState::Hover:
        return ui_theme::controlHover();
      case ButtonVisualState::Pressed:
        return ui_theme::controlPressed();
      }
      break;
    }
    return ui_theme::control();
  };
}

View::ThemeColorProvider semanticBorderProvider(SettingsButtonTone tone,
                                                ButtonVisualState state) {
  return [tone, state]() {
    if (tone == SettingsButtonTone::Neutral) {
      switch (state) {
      case ButtonVisualState::Normal:
        return ui_theme::hairline();
      case ButtonVisualState::Hover:
        return ui_theme::accentBorder();
      case ButtonVisualState::Pressed:
        return ui_theme::accentBorderStrong();
      }
    }

    const View::ThemeColorProvider baseProvider =
        semanticBackgroundProvider(tone, state);
    const uint8_t alpha = state == ButtonVisualState::Normal
                              ? 164
                              : (state == ButtonVisualState::Hover ? 206 : 232);
    return ui_theme::withAlpha(baseProvider(), alpha);
  };
}

void applySemanticButtonStyle(Button *button, TextView *text,
                              SettingsButtonTone tone) {
  if (button == nullptr) {
    return;
  }

  auto normal = semanticBackgroundProvider(tone, ButtonVisualState::Normal);
  button->setCornerRadius(ui_theme::controlRadius());
  button->setThemedBackgroundColors(
      normal, semanticBackgroundProvider(tone, ButtonVisualState::Hover),
      semanticBackgroundProvider(tone, ButtonVisualState::Pressed));
  button->setThemedBorderColors(
      semanticBorderProvider(tone, ButtonVisualState::Normal),
      semanticBorderProvider(tone, ButtonVisualState::Hover),
      semanticBorderProvider(tone, ButtonVisualState::Pressed));
  button->setStyledBorderWidth(1);

  if (text == nullptr) {
    return;
  }
  if (tone == SettingsButtonTone::Neutral) {
    text->setThemedColor(ui_theme::textPrimary);
  } else {
    text->setThemedColor([normal]() { return ui_theme::textOn(normal()); });
  }
}
} // namespace

void SettingsScene::styleVisibilityButton(Button *button, TextView *text, bool visible) {
  applySemanticButtonStyle(button, text,
      visible ? SettingsButtonTone::Success : SettingsButtonTone::Danger);
}

void SettingsScene::refreshSettingsText(bool syncInputs) {
  const int offsetMs = context.settings.audioOffsetMs;
  const int visualOffsetMs = context.settings.visualOffsetMs;
  const int visibleTimeDurationMilliseconds =
      context.settings.visibleTimeDurationMilliseconds;
  const i18n::Text offsetLabel = formatOffsetLabel(offsetMs);
  const i18n::Text visualOffsetLabel = formatOffsetLabel(visualOffsetMs);
  const i18n::Text visibleTimeLabel = formatVisibleTimeLabel(
      visibleTimeDurationMilliseconds,
      context.settings.visibleTimeUseMilliseconds);
  const i18n::Text keysoundLabel =
      context.settings.inputKeysoundEnabled ? i18n::message("settings.controls.keysound.input_trigger.label") : i18n::message("settings.controls.keysound.auto_timed.label");
  const i18n::Text prepMetronomeLabel =
      context.settings.prepMetronomeEnabled ? i18n::message("settings.controls.prep_metronome.prep_on.label") : i18n::message("settings.controls.prep_metronome.prep_off.label");
  const i18n::Text bgaLabel =
      context.settings.bgaEnabled ? i18n::message("settings.controls.bga.enabled.label") : i18n::message("settings.controls.bga.disabled.label");
  const i18n::Text bgaDisplayLabel =
      formatBgaDisplayModeLabel(context.settings.bgaDisplayMode);
  const i18n::Text bgaBrightnessLabel =
      formatBgaBrightnessLabel(context.settings.bgaBrightnessPercent);
  const i18n::Text bgaBlurLabel =
      formatBgaBlurLabel(context.settings.bgaBlurStrength);
  const i18n::Text laneAngleLabel =
      formatLaneAngleLabel(context.settings.presentation().laneAngleDegrees);
  const i18n::Text laneLengthLabel =
      formatLaneLengthLabel(context.settings.presentation().laneLength);
  const i18n::Text laneBeamLengthLabel =
      formatLaneBeamLengthLabel(context.settings.presentation().laneBeamLengthPercent);
  const i18n::Text noteStartPositionLabel =
      formatNoteStartPositionLabel(context.settings.presentation().noteStartPositionPercent);
  const i18n::Text previewPlayAreaWidthLabel =
      formatPlayAreaWidthLabel(context.settings.playAreaWidthForKeyMode(previewKeyMode));
  const i18n::Text judgementIndicatorYLabel = formatJudgementPercentLabel(
      judgementIndicatorYToPercent(context.settings.presentation().judgementIndicatorY));
  const i18n::Text judgementIndicatorWidthLabel =
      std::to_string(judgementIndicatorWidthScaleToPercent(
          context.settings.presentation().judgementIndicatorWidthScale)) +
      "%";
  const i18n::Text judgementIndicatorRangeLabel =
      formatJudgementIndicatorRangeLabel(
          context.settings.presentation().judgementIndicatorRangeMilliseconds);
  const i18n::Text notePriorityLabel =
      formatNotePriorityModeLabel(context.settings.notePriorityMode);
  const i18n::Text invisibleNotesLabel =
      context.settings.showInvisibleNotes ? i18n::message("settings.controls.invisible_notes.shown.label") : i18n::message("settings.controls.invisible_notes.hidden.label");
  const i18n::Text markProcessedNotesLabel =
      context.settings.markProcessedNotes ? i18n::message("settings.controls.mark_processed_notes.enabled.label") : i18n::message("settings.controls.mark_processed_notes.disabled.label");
  const i18n::Text startLaneIndicatorsLabel =
      context.settings.startLaneIndicatorsEnabled ? i18n::message("settings.controls.start_lane_indicators.shown.label") : i18n::message("settings.controls.start_lane_indicators.hidden.label");
  const i18n::Text touchVisualizationLabel =
      context.settings.touchVisualizationEnabled ? i18n::message("settings.controls.touch_visualization.shown.label") : i18n::message("settings.controls.touch_visualization.hidden.label");
  const i18n::Text hispeedAutoAdjustLabel =
      context.settings.hispeedAutoAdjust ? i18n::message("settings.controls.hispeed_auto_adjust.hi_speed_auto_adjust_on.label")
                                         : i18n::message("settings.controls.hispeed_auto_adjust.hi_speed_auto_adjust_off.label");
  const i18n::Text archiveChartPreviewLabel =
      context.settings.archiveChartPreviewEnabled ? i18n::message("settings.controls.archive_chart_preview.enabled.label") : i18n::message("settings.controls.archive_chart_preview.disabled.label");
  const i18n::Text findBmsSkipUnarchivingLabel =
      context.settings.findBmsSkipUnarchivingForNonSolidArchives ? i18n::message("settings.controls.find_bms_skip_unarchiving.on.label")
                                                                 : i18n::message("settings.controls.find_bms_skip_unarchiving.off.label");
  const i18n::Text judgementIndicatorLabel =
      context.settings.presentation().judgementIndicatorEnabled ? i18n::message("settings.controls.judgement_indicator.enabled.label") : i18n::message("settings.controls.judgement_indicator.disabled.label");
  const i18n::Text judgementIndicatorRenderModeLabel =
      formatJudgementIndicatorRenderModeLabel(
          context.settings.presentation().judgementIndicatorRenderMode);
  const i18n::Text judgementCounterPositionLabel =
      formatJudgementCounterPositionLabel(
          context.settings.presentation().judgementCounterPosition);
  const i18n::Text judgementCounterModeLabel =
      context.settings.presentation().judgementCounterEnabled ? i18n::message("settings.controls.judgement_counter_mode.enabled.label") : i18n::message("settings.controls.judgement_counter_mode.disabled.label");
  const i18n::Text judgementCounterSummaryLabel =
      context.settings.presentation().judgementCounterEnabled ? judgementCounterPositionLabel
                                               : i18n::message("settings.controls.judgement_counter_summary.disabled.label");
  const i18n::Text judgementTimingFastSlowLabel =
      formatJudgementTimingDisplayCriteriaLabel(
          context.settings.presentation().judgementTimingFastSlowCriteria);
  const i18n::Text judgementTimingMillisecondsLabel =
      formatJudgementTimingDisplayCriteriaLabel(
          context.settings.presentation().judgementTimingMillisecondsCriteria);
  const i18n::Text gaugeBarPositionLabel =
      formatGaugeBarPositionLabel(context.settings.presentation().gaugeBarPosition);
  const i18n::Text uiThemeLabel =
      formatUiThemeModeLabel(context.settings.uiThemeMode);

  if (syncInputs) {
    syncOffsetInputText();
  }
  if (summaryOffsetValueText != nullptr) {
    summaryOffsetValueText->setLocalizedText(offsetLabel);
  }
  if (syncInputs) {
    syncVisualOffsetInputText();
  }
  if (summaryVisualOffsetValueText != nullptr) {
    summaryVisualOffsetValueText->setLocalizedText(visualOffsetLabel);
  }
  if (syncInputs) {
    syncVisibleTimeInputText();
  }
  if (summaryVisibleTimeValueText != nullptr) {
    summaryVisibleTimeValueText->setLocalizedText(visibleTimeLabel);
  }
  if (summaryKeysoundValueText != nullptr) {
    summaryKeysoundValueText->setLocalizedText(keysoundLabel);
  }
  if (summaryBgaValueText != nullptr) {
    summaryBgaValueText->setLocalizedText(bgaLabel);
  }
  if (summaryBgaDisplayValueText != nullptr) {
    summaryBgaDisplayValueText->setLocalizedText(bgaDisplayLabel);
  }
  if (syncInputs) {
    syncBgaBrightnessInputText();
  }
  if (summaryBgaBrightnessValueText != nullptr) {
    summaryBgaBrightnessValueText->setLocalizedText(bgaBrightnessLabel);
  }
  if (syncInputs) {
    syncBgaBlurInputText();
  }
  if (summaryBgaBlurValueText != nullptr) {
    summaryBgaBlurValueText->setLocalizedText(bgaBlurLabel);
  }
  if (syncInputs) {
    syncLaneAngleInputText();
  }
  if (summaryLaneAngleValueText != nullptr) {
    summaryLaneAngleValueText->setLocalizedText(laneAngleLabel);
  }
  if (syncInputs) {
    syncLaneLengthInputText();
  }
  if (summaryLaneLengthValueText != nullptr) {
    summaryLaneLengthValueText->setLocalizedText(laneLengthLabel);
  }
  if (syncInputs) {
    syncLaneBeamLengthInputText();
  }
  if (summaryLaneBeamLengthValueText != nullptr) {
    summaryLaneBeamLengthValueText->setLocalizedText(laneBeamLengthLabel);
  }
  if (syncInputs) {
    syncNoteStartPositionInputText();
  }
  if (summaryNoteStartPositionValueText != nullptr) {
    summaryNoteStartPositionValueText->setLocalizedText(noteStartPositionLabel);
  }
  if (summaryPreviewPlayAreaWidthValueText != nullptr) {
    summaryPreviewPlayAreaWidthValueText->setLocalizedText(previewPlayAreaWidthLabel);
  }
  if (summaryJudgementIndicatorYValueText != nullptr) {
    summaryJudgementIndicatorYValueText->setLocalizedText(judgementIndicatorYLabel);
  }
  if (summaryJudgementIndicatorWidthValueText != nullptr) {
    summaryJudgementIndicatorWidthValueText->setLocalizedText(
        judgementIndicatorWidthLabel);
  }
  if (summaryJudgementIndicatorRangeValueText != nullptr) {
    summaryJudgementIndicatorRangeValueText->setLocalizedText(
        judgementIndicatorRangeLabel);
  }
  if (summaryJudgementCounterPositionValueText != nullptr) {
    summaryJudgementCounterPositionValueText->setLocalizedText(
        judgementCounterSummaryLabel);
  }
  if (summaryJudgementTimingFastSlowValueText != nullptr) {
    summaryJudgementTimingFastSlowValueText->setLocalizedText(
        judgementTimingFastSlowLabel);
  }
  if (summaryJudgementTimingMillisecondsValueText != nullptr) {
    summaryJudgementTimingMillisecondsValueText->setLocalizedText(
        judgementTimingMillisecondsLabel);
  }
  if (summaryGaugeBarPositionValueText != nullptr) {
    summaryGaugeBarPositionValueText->setLocalizedText(gaugeBarPositionLabel);
  }
  if (summaryNotePriorityValueText != nullptr) {
    summaryNotePriorityValueText->setLocalizedText(notePriorityLabel);
  }
  if (summaryUiThemeValueText != nullptr) {
    summaryUiThemeValueText->setLocalizedText(uiThemeLabel);
  }
  if (syncInputs) {
    syncJudgementIndicatorYInputText();
  }
  if (syncInputs) {
    syncJudgementIndicatorWidthInputText();
  }
  if (syncInputs) {
    syncJudgementIndicatorRangeInputText();
  }
  if (keysoundModeText != nullptr) {
    keysoundModeText->setLocalizedText(keysoundLabel);
  }
  if (prepMetronomeModeText != nullptr) {
    prepMetronomeModeText->setLocalizedText(prepMetronomeLabel);
  }
  if (notePriorityModeText != nullptr) {
    notePriorityModeText->setLocalizedText(notePriorityLabel);
  }
  if (showInvisibleNotesModeText != nullptr) {
    showInvisibleNotesModeText->setLocalizedText(invisibleNotesLabel);
  }
  if (markProcessedNotesModeText != nullptr) {
    markProcessedNotesModeText->setLocalizedText(markProcessedNotesLabel);
  }
  if (ipadGestureReminderModeText != nullptr) {
    ipadGestureReminderModeText->setLocalizedText(i18n::message(
        context.settings.ipadGestureReminderEnabled
            ? "settings.visual.ipad_gesture_reminder.on"
            : "settings.visual.ipad_gesture_reminder.off"));
  }
  if (startLaneIndicatorsModeText != nullptr) {
    startLaneIndicatorsModeText->setLocalizedText(startLaneIndicatorsLabel);
  }
  if (touchVisualizationModeText != nullptr) {
    touchVisualizationModeText->setLocalizedText(touchVisualizationLabel);
  }
  if (hispeedAutoAdjustModeText != nullptr) {
    hispeedAutoAdjustModeText->setLocalizedText(hispeedAutoAdjustLabel);
  }
  if (archiveChartPreviewModeText != nullptr) {
    archiveChartPreviewModeText->setLocalizedText(archiveChartPreviewLabel);
  }
  if (findBmsSkipUnarchivingModeText != nullptr) {
    findBmsSkipUnarchivingModeText->setLocalizedText(findBmsSkipUnarchivingLabel);
  }
  if (judgementIndicatorModeText != nullptr) {
    judgementIndicatorModeText->setLocalizedText(judgementIndicatorLabel);
  }
  if (judgementIndicatorRenderModeText != nullptr) {
    judgementIndicatorRenderModeText->setLocalizedText(
        judgementIndicatorRenderModeLabel);
  }
  if (judgementCounterPositionText != nullptr) {
    judgementCounterPositionText->setLocalizedText(judgementCounterPositionLabel);
  }
  if (judgementCounterModeText != nullptr) {
    judgementCounterModeText->setLocalizedText(judgementCounterModeLabel);
  }
  if (judgementTimingFastSlowCriteriaText != nullptr) {
    judgementTimingFastSlowCriteriaText->setLocalizedText(judgementTimingFastSlowLabel);
  }
  if (judgementTimingMillisecondsCriteriaText != nullptr) {
    judgementTimingMillisecondsCriteriaText->setLocalizedText(
        judgementTimingMillisecondsLabel);
  }
  if (gaugeBarPositionText != nullptr) {
    gaugeBarPositionText->setLocalizedText(gaugeBarPositionLabel);
  }
  if (bgaModeText != nullptr) {
    bgaModeText->setLocalizedText(bgaLabel);
  }
  if (bgaDisplayModeText != nullptr) {
    bgaDisplayModeText->setLocalizedText(bgaDisplayLabel);
  }
  if (uiThemeModeText != nullptr) {
    uiThemeModeText->setLocalizedText(uiThemeLabel);
  }
  if (visibleTimeModeText != nullptr) {
    visibleTimeModeText->setLocalizedText(context.settings.visibleTimeUseMilliseconds
                                     ? i18n::message("settings.controls.milliseconds.label")
                                     : i18n::message("settings.controls.green_number.label"));
  }
  applySemanticButtonStyle(visibleTimeModeButton, visibleTimeModeText,
                           context.settings.visibleTimeUseMilliseconds
                               ? SettingsButtonTone::Success
          : SettingsButtonTone::Info);
  applySemanticButtonStyle(keysoundModeButton, keysoundModeText,
                           context.settings.inputKeysoundEnabled
                               ? SettingsButtonTone::Info
                               : SettingsButtonTone::Warning);
  applySemanticButtonStyle(prepMetronomeModeButton, prepMetronomeModeText,
                           context.settings.prepMetronomeEnabled
                               ? SettingsButtonTone::Success
                               : SettingsButtonTone::Info);
  applySemanticButtonStyle(notePriorityModeButton, notePriorityModeText,
                           context.settings.notePriorityMode ==
                                   AppSettings::NotePriorityMode::Lowest
                               ? SettingsButtonTone::Info
                               : SettingsButtonTone::Success);
  applySemanticButtonStyle(
      showInvisibleNotesModeButton, showInvisibleNotesModeText,
      context.settings.showInvisibleNotes ? SettingsButtonTone::Success
                                          : SettingsButtonTone::Info);
  applySemanticButtonStyle(
      markProcessedNotesModeButton, markProcessedNotesModeText,
      context.settings.markProcessedNotes ? SettingsButtonTone::Success
                                           : SettingsButtonTone::Info);
  applySemanticButtonStyle(
      ipadGestureReminderModeButton, ipadGestureReminderModeText,
      context.settings.ipadGestureReminderEnabled ? SettingsButtonTone::Success
                                                  : SettingsButtonTone::Info);
  applySemanticButtonStyle(
      startLaneIndicatorsModeButton, startLaneIndicatorsModeText,
      context.settings.startLaneIndicatorsEnabled ? SettingsButtonTone::Success
                                                  : SettingsButtonTone::Info);
  applySemanticButtonStyle(
      touchVisualizationModeButton, touchVisualizationModeText,
      context.settings.touchVisualizationEnabled ? SettingsButtonTone::Success
                                                 : SettingsButtonTone::Info);
  applySemanticButtonStyle(
      hispeedAutoAdjustModeButton, hispeedAutoAdjustModeText,
      context.settings.hispeedAutoAdjust ? SettingsButtonTone::Success
                                          : SettingsButtonTone::Info);
  applySemanticButtonStyle(
      archiveChartPreviewModeButton, archiveChartPreviewModeText,
      context.settings.archiveChartPreviewEnabled ? SettingsButtonTone::Success
                                                  : SettingsButtonTone::Danger);
  applySemanticButtonStyle(
      findBmsSkipUnarchivingModeButton, findBmsSkipUnarchivingModeText,
      context.settings.findBmsSkipUnarchivingForNonSolidArchives
          ? SettingsButtonTone::Success
          : SettingsButtonTone::Info);
  applySemanticButtonStyle(
      judgementIndicatorModeButton, judgementIndicatorModeText,
      context.settings.presentation().judgementIndicatorEnabled ? SettingsButtonTone::Success
                                                 : SettingsButtonTone::Danger);
  applySemanticButtonStyle(
      judgementIndicatorRenderModeButton, judgementIndicatorRenderModeText,
      context.settings.presentation().judgementIndicatorRenderMode ==
              AppSettings::JudgementIndicatorRenderMode::Hud2D
          ? SettingsButtonTone::Success
          : SettingsButtonTone::Info);
  applySemanticButtonStyle(judgementCounterModeButton, judgementCounterModeText,
                           context.settings.presentation().judgementCounterEnabled
                               ? SettingsButtonTone::Success
                               : SettingsButtonTone::Danger);
  SettingsButtonTone judgementCounterPositionTone = SettingsButtonTone::Neutral;
  if (context.settings.presentation().judgementCounterEnabled) {
    judgementCounterPositionTone =
        context.settings.presentation().judgementCounterPosition ==
                AppSettings::JudgementCounterPosition::Top
            ? SettingsButtonTone::Info
            : SettingsButtonTone::Success;
  }
  applySemanticButtonStyle(judgementCounterPositionButton,
                           judgementCounterPositionText,
                           judgementCounterPositionTone);
  auto judgementTimingCriteriaTone =
      [](AppSettings::JudgementTimingDisplayCriteria criteria) {
        if (criteria == AppSettings::JudgementTimingDisplayCriteria::Off) {
          return SettingsButtonTone::Danger;
        }
        if (criteria ==
                AppSettings::JudgementTimingDisplayCriteria::GoodOrBelow ||
            criteria ==
                AppSettings::JudgementTimingDisplayCriteria::BadOrBelow) {
          return SettingsButtonTone::Success;
        }
        if (criteria ==
            AppSettings::JudgementTimingDisplayCriteria::PGreatOrBelow) {
          return SettingsButtonTone::Warning;
        }
        return SettingsButtonTone::Info;
      };
  applySemanticButtonStyle(
      judgementTimingFastSlowCriteriaButton,
      judgementTimingFastSlowCriteriaText,
      judgementTimingCriteriaTone(
          context.settings.presentation().judgementTimingFastSlowCriteria));
  applySemanticButtonStyle(
      judgementTimingMillisecondsCriteriaButton,
      judgementTimingMillisecondsCriteriaText,
      judgementTimingCriteriaTone(
          context.settings.presentation().judgementTimingMillisecondsCriteria));
  applySemanticButtonStyle(gaugeBarPositionButton, gaugeBarPositionText,
                           context.settings.presentation().gaugeBarPosition ==
                                   AppSettings::GaugeBarPosition::World
                               ? SettingsButtonTone::Info
                               : SettingsButtonTone::Success);
  applySemanticButtonStyle(bgaModeButton, bgaModeText,
                           context.settings.bgaEnabled
                               ? SettingsButtonTone::Success
                               : SettingsButtonTone::Danger);
  applySemanticButtonStyle(uiThemeModeButton, uiThemeModeText,
                           context.settings.uiThemeMode ==
                                   AppSettings::UiThemeMode::Dark
                               ? SettingsButtonTone::Violet
                               : SettingsButtonTone::Warning);

  auto applyTabStyle = [this](Button *button, TextView *text, SettingsTab tab) {
    if (button == nullptr) {
      return;
    }
    if (activeTab == tab) {
      applySemanticButtonStyle(button, text, SettingsButtonTone::Primary);
    } else {
      applySemanticButtonStyle(button, text, SettingsButtonTone::Neutral);
    }
  };
  applyTabStyle(profileTabButton, profileTabText, SettingsTab::Profile);
  applyTabStyle(timingTabButton, timingTabText, SettingsTab::Timing);
  applyTabStyle(visualTabButton, visualTabText, SettingsTab::Visual);
  applyTabStyle(laneTabButton, laneTabText, SettingsTab::Lane);
  applyTabStyle(inputTabButton, inputTabText, SettingsTab::Input);
  applyTabStyle(miscTabButton, miscTabText, SettingsTab::Misc);
  applyTabStyle(audioTabButton, audioTabText, SettingsTab::Audio);
  applyTabStyle(displayTabButton, displayTabText, SettingsTab::Display);
  applyTabStyle(difficultyTablesTabButton, difficultyTablesTabText,
                SettingsTab::DifficultyTables);
  applyTabStyle(bmsLibraryTabButton, bmsLibraryTabText,
                SettingsTab::BmsLibrary);
  applyTabStyle(gameplaySkinsTabButton, gameplaySkinsTabText,
                SettingsTab::GameplaySkins);
  applyTabStyle(irTabButton, irTabText, SettingsTab::Ir);

  if (rootLayout != nullptr) {
    rootLayout->applyYogaLayout();
  }
  if (scrollView != nullptr) {
    scrollView->refreshContentLayout();
  }
}

void SettingsScene::persistSettings() {
  context.settings.sanitize();
  const ui_theme::ThemeMode previousMode = ui_theme::activeMode();
  const ui_theme::ThemeMode nextMode =
      context.settings.uiThemeMode == AppSettings::UiThemeMode::Light
          ? ui_theme::ThemeMode::Light
          : ui_theme::ThemeMode::Dark;
  ui_theme::setActiveMode(nextMode);
  const bool themeChanged = previousMode != nextMode;
  if (!context.saveSettings()) {
    SDL_Log("Failed to save settings");
  }
  context.jukebox.setVisualsEnabled(context.settings.bgaEnabled);
  context.jukebox.setBgaOffsetMs(context.settings.audioOffsetMs);
  context.jukebox.setBgaDisplayMode(context.settings.bgaDisplayMode);
  if (themeChanged && rootLayout != nullptr) {
    rootLayout->propagateThemeChange();
  }
  refreshSettingsText();
}

void SettingsScene::syncOffsetInputText(bool force) {
  if (offsetInput == nullptr) {
    return;
  }
  if (!force && offsetInput->getSelected()) {
    return;
  }
  offsetInput->setEditingText(
      formatOffsetInputValue(context.settings.audioOffsetMs));
}

void SettingsScene::syncVisualOffsetInputText(bool force) {
  if (visualOffsetInput == nullptr) {
    return;
  }
  if (!force && visualOffsetInput->getSelected()) {
    return;
  }
  visualOffsetInput->setEditingText(
      formatOffsetInputValue(context.settings.visualOffsetMs));
}

void SettingsScene::syncVisibleTimeInputText(bool force) {
  if (visibleTimeInput == nullptr) {
    return;
  }
  if (!force && visibleTimeInput->getSelected()) {
    return;
  }
  visibleTimeInput->setEditingText(
      formatVisibleTimeInputValue(
                                  context.settings.visibleTimeDurationMilliseconds,
                                  context.settings.visibleTimeUseMilliseconds));
}

void SettingsScene::syncBgaBrightnessInputText(bool force) {
  if (bgaBrightnessInput == nullptr) {
    return;
  }
  if (!force && bgaBrightnessInput->getSelected()) {
    return;
  }
  bgaBrightnessInput->setEditingText(
      std::to_string(context.settings.bgaBrightnessPercent));
}

void SettingsScene::syncBgaBlurInputText(bool force) {
  if (bgaBlurInput == nullptr) {
    return;
  }
  if (!force && bgaBlurInput->getSelected()) {
    return;
  }
  bgaBlurInput->setEditingText(
      formatFloatValue(context.settings.bgaBlurStrength));
}

void SettingsScene::syncLaneAngleInputText(bool force) {
  if (laneAngleInput == nullptr) {
    return;
  }
  if (!force && laneAngleInput->getSelected()) {
    return;
  }
  laneAngleInput->setEditingText(
      formatFloatValue(context.settings.presentation().laneAngleDegrees));
}

void SettingsScene::syncLaneLengthInputText(bool force) {
  if (laneLengthInput == nullptr) {
    return;
  }
  if (!force && laneLengthInput->getSelected()) {
    return;
  }
  laneLengthInput->setEditingText(
      formatFloatValue(context.settings.presentation().laneLength));
}

void SettingsScene::syncLaneBeamLengthInputText(bool force) {
  if (laneBeamLengthInput == nullptr) {
    return;
  }
  if (!force && laneBeamLengthInput->getSelected()) {
    return;
  }
  laneBeamLengthInput->setEditingText(
      std::to_string(context.settings.presentation().laneBeamLengthPercent));
}

void SettingsScene::syncNoteStartPositionInputText(bool force) {
  if (noteStartPositionInput == nullptr) {
    return;
  }
  if (!force && noteStartPositionInput->getSelected()) {
    return;
  }
  noteStartPositionInput->setEditingText(
      std::to_string(context.settings.presentation().noteStartPositionPercent));
}

void SettingsScene::syncJudgementIndicatorYInputText(bool force) {
  if (judgementIndicatorYInput == nullptr) {
    return;
  }
  if (!force && judgementIndicatorYInput->getSelected()) {
    return;
  }
  judgementIndicatorYInput->setEditingText(std::to_string(
      judgementIndicatorYToPercent(context.settings.presentation().judgementIndicatorY)));
}

void SettingsScene::syncJudgementIndicatorWidthInputText(bool force) {
  if (judgementIndicatorWidthInput == nullptr) {
    return;
  }
  if (!force && judgementIndicatorWidthInput->getSelected()) {
    return;
  }
  judgementIndicatorWidthInput->setEditingText(
      std::to_string(judgementIndicatorWidthScaleToPercent(
          context.settings.presentation().judgementIndicatorWidthScale)));
}

void SettingsScene::syncJudgementIndicatorRangeInputText(bool force) {
  if (judgementIndicatorRangeInput == nullptr) {
    return;
  }
  if (!force && judgementIndicatorRangeInput->getSelected()) {
    return;
  }
  judgementIndicatorRangeInput->setEditingText(
      std::to_string(context.settings.presentation().judgementIndicatorRangeMilliseconds));
}

void SettingsScene::commitOffsetInput() {
  if (offsetInput == nullptr) {
    return;
  }

  const std::string rawText = offsetInput->getText();
  if (rawText.empty()) {
    syncOffsetInputText(true);
    return;
  }

  try {
    context.settings.audioOffsetMs = clampOffset(std::stoi(rawText));
    persistSettings();
    syncOffsetInputText(true);
  } catch (const std::exception &) {
    syncOffsetInputText(true);
  }
}

void SettingsScene::commitVisualOffsetInput() {
  if (visualOffsetInput == nullptr) {
    return;
  }

  const std::string rawText = visualOffsetInput->getText();
  if (rawText.empty()) {
    syncVisualOffsetInputText(true);
    return;
  }

  try {
    context.settings.visualOffsetMs = clampVisualOffset(std::stoi(rawText));
    persistSettings();
    syncVisualOffsetInputText(true);
  } catch (const std::exception &) {
    syncVisualOffsetInputText(true);
  }
}

void SettingsScene::commitVisibleTimeInput() {
  if (visibleTimeInput == nullptr) {
    return;
  }

  const std::string rawText = visibleTimeInput->getText();
  if (rawText.empty()) {
    syncVisibleTimeInputText(true);
    return;
  }

  try {
    const int parsedValue = std::stoi(rawText);
    if (context.settings.visibleTimeUseMilliseconds) {
      context.settings.visibleTimeDurationMilliseconds = std::clamp(
          parsedValue, AppSettings::kMinVisibleTimeMs,
          AppSettings::kMaxVisibleTimeMs);
    } else {
      context.settings.setVisibleTimeGreenNumber(parsedValue);
    }
    persistSettings();
    syncVisibleTimeInputText(true);
  } catch (const std::exception &) {
    syncVisibleTimeInputText(true);
  }
}

void SettingsScene::commitBgaBrightnessInput() {
  if (bgaBrightnessInput == nullptr) {
    return;
  }

  const std::string rawText = bgaBrightnessInput->getText();
  if (rawText.empty()) {
    syncBgaBrightnessInputText(true);
    return;
  }

  try {
    context.settings.bgaBrightnessPercent =
        clampBgaBrightness(std::stoi(rawText));
    persistSettings();
    syncBgaBrightnessInputText(true);
  } catch (const std::exception &) {
    syncBgaBrightnessInputText(true);
  }
}

void SettingsScene::commitBgaBlurInput() {
  if (bgaBlurInput == nullptr) {
    return;
  }

  const std::string rawText = bgaBlurInput->getText();
  if (rawText.empty()) {
    syncBgaBlurInputText(true);
    return;
  }

  try {
    context.settings.bgaBlurStrength = clampBgaBlur(std::stof(rawText));
    persistSettings();
    syncBgaBlurInputText(true);
  } catch (const std::exception &) {
    syncBgaBlurInputText(true);
  }
}

void SettingsScene::commitLaneAngleInput() {
  if (laneAngleInput == nullptr) {
    return;
  }

  const std::string rawText = laneAngleInput->getText();
  if (rawText.empty()) {
    syncLaneAngleInputText(true);
    return;
  }

  try {
    context.settings.presentation().laneAngleDegrees = clampLaneAngle(context.settings, std::stof(rawText));
    persistSettings();
    syncLaneAngleInputText(true);
  } catch (const std::exception &) {
    syncLaneAngleInputText(true);
  }
}

void SettingsScene::commitLaneLengthInput() {
  if (laneLengthInput == nullptr) {
    return;
  }

  const std::string rawText = laneLengthInput->getText();
  if (rawText.empty()) {
    syncLaneLengthInputText(true);
    return;
  }

  try {
    context.settings.presentation().laneLength = clampLaneLength(context.settings, std::stof(rawText));
    persistSettings();
    syncLaneLengthInputText(true);
  } catch (const std::exception &) {
    syncLaneLengthInputText(true);
  }
}

void SettingsScene::commitLaneBeamLengthInput() {
  if (laneBeamLengthInput == nullptr) {
    return;
  }

  const std::string rawText = laneBeamLengthInput->getText();
  if (rawText.empty()) {
    syncLaneBeamLengthInputText(true);
    return;
  }

  try {
    context.settings.presentation().laneBeamLengthPercent =
        clampLaneBeamLengthPercent(std::stoi(rawText));
    persistSettings();
    syncLaneBeamLengthInputText(true);
  } catch (const std::exception &) {
    syncLaneBeamLengthInputText(true);
  }
}

void SettingsScene::commitNoteStartPositionInput() {
  if (noteStartPositionInput == nullptr) {
    return;
  }

  const std::string rawText = noteStartPositionInput->getText();
  if (rawText.empty()) {
    syncNoteStartPositionInputText(true);
    return;
  }

  try {
    context.settings.presentation().noteStartPositionPercent =
        clampNoteStartPositionPercent(std::stoi(rawText));
    persistSettings();
    syncNoteStartPositionInputText(true);
  } catch (const std::exception &) {
    syncNoteStartPositionInputText(true);
  }
}

void SettingsScene::commitJudgementIndicatorYInput() {
  if (judgementIndicatorYInput == nullptr) {
    return;
  }

  const std::string rawText = judgementIndicatorYInput->getText();
  if (rawText.empty()) {
    syncJudgementIndicatorYInputText(true);
    return;
  }

  try {
    const int percent = std::clamp(std::stoi(rawText), 0, 100);
    context.settings.presentation().judgementIndicatorY =
        judgementIndicatorPercentToY(percent);
    persistSettings();
    syncJudgementIndicatorYInputText(true);
  } catch (const std::exception &) {
    syncJudgementIndicatorYInputText(true);
  }
}

void SettingsScene::commitJudgementIndicatorWidthInput() {
  if (judgementIndicatorWidthInput == nullptr) {
    return;
  }

  const std::string rawText = judgementIndicatorWidthInput->getText();
  if (rawText.empty()) {
    syncJudgementIndicatorWidthInputText(true);
    return;
  }

  try {
    const int minPercent = judgementIndicatorWidthScaleToPercent(
        AppSettings::kMinJudgementIndicatorWidthScale);
    const int maxPercent = judgementIndicatorWidthScaleToPercent(
        AppSettings::kMaxJudgementIndicatorWidthScale);
    const int percent = std::clamp(std::stoi(rawText), minPercent, maxPercent);
    context.settings.presentation().judgementIndicatorWidthScale =
        judgementIndicatorWidthPercentToScale(percent);
    persistSettings();
    syncJudgementIndicatorWidthInputText(true);
  } catch (const std::exception &) {
    syncJudgementIndicatorWidthInputText(true);
  }
}

void SettingsScene::commitJudgementIndicatorRangeInput() {
  if (judgementIndicatorRangeInput == nullptr) {
    return;
  }

  const std::string rawText = judgementIndicatorRangeInput->getText();
  if (rawText.empty()) {
    syncJudgementIndicatorRangeInputText(true);
    return;
  }

  try {
    context.settings.presentation().judgementIndicatorRangeMilliseconds =
        clampJudgementIndicatorRangeMilliseconds(std::stoi(rawText));
    persistSettings();
    syncJudgementIndicatorRangeInputText(true);
  } catch (const std::exception &) {
    syncJudgementIndicatorRangeInputText(true);
  }
}
