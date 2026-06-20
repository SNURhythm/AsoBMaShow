#include "SettingsSceneShared.h"
#include "../view/ScrollView.h"
#include "../view/UiTheme.h"

using namespace settings_scene;

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
  const std::string notePriorityLabel =
      formatNotePriorityModeLabel(context.settings.notePriorityMode);
  const std::string invisibleNotesLabel =
      context.settings.showInvisibleNotes ? "Shown" : "Hidden";
  const std::string archiveChartPreviewLabel =
      context.settings.archiveChartPreviewEnabled ? "Enabled" : "Disabled";
  const std::string judgementIndicatorLabel =
      context.settings.judgementIndicatorEnabled ? "Enabled" : "Disabled";
  const std::string judgementIndicatorRenderModeLabel =
      formatJudgementIndicatorRenderModeLabel(
          context.settings.judgementIndicatorRenderMode);
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
  if (notePriorityModeText != nullptr) {
    notePriorityModeText->setText(notePriorityLabel);
  }
  if (showInvisibleNotesModeText != nullptr) {
    showInvisibleNotesModeText->setText(invisibleNotesLabel);
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
    visibleTimeBpmStrategyText->setText("BPM: " +
                                        visibleTimeBpmStrategyLabel);
  }

  auto applyTonalStyle = [](Button *button, const Color &accent) {
    if (button == nullptr) {
      return;
    }
    const bool light = ui_theme::activeMode() == ui_theme::ThemeMode::Light;
    const uint8_t normalAlpha = light ? 54 : 82;
    const uint8_t hoverAlpha = light ? 74 : 108;
    const uint8_t pressedAlpha = light ? 100 : 136;
    button->setBackgroundColors(Color(accent.r, accent.g, accent.b, normalAlpha),
                                Color(accent.r, accent.g, accent.b, hoverAlpha),
                                Color(accent.r, accent.g, accent.b,
                                      pressedAlpha));
    button->setBorderColors(Color(accent.r, accent.g, accent.b, 178),
                            Color(accent.r, accent.g, accent.b, 216),
                            accent);
  };

  auto applyNeutralStyle = [](Button *button) {
    if (button == nullptr) {
      return;
    }
    button->setBackgroundColors(ui_theme::control(), ui_theme::controlHover(),
                                ui_theme::controlPressed());
    button->setBorderColors(ui_theme::hairline(), ui_theme::cyan(),
                            ui_theme::cyan());
  };

  applyTonalStyle(visibleTimeModeButton,
                  context.settings.visibleTimeUseMilliseconds
                      ? ui_theme::lime()
                      : ui_theme::cyan());
  applyTonalStyle(visibleTimeBpmStrategyButton,
                  context.settings.visibleTimeBpmStrategy ==
                          AppSettings::VisibleTimeBpmStrategy::MostPrevalent
                      ? ui_theme::lime()
                      : ui_theme::cyan());
  applyTonalStyle(keysoundModeButton,
                  context.settings.inputKeysoundEnabled ? ui_theme::cyan()
                                                        : ui_theme::amber());
  applyTonalStyle(notePriorityModeButton,
                  context.settings.notePriorityMode ==
                          AppSettings::NotePriorityMode::Lowest
                      ? ui_theme::cyan()
                      : ui_theme::lime());
  applyTonalStyle(showInvisibleNotesModeButton,
                  context.settings.showInvisibleNotes ? ui_theme::lime()
                                                      : ui_theme::cyan());
  applyTonalStyle(archiveChartPreviewModeButton,
                  context.settings.archiveChartPreviewEnabled
                      ? ui_theme::lime()
                      : ui_theme::coral());
  applyTonalStyle(judgementIndicatorModeButton,
                  context.settings.judgementIndicatorEnabled
                      ? ui_theme::lime()
                      : ui_theme::coral());
  applyTonalStyle(judgementIndicatorRenderModeButton,
                  context.settings.judgementIndicatorRenderMode ==
                          AppSettings::JudgementIndicatorRenderMode::Hud2D
                      ? ui_theme::lime()
                      : ui_theme::cyan());
  applyTonalStyle(bgaModeButton,
                  context.settings.bgaEnabled ? ui_theme::lime()
                                              : ui_theme::coral());
  applyTonalStyle(uiThemeModeButton,
                  context.settings.uiThemeMode == AppSettings::UiThemeMode::Dark
                      ? ui_theme::cyan()
                      : ui_theme::amber());

  auto applyTabStyle = [this, &applyTonalStyle,
                        &applyNeutralStyle](Button *button, SettingsTab tab) {
    if (button == nullptr) {
      return;
    }
    if (activeTab == tab) {
      applyTonalStyle(button, ui_theme::cyan());
    } else {
      applyNeutralStyle(button);
    }
  };
  applyTabStyle(timingTabButton, SettingsTab::Timing);
  applyTabStyle(visualTabButton, SettingsTab::Visual);
  applyTabStyle(laneTabButton, SettingsTab::Lane);
  applyTabStyle(miscTabButton, SettingsTab::Misc);
  applyTabStyle(tablesTabButton, SettingsTab::Tables);

  if (rootLayout != nullptr) {
    rootLayout->applyYogaLayout();
  }
  if (scrollView != nullptr) {
    scrollView->refreshContentLayout();
  }
}

void SettingsScene::persistSettings() {
  context.settings.sanitize();
  ui_theme::setActiveMode(context.settings.uiThemeMode ==
                                  AppSettings::UiThemeMode::Light
                              ? ui_theme::ThemeMode::Light
                              : ui_theme::ThemeMode::Dark);
  if (!context.settings.save()) {
    SDL_Log("Failed to save settings");
  }
  context.jukebox.setVisualsEnabled(context.settings.bgaEnabled);
  context.jukebox.setBgaOffsetMs(context.settings.audioOffsetMs);
  context.jukebox.setBgaDisplayMode(context.settings.bgaDisplayMode);
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
