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

void SettingsScene::refreshSettingsText() {
  const int offsetMs = context.settings.audioOffsetMs;
  const int visualOffsetMs = context.settings.visualOffsetMs;
  const int visibleTimeGreenNumber = context.settings.visibleTimeGreenNumber;
  const std::string offsetLabel = formatOffsetLabel(offsetMs);
  const std::string visualOffsetLabel = formatOffsetLabel(visualOffsetMs);
  const std::string visibleTimeLabel = formatVisibleTimeLabel(
      visibleTimeGreenNumber, context.settings.visibleTimeUseMilliseconds);
  const std::string visibleTimeBpmStrategyLabel =
      formatVisibleTimeBpmStrategyLabel(
          context.settings.visibleTimeBpmStrategy);
  const std::string keysoundLabel =
      context.settings.inputKeysoundEnabled ? "Input Trigger" : "Auto Timed";
  const std::string prepMetronomeLabel =
      context.settings.prepMetronomeEnabled ? "Prep On" : "Prep Off";
  const std::string bgaLabel =
      context.settings.bgaEnabled ? "Enabled" : "Disabled";
  const std::string bgaDisplayLabel =
      formatBgaDisplayModeLabel(context.settings.bgaDisplayMode);
  const std::string bgaBrightnessLabel =
      formatBgaBrightnessLabel(context.settings.bgaBrightnessPercent);
  const std::string bgaBlurLabel =
      formatBgaBlurLabel(context.settings.bgaBlurStrength);
  const std::string laneAngleLabel =
      formatLaneAngleLabel(context.settings.laneAngleDegrees);
  const std::string laneLengthLabel =
      formatLaneLengthLabel(context.settings.laneLength);
  const std::string laneBeamLengthLabel =
      formatLaneBeamLengthLabel(context.settings.laneBeamLengthPercent);
  const std::string noteStartPositionLabel =
      formatNoteStartPositionLabel(context.settings.noteStartPositionPercent);
  const std::string previewPlayAreaWidthLabel =
      formatPlayAreaWidthLabel(context.settings.playAreaWidthForKeyMode(7));
  const std::string judgementTextYLabel = formatJudgementPercentLabel(
      judgementTextYToPercent(context.settings.judgementTextY));
  const std::string judgementIndicatorYLabel = formatJudgementPercentLabel(
      judgementIndicatorYToPercent(context.settings.judgementIndicatorY));
  const std::string judgementIndicatorWidthLabel =
      std::to_string(judgementIndicatorWidthScaleToPercent(
          context.settings.judgementIndicatorWidthScale)) +
      "%";
  const std::string notePriorityLabel =
      formatNotePriorityModeLabel(context.settings.notePriorityMode);
  const std::string invisibleNotesLabel =
      context.settings.showInvisibleNotes ? "Shown" : "Hidden";
  const std::string startLaneIndicatorsLabel =
      context.settings.startLaneIndicatorsEnabled ? "Shown" : "Hidden";
  const std::string touchVisualizationLabel =
      context.settings.touchVisualizationEnabled ? "Shown" : "Hidden";
  const std::string floatingLaneCoverLabel =
      context.settings.floatingLaneCoverEnabled ? "Floating On"
                                                : "Floating Off";
  const std::string archiveChartPreviewLabel =
      context.settings.archiveChartPreviewEnabled ? "Enabled" : "Disabled";
  const std::string judgementIndicatorLabel =
      context.settings.judgementIndicatorEnabled ? "Enabled" : "Disabled";
  const std::string judgementIndicatorRenderModeLabel =
      formatJudgementIndicatorRenderModeLabel(
          context.settings.judgementIndicatorRenderMode);
  const std::string judgementCounterPositionLabel =
      formatJudgementCounterPositionLabel(
          context.settings.judgementCounterPosition);
  const std::string judgementCounterModeLabel =
      context.settings.judgementCounterEnabled ? "Enabled" : "Disabled";
  const std::string judgementCounterSummaryLabel =
      context.settings.judgementCounterEnabled ? judgementCounterPositionLabel
                                               : "Disabled";
  const std::string judgementTimingFastSlowLabel =
      formatJudgementTimingDisplayCriteriaLabel(
          context.settings.judgementTimingFastSlowCriteria);
  const std::string judgementTimingMillisecondsLabel =
      formatJudgementTimingDisplayCriteriaLabel(
          context.settings.judgementTimingMillisecondsCriteria);
  const std::string gaugeBarPositionLabel =
      formatGaugeBarPositionLabel(context.settings.gaugeBarPosition);
  const std::string uiThemeLabel =
      formatUiThemeModeLabel(context.settings.uiThemeMode);

  syncOffsetInputText();
  if (summaryOffsetValueText != nullptr) {
    summaryOffsetValueText->setText(offsetLabel);
  }
  syncVisualOffsetInputText();
  if (summaryVisualOffsetValueText != nullptr) {
    summaryVisualOffsetValueText->setText(visualOffsetLabel);
  }
  syncVisibleTimeInputText();
  if (summaryVisibleTimeValueText != nullptr) {
    summaryVisibleTimeValueText->setText(visibleTimeLabel);
  }
  if (summaryKeysoundValueText != nullptr) {
    summaryKeysoundValueText->setText(keysoundLabel);
  }
  if (summaryBgaValueText != nullptr) {
    summaryBgaValueText->setText(bgaLabel);
  }
  if (summaryBgaDisplayValueText != nullptr) {
    summaryBgaDisplayValueText->setText(bgaDisplayLabel);
  }
  syncBgaBrightnessInputText();
  if (summaryBgaBrightnessValueText != nullptr) {
    summaryBgaBrightnessValueText->setText(bgaBrightnessLabel);
  }
  syncBgaBlurInputText();
  if (summaryBgaBlurValueText != nullptr) {
    summaryBgaBlurValueText->setText(bgaBlurLabel);
  }
  syncLaneAngleInputText();
  if (summaryLaneAngleValueText != nullptr) {
    summaryLaneAngleValueText->setText(laneAngleLabel);
  }
  syncLaneLengthInputText();
  if (summaryLaneLengthValueText != nullptr) {
    summaryLaneLengthValueText->setText(laneLengthLabel);
  }
  syncLaneBeamLengthInputText();
  if (summaryLaneBeamLengthValueText != nullptr) {
    summaryLaneBeamLengthValueText->setText(laneBeamLengthLabel);
  }
  syncNoteStartPositionInputText();
  if (summaryNoteStartPositionValueText != nullptr) {
    summaryNoteStartPositionValueText->setText(noteStartPositionLabel);
  }
  if (summaryPreviewPlayAreaWidthValueText != nullptr) {
    summaryPreviewPlayAreaWidthValueText->setText(previewPlayAreaWidthLabel);
  }
  if (summaryJudgementTextYValueText != nullptr) {
    summaryJudgementTextYValueText->setText(judgementTextYLabel);
  }
  if (summaryJudgementIndicatorYValueText != nullptr) {
    summaryJudgementIndicatorYValueText->setText(judgementIndicatorYLabel);
  }
  if (summaryJudgementIndicatorWidthValueText != nullptr) {
    summaryJudgementIndicatorWidthValueText->setText(
        judgementIndicatorWidthLabel);
  }
  if (summaryJudgementCounterPositionValueText != nullptr) {
    summaryJudgementCounterPositionValueText->setText(
        judgementCounterSummaryLabel);
  }
  if (summaryJudgementTimingFastSlowValueText != nullptr) {
    summaryJudgementTimingFastSlowValueText->setText(
        judgementTimingFastSlowLabel);
  }
  if (summaryJudgementTimingMillisecondsValueText != nullptr) {
    summaryJudgementTimingMillisecondsValueText->setText(
        judgementTimingMillisecondsLabel);
  }
  if (summaryGaugeBarPositionValueText != nullptr) {
    summaryGaugeBarPositionValueText->setText(gaugeBarPositionLabel);
  }
  if (summaryNotePriorityValueText != nullptr) {
    summaryNotePriorityValueText->setText(notePriorityLabel);
  }
  if (summaryUiThemeValueText != nullptr) {
    summaryUiThemeValueText->setText(uiThemeLabel);
  }
  syncJudgementIndicatorYInputText();
  syncJudgementIndicatorWidthInputText();
  if (keysoundModeText != nullptr) {
    keysoundModeText->setText(keysoundLabel);
  }
  if (prepMetronomeModeText != nullptr) {
    prepMetronomeModeText->setText(prepMetronomeLabel);
  }
  if (notePriorityModeText != nullptr) {
    notePriorityModeText->setText(notePriorityLabel);
  }
  if (showInvisibleNotesModeText != nullptr) {
    showInvisibleNotesModeText->setText(invisibleNotesLabel);
  }
  if (startLaneIndicatorsModeText != nullptr) {
    startLaneIndicatorsModeText->setText(startLaneIndicatorsLabel);
  }
  if (touchVisualizationModeText != nullptr) {
    touchVisualizationModeText->setText(touchVisualizationLabel);
  }
  if (floatingLaneCoverModeText != nullptr) {
    floatingLaneCoverModeText->setText(floatingLaneCoverLabel);
  }
  if (archiveChartPreviewModeText != nullptr) {
    archiveChartPreviewModeText->setText(archiveChartPreviewLabel);
  }
  if (judgementIndicatorModeText != nullptr) {
    judgementIndicatorModeText->setText(judgementIndicatorLabel);
  }
  if (judgementIndicatorRenderModeText != nullptr) {
    judgementIndicatorRenderModeText->setText(
        judgementIndicatorRenderModeLabel);
  }
  if (judgementCounterPositionText != nullptr) {
    judgementCounterPositionText->setText(judgementCounterPositionLabel);
  }
  if (judgementCounterModeText != nullptr) {
    judgementCounterModeText->setText(judgementCounterModeLabel);
  }
  if (judgementTimingFastSlowCriteriaText != nullptr) {
    judgementTimingFastSlowCriteriaText->setText(judgementTimingFastSlowLabel);
  }
  if (judgementTimingMillisecondsCriteriaText != nullptr) {
    judgementTimingMillisecondsCriteriaText->setText(
        judgementTimingMillisecondsLabel);
  }
  if (gaugeBarPositionText != nullptr) {
    gaugeBarPositionText->setText(gaugeBarPositionLabel);
  }
  if (bgaModeText != nullptr) {
    bgaModeText->setText(bgaLabel);
  }
  if (bgaDisplayModeText != nullptr) {
    bgaDisplayModeText->setText(bgaDisplayLabel);
  }
  if (uiThemeModeText != nullptr) {
    uiThemeModeText->setText(uiThemeLabel);
  }
  if (visibleTimeModeText != nullptr) {
    visibleTimeModeText->setText(context.settings.visibleTimeUseMilliseconds
                                     ? "Milliseconds"
                                     : "Green Number");
  }
  if (visibleTimeBpmStrategyText != nullptr) {
    visibleTimeBpmStrategyText->setText("BPM: " + visibleTimeBpmStrategyLabel);
  }

  applySemanticButtonStyle(visibleTimeModeButton, visibleTimeModeText,
                           context.settings.visibleTimeUseMilliseconds
                               ? SettingsButtonTone::Success
                               : SettingsButtonTone::Info);
  applySemanticButtonStyle(
      visibleTimeBpmStrategyButton, visibleTimeBpmStrategyText,
      context.settings.visibleTimeBpmStrategy ==
              AppSettings::VisibleTimeBpmStrategy::MostPrevalent
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
      startLaneIndicatorsModeButton, startLaneIndicatorsModeText,
      context.settings.startLaneIndicatorsEnabled ? SettingsButtonTone::Success
                                                  : SettingsButtonTone::Info);
  applySemanticButtonStyle(
      touchVisualizationModeButton, touchVisualizationModeText,
      context.settings.touchVisualizationEnabled ? SettingsButtonTone::Success
                                                 : SettingsButtonTone::Info);
  applySemanticButtonStyle(
      floatingLaneCoverModeButton, floatingLaneCoverModeText,
      context.settings.floatingLaneCoverEnabled ? SettingsButtonTone::Success
                                                : SettingsButtonTone::Info);
  applySemanticButtonStyle(
      archiveChartPreviewModeButton, archiveChartPreviewModeText,
      context.settings.archiveChartPreviewEnabled ? SettingsButtonTone::Success
                                                  : SettingsButtonTone::Danger);
  applySemanticButtonStyle(
      judgementIndicatorModeButton, judgementIndicatorModeText,
      context.settings.judgementIndicatorEnabled ? SettingsButtonTone::Success
                                                 : SettingsButtonTone::Danger);
  applySemanticButtonStyle(
      judgementIndicatorRenderModeButton, judgementIndicatorRenderModeText,
      context.settings.judgementIndicatorRenderMode ==
              AppSettings::JudgementIndicatorRenderMode::Hud2D
          ? SettingsButtonTone::Success
          : SettingsButtonTone::Info);
  applySemanticButtonStyle(judgementCounterModeButton, judgementCounterModeText,
                           context.settings.judgementCounterEnabled
                               ? SettingsButtonTone::Success
                               : SettingsButtonTone::Danger);
  SettingsButtonTone judgementCounterPositionTone = SettingsButtonTone::Neutral;
  if (context.settings.judgementCounterEnabled) {
    judgementCounterPositionTone =
        context.settings.judgementCounterPosition ==
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
          context.settings.judgementTimingFastSlowCriteria));
  applySemanticButtonStyle(
      judgementTimingMillisecondsCriteriaButton,
      judgementTimingMillisecondsCriteriaText,
      judgementTimingCriteriaTone(
          context.settings.judgementTimingMillisecondsCriteria));
  applySemanticButtonStyle(gaugeBarPositionButton, gaugeBarPositionText,
                           context.settings.gaugeBarPosition ==
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
      formatVisibleTimeInputValue(context.settings.visibleTimeGreenNumber,
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
      formatFloatValue(context.settings.laneAngleDegrees));
}

void SettingsScene::syncLaneLengthInputText(bool force) {
  if (laneLengthInput == nullptr) {
    return;
  }
  if (!force && laneLengthInput->getSelected()) {
    return;
  }
  laneLengthInput->setEditingText(
      formatFloatValue(context.settings.laneLength));
}

void SettingsScene::syncLaneBeamLengthInputText(bool force) {
  if (laneBeamLengthInput == nullptr) {
    return;
  }
  if (!force && laneBeamLengthInput->getSelected()) {
    return;
  }
  laneBeamLengthInput->setEditingText(
      std::to_string(context.settings.laneBeamLengthPercent));
}

void SettingsScene::syncNoteStartPositionInputText(bool force) {
  if (noteStartPositionInput == nullptr) {
    return;
  }
  if (!force && noteStartPositionInput->getSelected()) {
    return;
  }
  noteStartPositionInput->setEditingText(
      std::to_string(context.settings.noteStartPositionPercent));
}

void SettingsScene::syncJudgementIndicatorYInputText(bool force) {
  if (judgementIndicatorYInput == nullptr) {
    return;
  }
  if (!force && judgementIndicatorYInput->getSelected()) {
    return;
  }
  judgementIndicatorYInput->setEditingText(std::to_string(
      judgementIndicatorYToPercent(context.settings.judgementIndicatorY)));
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
          context.settings.judgementIndicatorWidthScale)));
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
      const int milliseconds =
          std::clamp(parsedValue, AppSettings::kMinVisibleTimeMs,
                     AppSettings::kMaxVisibleTimeMs);
      context.settings.visibleTimeGreenNumber =
          clampVisibleTimeGreenNumber(millisecondsToGreenNumber(milliseconds));
    } else {
      context.settings.visibleTimeGreenNumber =
          clampVisibleTimeGreenNumber(parsedValue);
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
    context.settings.laneAngleDegrees = clampLaneAngle(std::stof(rawText));
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
    context.settings.laneLength = clampLaneLength(std::stof(rawText));
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
    context.settings.laneBeamLengthPercent =
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
    context.settings.noteStartPositionPercent =
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
    context.settings.judgementIndicatorY =
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
    context.settings.judgementIndicatorWidthScale =
        judgementIndicatorWidthPercentToScale(percent);
    persistSettings();
    syncJudgementIndicatorWidthInputText(true);
  } catch (const std::exception &) {
    syncJudgementIndicatorWidthInputText(true);
  }
}
