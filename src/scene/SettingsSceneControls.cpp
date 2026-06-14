#include "SettingsSceneShared.h"
#include "../view/ScrollView.h"

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
  const std::string notePriorityLabel =
      formatNotePriorityModeLabel(context.settings.notePriorityMode);
  const std::string invisibleNotesLabel =
      context.settings.showInvisibleNotes ? "Shown" : "Hidden";
  const std::string judgementIndicatorLabel =
      context.settings.judgementIndicatorEnabled ? "Enabled" : "Disabled";
  const std::string judgementIndicatorRenderModeLabel =
      formatJudgementIndicatorRenderModeLabel(
          context.settings.judgementIndicatorRenderMode);

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
  if (summaryNotePriorityValueText != nullptr) {
    summaryNotePriorityValueText->setText(notePriorityLabel);
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
  if (visibleTimeModeText != nullptr) {
    visibleTimeModeText->setText(context.settings.visibleTimeUseMilliseconds
                                     ? "Milliseconds"
                                     : "Green Number");
  }
  if (visibleTimeBpmStrategyText != nullptr) {
    visibleTimeBpmStrategyText->setText("BPM: " +
                                        visibleTimeBpmStrategyLabel);
  }

  if (visibleTimeModeButton != nullptr) {
    if (context.settings.visibleTimeUseMilliseconds) {
      visibleTimeModeButton->setBackgroundColors(Color(35, 68, 62, 255),
                                                 Color(45, 88, 80, 255),
                                                 Color(63, 118, 107, 255));
      visibleTimeModeButton->setBorderColors(Color(97, 157, 142, 255),
                                             Color(120, 187, 169, 255),
                                             Color(145, 214, 195, 255));
    } else {
      visibleTimeModeButton->setBackgroundColors(Color(33, 56, 87, 255),
                                                 Color(43, 72, 110, 255),
                                                 Color(59, 98, 147, 255));
      visibleTimeModeButton->setBorderColors(Color(92, 131, 177, 255),
                                             Color(118, 163, 217, 255),
                                             Color(139, 189, 244, 255));
    }
  }

  if (visibleTimeBpmStrategyButton != nullptr) {
    if (context.settings.visibleTimeBpmStrategy ==
        AppSettings::VisibleTimeBpmStrategy::MostPrevalent) {
      visibleTimeBpmStrategyButton->setBackgroundColors(
          Color(35, 68, 62, 255), Color(45, 88, 80, 255),
          Color(63, 118, 107, 255));
      visibleTimeBpmStrategyButton->setBorderColors(
          Color(97, 157, 142, 255), Color(120, 187, 169, 255),
          Color(145, 214, 195, 255));
    } else {
      visibleTimeBpmStrategyButton->setBackgroundColors(
          Color(33, 56, 87, 255), Color(43, 72, 110, 255),
          Color(59, 98, 147, 255));
      visibleTimeBpmStrategyButton->setBorderColors(
          Color(92, 131, 177, 255), Color(118, 163, 217, 255),
          Color(139, 189, 244, 255));
    }
  }

  if (keysoundModeButton != nullptr) {
    if (context.settings.inputKeysoundEnabled) {
      keysoundModeButton->setBackgroundColors(Color(33, 56, 87, 255),
                                              Color(43, 72, 110, 255),
                                              Color(59, 98, 147, 255));
      keysoundModeButton->setBorderColors(Color(92, 131, 177, 255),
                                          Color(118, 163, 217, 255),
                                          Color(139, 189, 244, 255));
    } else {
      keysoundModeButton->setBackgroundColors(Color(73, 56, 35, 255),
                                              Color(96, 72, 45, 255),
                                              Color(127, 95, 59, 255));
      keysoundModeButton->setBorderColors(Color(165, 120, 74, 255),
                                          Color(194, 141, 88, 255),
                                          Color(224, 163, 103, 255));
    }
  }

  if (notePriorityModeButton != nullptr) {
    if (context.settings.notePriorityMode ==
        AppSettings::NotePriorityMode::Lowest) {
      notePriorityModeButton->setBackgroundColors(Color(33, 56, 87, 255),
                                                  Color(43, 72, 110, 255),
                                                  Color(59, 98, 147, 255));
      notePriorityModeButton->setBorderColors(Color(92, 131, 177, 255),
                                              Color(118, 163, 217, 255),
                                              Color(139, 189, 244, 255));
    } else {
      notePriorityModeButton->setBackgroundColors(Color(35, 68, 62, 255),
                                                  Color(45, 88, 80, 255),
                                                  Color(63, 118, 107, 255));
      notePriorityModeButton->setBorderColors(Color(97, 157, 142, 255),
                                              Color(120, 187, 169, 255),
                                              Color(145, 214, 195, 255));
    }
  }

  if (showInvisibleNotesModeButton != nullptr) {
    if (context.settings.showInvisibleNotes) {
      showInvisibleNotesModeButton->setBackgroundColors(
          Color(35, 68, 62, 255), Color(45, 88, 80, 255),
          Color(63, 118, 107, 255));
      showInvisibleNotesModeButton->setBorderColors(Color(97, 157, 142, 255),
                                                    Color(120, 187, 169, 255),
                                                    Color(145, 214, 195, 255));
    } else {
      showInvisibleNotesModeButton->setBackgroundColors(
          Color(33, 56, 87, 255), Color(43, 72, 110, 255),
          Color(59, 98, 147, 255));
      showInvisibleNotesModeButton->setBorderColors(Color(92, 131, 177, 255),
                                                    Color(118, 163, 217, 255),
                                                    Color(139, 189, 244, 255));
    }
  }

  if (judgementIndicatorModeButton != nullptr) {
    if (context.settings.judgementIndicatorEnabled) {
      judgementIndicatorModeButton->setBackgroundColors(
          Color(35, 68, 62, 255), Color(45, 88, 80, 255),
          Color(63, 118, 107, 255));
      judgementIndicatorModeButton->setBorderColors(Color(97, 157, 142, 255),
                                                    Color(120, 187, 169, 255),
                                                    Color(145, 214, 195, 255));
    } else {
      judgementIndicatorModeButton->setBackgroundColors(
          Color(56, 42, 40, 255), Color(75, 55, 52, 255),
          Color(104, 75, 71, 255));
      judgementIndicatorModeButton->setBorderColors(Color(141, 103, 98, 255),
                                                    Color(176, 127, 121, 255),
                                                    Color(209, 150, 143, 255));
    }
  }

  if (judgementIndicatorRenderModeButton != nullptr) {
    if (context.settings.judgementIndicatorRenderMode ==
        AppSettings::JudgementIndicatorRenderMode::Hud2D) {
      judgementIndicatorRenderModeButton->setBackgroundColors(
          Color(35, 68, 62, 255), Color(45, 88, 80, 255),
          Color(63, 118, 107, 255));
      judgementIndicatorRenderModeButton->setBorderColors(
          Color(97, 157, 142, 255), Color(120, 187, 169, 255),
          Color(145, 214, 195, 255));
    } else {
      judgementIndicatorRenderModeButton->setBackgroundColors(
          Color(33, 56, 87, 255), Color(43, 72, 110, 255),
          Color(59, 98, 147, 255));
      judgementIndicatorRenderModeButton->setBorderColors(
          Color(92, 131, 177, 255), Color(118, 163, 217, 255),
          Color(139, 189, 244, 255));
    }
  }

  if (bgaModeButton != nullptr) {
    if (context.settings.bgaEnabled) {
      bgaModeButton->setBackgroundColors(Color(35, 68, 62, 255),
                                         Color(45, 88, 80, 255),
                                         Color(63, 118, 107, 255));
      bgaModeButton->setBorderColors(Color(97, 157, 142, 255),
                                     Color(120, 187, 169, 255),
                                     Color(145, 214, 195, 255));
    } else {
      bgaModeButton->setBackgroundColors(Color(56, 42, 40, 255),
                                         Color(75, 55, 52, 255),
                                         Color(104, 75, 71, 255));
      bgaModeButton->setBorderColors(Color(141, 103, 98, 255),
                                     Color(176, 127, 121, 255),
                                     Color(209, 150, 143, 255));
    }
  }

  auto applyTabStyle = [this](Button *button, SettingsTab tab) {
    if (button == nullptr) {
      return;
    }
    if (activeTab == tab) {
      button->setBackgroundColors(Color(35, 68, 62, 255),
                                  Color(45, 88, 80, 255),
                                  Color(63, 118, 107, 255));
      button->setBorderColors(Color(97, 157, 142, 255),
                              Color(120, 187, 169, 255),
                              Color(145, 214, 195, 255));
    } else {
      button->setBackgroundColors(Color(28, 40, 58, 255),
                                  Color(36, 52, 75, 255),
                                  Color(61, 87, 118, 255));
      button->setBorderColors(Color(84, 107, 139, 255),
                              Color(108, 136, 174, 255),
                              Color(139, 172, 217, 255));
    }
  };
  applyTabStyle(timingTabButton, SettingsTab::Timing);
  applyTabStyle(visualTabButton, SettingsTab::Visual);
  applyTabStyle(laneTabButton, SettingsTab::Lane);
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
