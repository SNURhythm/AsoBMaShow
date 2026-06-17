#include "SettingsSceneShared.h"
#include "../view/BlockingOverlayView.h"
#include "../view/ScrollView.h"
#include "play/BMSRenderer.h"

using namespace settings_scene;

namespace {
View *makeCardsColumn(const LayoutMetrics &metrics) {
  auto *cardsColumn = new View();
  cardsColumn->setFlexDirection(FlexDirection::Column);
  cardsColumn->setGap(static_cast<float>(metrics.secondaryGap));
  cardsColumn->setWidth(static_cast<float>(metrics.cardsWidth));
  return cardsColumn;
}
} // namespace

void SettingsScene::resetViewState() {
  for (auto *view : views) {
    delete view;
  }
  views.clear();
  rootLayout = nullptr;
  scrollView = nullptr;
  offsetInput = nullptr;
  summaryOffsetValueText = nullptr;
  visualOffsetInput = nullptr;
  summaryVisualOffsetValueText = nullptr;
  visibleTimeInput = nullptr;
  summaryVisibleTimeValueText = nullptr;
  summaryKeysoundValueText = nullptr;
  summaryBgaValueText = nullptr;
  summaryBgaBrightnessValueText = nullptr;
  summaryBgaBlurValueText = nullptr;
  summaryBgaDisplayValueText = nullptr;
  summaryLaneAngleValueText = nullptr;
  summaryLaneLengthValueText = nullptr;
  summaryLaneBeamLengthValueText = nullptr;
  summaryNoteStartPositionValueText = nullptr;
  summaryPreviewPlayAreaWidthValueText = nullptr;
  summaryNotePriorityValueText = nullptr;
  judgementIndicatorYInput = nullptr;
  judgementIndicatorWidthInput = nullptr;
  visibleTimeModeText = nullptr;
  visibleTimeBpmStrategyText = nullptr;
  keysoundModeText = nullptr;
  showInvisibleNotesModeText = nullptr;
  notePriorityModeText = nullptr;
  judgementIndicatorModeText = nullptr;
  judgementIndicatorRenderModeText = nullptr;
  bgaModeText = nullptr;
  bgaDisplayModeText = nullptr;
  visibleTimeModeButton = nullptr;
  visibleTimeBpmStrategyButton = nullptr;
  keysoundModeButton = nullptr;
  showInvisibleNotesModeButton = nullptr;
  notePriorityModeButton = nullptr;
  judgementIndicatorModeButton = nullptr;
  judgementIndicatorRenderModeButton = nullptr;
  bgaModeButton = nullptr;
  bgaDisplayModeButton = nullptr;
  timingTabButton = nullptr;
  visualTabButton = nullptr;
  laneTabButton = nullptr;
  tablesTabButton = nullptr;
  bgaBrightnessInput = nullptr;
  bgaBlurInput = nullptr;
  laneAngleInput = nullptr;
  laneLengthInput = nullptr;
  laneBeamLengthInput = nullptr;
  noteStartPositionInput = nullptr;
  tableUrlInput = nullptr;
  difficultyTableStatusText = nullptr;
  difficultyTableImportModalRoot = nullptr;
  difficultyTableImportProgressFill = nullptr;
  difficultyTableImportTitleText = nullptr;
  difficultyTableImportStatusText = nullptr;
  difficultyTableImportTableText = nullptr;
  difficultyTableImportProgressText = nullptr;
  difficultyTableImportCloseButton = nullptr;
}

void SettingsScene::ensureLayoutUpToDate() {
  const SafeAreaInsets safe = getSafeAreaInsetsUi();
  if (rendering::window_width == lastLayoutWidth &&
      rendering::window_height == lastLayoutHeight && safe.top == lastSafeTop &&
      safe.left == lastSafeLeft && safe.bottom == lastSafeBottom &&
      safe.right == lastSafeRight && rootLayout != nullptr) {
    return;
  }

  resetViewState();
  lastLayoutWidth = rendering::window_width;
  lastLayoutHeight = rendering::window_height;
  lastSafeTop = safe.top;
  lastSafeLeft = safe.left;
  lastSafeBottom = safe.bottom;
  lastSafeRight = safe.right;
  initView();
}

View *SettingsScene::buildVisibleTimeControls(const LayoutMetrics &metrics,
                                              bool includeDescription,
                                              bool compactAdjustments) {
  auto *visibleTimeControls = new View();
  visibleTimeControls->setFlexDirection(FlexDirection::Column);
  visibleTimeControls->setGap(metrics.compact ? 12.0f : 16.0f);
  visibleTimeControls->setAlignItems(YGAlignFlexStart);
  if (includeDescription) {
    visibleTimeControls->addView(makeWrappedText(
        metrics.compact
            ? "600 green = 1000 ms. This controls how long notes stay "
              "visible."
            : "Green Number is the legacy BMS unit for note visible time. "
              "600 green equals 60 frames on a 60 FPS system, which is "
              "1000 ms.",
        metrics.bodyTextSize, Color(150, 171, 193)));
  }

  visibleTimeModeText =
      makeText("", metrics.bodyTextSize + 6, Color(245, 248, 252),
               TextView::CENTER, TextView::MIDDLE);
  visibleTimeModeButton = makeButton(
      metrics.actionButtonWidth, metrics.actionButtonHeight,
      visibleTimeModeText, Color(33, 56, 87, 255), Color(43, 72, 110, 255),
      Color(59, 98, 147, 255), Color(92, 131, 177, 255),
      Color(118, 163, 217, 255), Color(139, 189, 244, 255));
  visibleTimeModeButton->setOnClickListener([this]() {
    context.settings.visibleTimeUseMilliseconds =
        !context.settings.visibleTimeUseMilliseconds;
    persistSettings();
    syncVisibleTimeInputText(true);
  });
  visibleTimeControls->addView(visibleTimeModeButton);

  visibleTimeBpmStrategyText =
      makeText("", metrics.bodyTextSize + 6, Color(245, 248, 252),
               TextView::CENTER, TextView::MIDDLE);
  visibleTimeBpmStrategyButton = makeButton(
      metrics.actionButtonWidth, metrics.actionButtonHeight,
      visibleTimeBpmStrategyText, Color(33, 56, 87, 255),
      Color(43, 72, 110, 255), Color(59, 98, 147, 255),
      Color(92, 131, 177, 255), Color(118, 163, 217, 255),
      Color(139, 189, 244, 255));
  visibleTimeBpmStrategyButton->setOnClickListener([this]() {
    context.settings.visibleTimeBpmStrategy =
        nextVisibleTimeBpmStrategy(context.settings.visibleTimeBpmStrategy);
    persistSettings();
    if (previewRenderer != nullptr) {
      previewRenderer->setVisibleTimeBpmStrategy(
          context.settings.visibleTimeBpmStrategy);
    }
  });
  visibleTimeControls->addView(visibleTimeBpmStrategyButton);

  auto *visibleTimeValueControls = new View();
  visibleTimeValueControls->setFlexDirection(FlexDirection::Row);
  visibleTimeValueControls->setFlexWrap(YGWrapWrap);
  visibleTimeValueControls->setGap(metrics.compact ? 8.0f : 12.0f);
  visibleTimeValueControls->setAlignItems(YGAlignFlexStart);

  auto updateVisibleTime = [this](int delta) {
    context.settings.visibleTimeGreenNumber = adjustVisibleTimeGreenNumber(
        context.settings.visibleTimeGreenNumber,
        context.settings.visibleTimeUseMilliseconds, delta);
    persistSettings();
    syncVisibleTimeInputText(true);
  };

  if (!compactAdjustments) {
    auto *minusVisibleTimeLarge =
        makeStepButton(metrics, metrics.offsetButtonWidthLarge, "-100");
    minusVisibleTimeLarge->setOnClickListener(
        [updateVisibleTime]() { updateVisibleTime(-100); });
    visibleTimeValueControls->addView(minusVisibleTimeLarge);
  }

  auto *minusVisibleTimeSmall =
      makeStepButton(metrics, metrics.offsetButtonWidthSmall, "-10");
  minusVisibleTimeSmall->setOnClickListener(
      [updateVisibleTime]() { updateVisibleTime(-10); });
  visibleTimeValueControls->addView(minusVisibleTimeSmall);

  if (!compactAdjustments) {
    auto *minusVisibleTimeOne =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "-1");
    minusVisibleTimeOne->setOnClickListener(
        [updateVisibleTime]() { updateVisibleTime(-1); });
    visibleTimeValueControls->addView(minusVisibleTimeOne);
  }

  visibleTimeInput = makeNumericInput(metrics);
  visibleTimeInput->onEditingFinished(
      [this](const std::string &) { commitVisibleTimeInput(); });
  visibleTimeValueControls->addView(makeInputFrame(metrics, visibleTimeInput));

  if (!compactAdjustments) {
    auto *plusVisibleTimeOne =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "+1");
    plusVisibleTimeOne->setOnClickListener(
        [updateVisibleTime]() { updateVisibleTime(1); });
    visibleTimeValueControls->addView(plusVisibleTimeOne);
  }

  auto *plusVisibleTimeSmall =
      makeStepButton(metrics, metrics.offsetButtonWidthSmall, "+10");
  plusVisibleTimeSmall->setOnClickListener(
      [updateVisibleTime]() { updateVisibleTime(10); });
  visibleTimeValueControls->addView(plusVisibleTimeSmall);

  if (!compactAdjustments) {
    auto *plusVisibleTimeLarge =
        makeStepButton(metrics, metrics.offsetButtonWidthLarge, "+100");
    plusVisibleTimeLarge->setOnClickListener(
        [updateVisibleTime]() { updateVisibleTime(100); });
    visibleTimeValueControls->addView(plusVisibleTimeLarge);

    auto *resetVisibleTime = makeResetButton(metrics);
    resetVisibleTime->setOnClickListener([this]() {
      context.settings.visibleTimeGreenNumber = 400;
      persistSettings();
      syncVisibleTimeInputText(true);
    });
    visibleTimeValueControls->addView(resetVisibleTime);
  }

  visibleTimeControls->addView(visibleTimeValueControls);
  return visibleTimeControls;
}

void SettingsScene::buildPreviewLayout(const LayoutMetrics &metrics) {
  rootLayout->setFlexDirection(FlexDirection::Row);
  rootLayout->setJustifyContent(YGJustifyFlexEnd);
  rootLayout->setAlignItems(YGAlignFlexStart);

  const int foldButtonSize = metrics.compact ? 54 : 58;
  constexpr int previewPanelPageCount = 2;
  if (previewPanelPage < 0 || previewPanelPage >= previewPanelPageCount) {
    previewPanelPage = 0;
  }
  const int panelWidth =
      previewPanelFolded
          ? foldButtonSize
          : (metrics.compact ? std::min(metrics.contentWidth, 520) : 380);
  auto *previewPanel = new View();
  previewPanel->setWidth(static_cast<float>(panelWidth));
  previewPanel->setPadding(
      Edge::All,
      static_cast<float>(previewPanelFolded ? 0 : metrics.cardPadding));
  previewPanel->setGap(metrics.compact ? 12.0f : 16.0f);
  previewPanel->setFlexDirection(FlexDirection::Column);
  previewPanel->setAlignItems(previewPanelFolded ? YGAlignFlexEnd
                                                 : YGAlignStretch);
  previewPanel->setBackgroundColor(Color(12, 20, 32, 184));
  previewPanel->setBorderColor(Color(78, 105, 140, 220));
  previewPanel->setBorderWidth(2);

  auto makeFoldButton = [this, foldButtonSize](const std::string &label) {
    auto *button =
        makeButton(foldButtonSize, foldButtonSize,
                   makeText(label, 28, Color(239, 244, 251), TextView::CENTER,
                            TextView::MIDDLE),
                   Color(22, 33, 49, 190), Color(31, 46, 67, 220),
                   Color(53, 78, 110, 240), Color(96, 121, 156, 230),
                   Color(120, 151, 190, 245), Color(148, 186, 231, 255));
    button->setOnClickListener([this]() {
      previewPanelFolded = !previewPanelFolded;
      lastLayoutWidth = -1;
    });
    return button;
  };

  if (previewPanelFolded) {
    previewPanel->addView(makeFoldButton("<"));
    rootLayout->addView(previewPanel);
    addView(rootLayout);
    rootLayout->applyYogaLayout();
    refreshSettingsText();
    return;
  }

  auto *previewHeader = new View();
  previewHeader->setFlexDirection(FlexDirection::Row);
  previewHeader->setAlignItems(YGAlignCenter);
  previewHeader->setJustifyContent(YGJustifySpaceBetween);
  previewHeader->addView(
      makeText("Preview", metrics.sectionTitleSize, Color(244, 248, 255)));

  auto *previewHeaderActions = new View();
  previewHeaderActions->setFlexDirection(FlexDirection::Row);
  previewHeaderActions->setGap(metrics.compact ? 8.0f : 10.0f);
  previewHeaderActions->setAlignItems(YGAlignCenter);
  auto *pageButton = makeButton(
      metrics.compact ? 78 : 88, foldButtonSize,
      makeText(std::to_string(previewPanelPage + 1) + "/2",
               metrics.smallTextSize, Color(239, 244, 251), TextView::CENTER,
               TextView::MIDDLE),
      Color(22, 33, 49, 190), Color(31, 46, 67, 220),
      Color(53, 78, 110, 240), Color(96, 121, 156, 230),
      Color(120, 151, 190, 245), Color(148, 186, 231, 255));
  pageButton->setOnClickListener([this]() {
    previewPanelPage = (previewPanelPage + 1) % 2;
    lastLayoutWidth = -1;
  });
  previewHeaderActions->addView(pageButton);
  previewHeaderActions->addView(makeFoldButton(">"));
  previewHeader->addView(previewHeaderActions);
  previewPanel->addView(previewHeader);

  if (previewPanelPage == 0) {
    previewPanel->addView(
        makeSummaryRow(metrics, "Visible Time", &summaryVisibleTimeValueText));
    previewPanel->addView(buildVisibleTimeControls(metrics, false, true));
    previewPanel->addView(
        makeSummaryRow(metrics, "Lane Angle", &summaryLaneAngleValueText));
    auto *angleControls = new View();
    angleControls->setFlexDirection(FlexDirection::Row);
    angleControls->setFlexWrap(YGWrapWrap);
    angleControls->setGap(metrics.compact ? 8.0f : 10.0f);
    auto updateLaneAngle = [this](float delta) {
      context.settings.laneAngleDegrees =
          clampLaneAngle(context.settings.laneAngleDegrees + delta);
      persistSettings();
    };
    auto *minusAngle =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "-1");
    minusAngle->setOnClickListener(
        [updateLaneAngle]() { updateLaneAngle(-1.0f); });
    angleControls->addView(minusAngle);
    auto *plusAngle =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "+1");
    plusAngle->setOnClickListener(
        [updateLaneAngle]() { updateLaneAngle(1.0f); });
    angleControls->addView(plusAngle);
    auto *resetAngle = makeResetButton(metrics);
    resetAngle->setOnClickListener([this]() {
      context.settings.laneAngleDegrees =
          AppSettings::kDefaultLaneAngleDegrees;
      persistSettings();
    });
    angleControls->addView(resetAngle);
    previewPanel->addView(angleControls);

    previewPanel->addView(
        makeSummaryRow(metrics, "Lane Length", &summaryLaneLengthValueText));
    auto *lengthControls = new View();
    lengthControls->setFlexDirection(FlexDirection::Row);
    lengthControls->setFlexWrap(YGWrapWrap);
    lengthControls->setGap(metrics.compact ? 8.0f : 10.0f);
    auto updateLaneLength = [this](float delta) {
      context.settings.laneLength =
          clampLaneLength(context.settings.laneLength + delta);
      persistSettings();
    };
    auto *minusLength =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "-0.5");
    minusLength->setOnClickListener(
        [updateLaneLength]() { updateLaneLength(-0.5f); });
    lengthControls->addView(minusLength);
    auto *plusLength =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "+0.5");
    plusLength->setOnClickListener(
        [updateLaneLength]() { updateLaneLength(0.5f); });
    lengthControls->addView(plusLength);
    auto *resetLength = makeResetButton(metrics);
    resetLength->setOnClickListener([this]() {
      context.settings.laneLength = AppSettings::kDefaultLaneLength;
      persistSettings();
    });
    lengthControls->addView(resetLength);
    previewPanel->addView(lengthControls);
  } else {
    previewPanel->addView(makeSummaryRow(
        metrics, "Note Start", &summaryNoteStartPositionValueText));
    auto *noteStartControls = new View();
    noteStartControls->setFlexDirection(FlexDirection::Row);
    noteStartControls->setFlexWrap(YGWrapWrap);
    noteStartControls->setGap(metrics.compact ? 8.0f : 10.0f);
    auto updateNoteStartPosition = [this](int deltaPercent) {
      context.settings.noteStartPositionPercent =
          clampNoteStartPositionPercent(
              context.settings.noteStartPositionPercent + deltaPercent);
      persistSettings();
    };
    auto *minusNoteStart =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "-10%");
    minusNoteStart->setOnClickListener(
        [updateNoteStartPosition]() { updateNoteStartPosition(-10); });
    noteStartControls->addView(minusNoteStart);
    auto *plusNoteStart =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "+10%");
    plusNoteStart->setOnClickListener(
        [updateNoteStartPosition]() { updateNoteStartPosition(10); });
    noteStartControls->addView(plusNoteStart);
    auto *resetNoteStart = makeResetButton(metrics);
    resetNoteStart->setOnClickListener([this]() {
      context.settings.noteStartPositionPercent =
          AppSettings::kDefaultNoteStartPositionPercent;
      persistSettings();
    });
    noteStartControls->addView(resetNoteStart);
    previewPanel->addView(noteStartControls);

    previewPanel->addView(makeSummaryRow(
        metrics, "Beam Length", &summaryLaneBeamLengthValueText));
    auto *beamControls = new View();
    beamControls->setFlexDirection(FlexDirection::Row);
    beamControls->setFlexWrap(YGWrapWrap);
    beamControls->setGap(metrics.compact ? 8.0f : 10.0f);
    auto updateLaneBeamLength = [this](int deltaPercent) {
      context.settings.laneBeamLengthPercent = clampLaneBeamLengthPercent(
          context.settings.laneBeamLengthPercent + deltaPercent);
      persistSettings();
    };
    auto *minusBeam =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "-10%");
    minusBeam->setOnClickListener(
        [updateLaneBeamLength]() { updateLaneBeamLength(-10); });
    beamControls->addView(minusBeam);
    auto *plusBeam =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "+10%");
    plusBeam->setOnClickListener(
        [updateLaneBeamLength]() { updateLaneBeamLength(10); });
    beamControls->addView(plusBeam);
    auto *resetBeam = makeResetButton(metrics);
    resetBeam->setOnClickListener([this]() {
      context.settings.laneBeamLengthPercent =
          AppSettings::kDefaultLaneBeamLengthPercent;
      persistSettings();
    });
    beamControls->addView(resetBeam);
    previewPanel->addView(beamControls);

    previewPanel->addView(makeSummaryRow(
        metrics, "Play Width 7K", &summaryPreviewPlayAreaWidthValueText));
    auto *playAreaWidthControls = new View();
    playAreaWidthControls->setFlexDirection(FlexDirection::Row);
    playAreaWidthControls->setFlexWrap(YGWrapWrap);
    playAreaWidthControls->setGap(metrics.compact ? 8.0f : 10.0f);
    auto updatePreviewPlayAreaWidth = [this](float delta) {
      constexpr int previewKeyMode = 7;
      context.settings.setPlayAreaWidthForKeyMode(
          previewKeyMode,
          clampPlayAreaWidth(
              context.settings.playAreaWidthForKeyMode(previewKeyMode) +
              delta));
      persistSettings();
    };
    auto *minusWidth =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "-0.5");
    minusWidth->setOnClickListener(
        [updatePreviewPlayAreaWidth]() { updatePreviewPlayAreaWidth(-0.5f); });
    playAreaWidthControls->addView(minusWidth);
    auto *plusWidth =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "+0.5");
    plusWidth->setOnClickListener(
        [updatePreviewPlayAreaWidth]() { updatePreviewPlayAreaWidth(0.5f); });
    playAreaWidthControls->addView(plusWidth);
    auto *resetWidth = makeResetButton(metrics);
    resetWidth->setOnClickListener([this]() {
      context.settings.setPlayAreaWidthForKeyMode(
          7, AppSettings::kDefaultPlayAreaWidth);
      persistSettings();
    });
    playAreaWidthControls->addView(resetWidth);
    previewPanel->addView(playAreaWidthControls);
  }

  auto *restartButton = makeButton(
      metrics.actionButtonWidth, metrics.actionButtonHeight,
      makeText("Restart", metrics.bodyTextSize + 4, Color(239, 244, 251),
               TextView::CENTER, TextView::MIDDLE),
      Color(28, 40, 58, 255), Color(36, 52, 75, 255), Color(61, 87, 118, 255),
      Color(84, 107, 139, 255), Color(108, 136, 174, 255),
      Color(139, 172, 217, 255));
  restartButton->setOnClickListener([this]() { resetPreviewSimulation(); });
  previewPanel->addView(restartButton);

  auto *doneButton = makeButton(
      metrics.actionButtonWidth, metrics.actionButtonHeight,
      makeText("Done", metrics.bodyTextSize + 4, Color(237, 243, 252),
               TextView::CENTER, TextView::MIDDLE),
      Color(22, 33, 49, 255), Color(31, 46, 67, 255), Color(53, 78, 110, 255),
      Color(96, 121, 156, 255), Color(120, 151, 190, 255),
      Color(148, 186, 231, 255));
  doneButton->setOnClickListener([this]() { stopLanePreview(); });
  previewPanel->addView(doneButton);

  rootLayout->addView(previewPanel);
  addView(rootLayout);
  rootLayout->applyYogaLayout();
  refreshSettingsText();
  return;
}

View *SettingsScene::buildTimingTab(const LayoutMetrics &metrics) {
  auto *cardsColumn = makeCardsColumn(metrics);
  auto *offsetControls = new View();
  offsetControls->setFlexDirection(FlexDirection::Row);
  offsetControls->setFlexWrap(YGWrapWrap);
  offsetControls->setGap(metrics.compact ? 8.0f : 12.0f);
  offsetControls->setAlignItems(YGAlignFlexStart);

  auto updateOffset = [this](int delta) {
    context.settings.audioOffsetMs =
        clampOffset(context.settings.audioOffsetMs + delta);
    persistSettings();
    syncOffsetInputText(true);
  };

  auto *minusTen =
      makeButton(metrics.offsetButtonWidthLarge, metrics.actionButtonHeight,
                 makeText("-10", metrics.bodyTextSize + 4, Color(239, 244, 251),
                          TextView::CENTER, TextView::MIDDLE),
                 Color(28, 40, 58, 255), Color(36, 52, 75, 255),
                 Color(61, 87, 118, 255), Color(84, 107, 139, 255),
                 Color(108, 136, 174, 255), Color(139, 172, 217, 255));
  minusTen->setOnClickListener([updateOffset]() { updateOffset(-10); });
  offsetControls->addView(minusTen);

  auto *minusOne =
      makeButton(metrics.offsetButtonWidthSmall, metrics.actionButtonHeight,
                 makeText("-1", metrics.bodyTextSize + 4, Color(239, 244, 251),
                          TextView::CENTER, TextView::MIDDLE),
                 Color(28, 40, 58, 255), Color(36, 52, 75, 255),
                 Color(61, 87, 118, 255), Color(84, 107, 139, 255),
                 Color(108, 136, 174, 255), Color(139, 172, 217, 255));
  minusOne->setOnClickListener([updateOffset]() { updateOffset(-1); });
  offsetControls->addView(minusOne);

  auto *offsetValue = new View();
  offsetValue->setWidth(static_cast<float>(metrics.offsetValueWidth));
  offsetValue->setHeight(static_cast<float>(metrics.actionButtonHeight));
  offsetValue->setBackgroundColor(Color(10, 17, 28, 255));
  offsetValue->setBorderColor(Color(78, 105, 140, 255));
  offsetValue->setBorderWidth(2);
  offsetInput = new TextInputBox(kFontPath, metrics.bodyTextSize + 6);
  offsetInput->setText("");
  offsetInput->setSize(metrics.offsetValueWidth, metrics.actionButtonHeight);
  offsetInput->setBackgroundColor(Color(0, 0, 0, 0));
  offsetInput->setBorderWidth(0);
  offsetInput->setAlign(TextView::CENTER);
  offsetInput->setVAlign(TextView::MIDDLE);
  offsetInput->setColor({244, 248, 255, 255});
  offsetInput->onEditingFinished(
      [this](const std::string &) { commitOffsetInput(); });
  offsetValue->addView(offsetInput);
  offsetControls->addView(offsetValue);

  auto *plusOne =
      makeButton(metrics.offsetButtonWidthSmall, metrics.actionButtonHeight,
                 makeText("+1", metrics.bodyTextSize + 4, Color(239, 244, 251),
                          TextView::CENTER, TextView::MIDDLE),
                 Color(28, 40, 58, 255), Color(36, 52, 75, 255),
                 Color(61, 87, 118, 255), Color(84, 107, 139, 255),
                 Color(108, 136, 174, 255), Color(139, 172, 217, 255));
  plusOne->setOnClickListener([updateOffset]() { updateOffset(1); });
  offsetControls->addView(plusOne);

  auto *plusTen =
      makeButton(metrics.offsetButtonWidthLarge, metrics.actionButtonHeight,
                 makeText("+10", metrics.bodyTextSize + 4, Color(239, 244, 251),
                          TextView::CENTER, TextView::MIDDLE),
                 Color(28, 40, 58, 255), Color(36, 52, 75, 255),
                 Color(61, 87, 118, 255), Color(84, 107, 139, 255),
                 Color(108, 136, 174, 255), Color(139, 172, 217, 255));
  plusTen->setOnClickListener([updateOffset]() { updateOffset(10); });
  offsetControls->addView(plusTen);

  auto *resetOffset = makeButton(
      metrics.resetButtonWidth, metrics.actionButtonHeight,
      makeText("Reset", metrics.bodyTextSize + 4, Color(248, 241, 236),
               TextView::CENTER, TextView::MIDDLE),
      Color(96, 57, 44, 255), Color(117, 72, 55, 255), Color(153, 96, 74, 255),
      Color(165, 105, 79, 255), Color(193, 124, 93, 255),
      Color(219, 145, 108, 255));
  resetOffset->setOnClickListener([this]() {
    context.settings.audioOffsetMs = 0;
    persistSettings();
    syncOffsetInputText(true);
  });
  offsetControls->addView(resetOffset);

  cardsColumn->addView(
      makeCard(metrics, "Audio Offset",
               metrics.compact
                   ? "Negative values make chart audio feel earlier."
                   : "Negative values delay gameplay and BGA so chart audio, "
                     "auto-timed keysounds, and replay keysounds feel earlier.",
               offsetControls, metrics.offsetCardHeight, metrics.cardsWidth));

  auto *visualOffsetControls = new View();
  visualOffsetControls->setFlexDirection(FlexDirection::Row);
  visualOffsetControls->setFlexWrap(YGWrapWrap);
  visualOffsetControls->setGap(metrics.compact ? 8.0f : 12.0f);
  visualOffsetControls->setAlignItems(YGAlignFlexStart);

  auto updateVisualOffset = [this](int delta) {
    context.settings.visualOffsetMs =
        clampVisualOffset(context.settings.visualOffsetMs + delta);
    persistSettings();
    syncVisualOffsetInputText(true);
  };

  auto *minusVisualTen =
      makeButton(metrics.offsetButtonWidthLarge, metrics.actionButtonHeight,
                 makeText("-10", metrics.bodyTextSize + 4, Color(239, 244, 251),
                          TextView::CENTER, TextView::MIDDLE),
                 Color(28, 40, 58, 255), Color(36, 52, 75, 255),
                 Color(61, 87, 118, 255), Color(84, 107, 139, 255),
                 Color(108, 136, 174, 255), Color(139, 172, 217, 255));
  minusVisualTen->setOnClickListener(
      [updateVisualOffset]() { updateVisualOffset(-10); });
  visualOffsetControls->addView(minusVisualTen);

  auto *minusVisualOne =
      makeButton(metrics.offsetButtonWidthSmall, metrics.actionButtonHeight,
                 makeText("-1", metrics.bodyTextSize + 4, Color(239, 244, 251),
                          TextView::CENTER, TextView::MIDDLE),
                 Color(28, 40, 58, 255), Color(36, 52, 75, 255),
                 Color(61, 87, 118, 255), Color(84, 107, 139, 255),
                 Color(108, 136, 174, 255), Color(139, 172, 217, 255));
  minusVisualOne->setOnClickListener(
      [updateVisualOffset]() { updateVisualOffset(-1); });
  visualOffsetControls->addView(minusVisualOne);

  auto *visualOffsetValue = new View();
  visualOffsetValue->setWidth(static_cast<float>(metrics.offsetValueWidth));
  visualOffsetValue->setHeight(static_cast<float>(metrics.actionButtonHeight));
  visualOffsetValue->setBackgroundColor(Color(10, 17, 28, 255));
  visualOffsetValue->setBorderColor(Color(78, 105, 140, 255));
  visualOffsetValue->setBorderWidth(2);
  visualOffsetInput = new TextInputBox(kFontPath, metrics.bodyTextSize + 6);
  visualOffsetInput->setText("");
  visualOffsetInput->setSize(metrics.offsetValueWidth,
                             metrics.actionButtonHeight);
  visualOffsetInput->setBackgroundColor(Color(0, 0, 0, 0));
  visualOffsetInput->setBorderWidth(0);
  visualOffsetInput->setAlign(TextView::CENTER);
  visualOffsetInput->setVAlign(TextView::MIDDLE);
  visualOffsetInput->setColor({244, 248, 255, 255});
  visualOffsetInput->onEditingFinished(
      [this](const std::string &) { commitVisualOffsetInput(); });
  visualOffsetValue->addView(visualOffsetInput);
  visualOffsetControls->addView(visualOffsetValue);

  auto *plusVisualOne =
      makeButton(metrics.offsetButtonWidthSmall, metrics.actionButtonHeight,
                 makeText("+1", metrics.bodyTextSize + 4, Color(239, 244, 251),
                          TextView::CENTER, TextView::MIDDLE),
                 Color(28, 40, 58, 255), Color(36, 52, 75, 255),
                 Color(61, 87, 118, 255), Color(84, 107, 139, 255),
                 Color(108, 136, 174, 255), Color(139, 172, 217, 255));
  plusVisualOne->setOnClickListener(
      [updateVisualOffset]() { updateVisualOffset(1); });
  visualOffsetControls->addView(plusVisualOne);

  auto *plusVisualTen =
      makeButton(metrics.offsetButtonWidthLarge, metrics.actionButtonHeight,
                 makeText("+10", metrics.bodyTextSize + 4, Color(239, 244, 251),
                          TextView::CENTER, TextView::MIDDLE),
                 Color(28, 40, 58, 255), Color(36, 52, 75, 255),
                 Color(61, 87, 118, 255), Color(84, 107, 139, 255),
                 Color(108, 136, 174, 255), Color(139, 172, 217, 255));
  plusVisualTen->setOnClickListener(
      [updateVisualOffset]() { updateVisualOffset(10); });
  visualOffsetControls->addView(plusVisualTen);

  auto *resetVisualOffset = makeButton(
      metrics.resetButtonWidth, metrics.actionButtonHeight,
      makeText("Reset", metrics.bodyTextSize + 4, Color(248, 241, 236),
               TextView::CENTER, TextView::MIDDLE),
      Color(96, 57, 44, 255), Color(117, 72, 55, 255), Color(153, 96, 74, 255),
      Color(165, 105, 79, 255), Color(193, 124, 93, 255),
      Color(219, 145, 108, 255));
  resetVisualOffset->setOnClickListener([this]() {
    context.settings.visualOffsetMs = 0;
    persistSettings();
    syncVisualOffsetInputText(true);
  });
  visualOffsetControls->addView(resetVisualOffset);

  cardsColumn->addView(makeCard(
      metrics, "Visual Offset",
      metrics.compact
          ? "Adjusts note display time only."
          : "Adjusts note display time only. BGA timing stays on the chart "
            "audio timeline.",
      visualOffsetControls, metrics.offsetCardHeight, metrics.cardsWidth));

  auto *judgementIndicatorControls = new View();
  judgementIndicatorControls->setFlexDirection(FlexDirection::Column);
  judgementIndicatorControls->setGap(metrics.compact ? 12.0f : 16.0f);
  judgementIndicatorControls->setAlignItems(YGAlignFlexStart);

  auto *judgementIndicatorModeControls = new View();
  judgementIndicatorModeControls->setFlexDirection(FlexDirection::Row);
  judgementIndicatorModeControls->setFlexWrap(YGWrapWrap);
  judgementIndicatorModeControls->setGap(metrics.compact ? 8.0f : 12.0f);
  judgementIndicatorModeControls->setAlignItems(YGAlignFlexStart);

  judgementIndicatorModeText =
      makeText("", metrics.bodyTextSize + 6, Color(245, 248, 252),
               TextView::CENTER, TextView::MIDDLE);
  judgementIndicatorModeButton =
      makeButton(metrics.actionButtonWidth, metrics.actionButtonHeight,
                 judgementIndicatorModeText, Color(35, 68, 62, 255),
                 Color(45, 88, 80, 255), Color(63, 118, 107, 255),
                 Color(97, 157, 142, 255), Color(120, 187, 169, 255),
                 Color(145, 214, 195, 255));
  judgementIndicatorModeButton->setOnClickListener([this]() {
    context.settings.judgementIndicatorEnabled =
        !context.settings.judgementIndicatorEnabled;
    persistSettings();
  });
  judgementIndicatorModeControls->addView(judgementIndicatorModeButton);

  judgementIndicatorRenderModeText =
      makeText("", metrics.bodyTextSize + 6, Color(245, 248, 252),
               TextView::CENTER, TextView::MIDDLE);
  judgementIndicatorRenderModeButton =
      makeButton(metrics.actionButtonWidth, metrics.actionButtonHeight,
                 judgementIndicatorRenderModeText, Color(33, 56, 87, 255),
                 Color(43, 72, 110, 255), Color(59, 98, 147, 255),
                 Color(92, 131, 177, 255), Color(118, 163, 217, 255),
                 Color(139, 189, 244, 255));
  judgementIndicatorRenderModeButton->setOnClickListener([this]() {
    context.settings.judgementIndicatorRenderMode =
        nextJudgementIndicatorRenderMode(
            context.settings.judgementIndicatorRenderMode);
    persistSettings();
  });
  judgementIndicatorModeControls->addView(judgementIndicatorRenderModeButton);
  judgementIndicatorControls->addView(judgementIndicatorModeControls);

  judgementIndicatorControls->addView(
      makeText("Y Position", metrics.bodyTextSize, Color(168, 186, 209)));
  auto *judgementIndicatorYControls = new View();
  judgementIndicatorYControls->setFlexDirection(FlexDirection::Row);
  judgementIndicatorYControls->setFlexWrap(YGWrapWrap);
  judgementIndicatorYControls->setGap(metrics.compact ? 8.0f : 12.0f);
  judgementIndicatorYControls->setAlignItems(YGAlignFlexStart);
  auto updateJudgementIndicatorY = [this](int deltaPercent) {
    const int currentPercent =
        judgementIndicatorYToPercent(context.settings.judgementIndicatorY);
    const int nextPercent = std::clamp(currentPercent + deltaPercent, 0, 100);
    context.settings.judgementIndicatorY =
        judgementIndicatorPercentToY(nextPercent);
    persistSettings();
    syncJudgementIndicatorYInputText(true);
  };

  auto *minusIndicatorYLarge =
      makeStepButton(metrics, metrics.offsetButtonWidthLarge, "-10%");
  minusIndicatorYLarge->setOnClickListener(
      [updateJudgementIndicatorY]() { updateJudgementIndicatorY(-10); });
  judgementIndicatorYControls->addView(minusIndicatorYLarge);
  auto *minusIndicatorYSmall =
      makeStepButton(metrics, metrics.offsetButtonWidthSmall, "-1%");
  minusIndicatorYSmall->setOnClickListener(
      [updateJudgementIndicatorY]() { updateJudgementIndicatorY(-1); });
  judgementIndicatorYControls->addView(minusIndicatorYSmall);
  judgementIndicatorYInput = makeNumericInput(metrics);
  judgementIndicatorYInput->onEditingFinished(
      [this](const std::string &) { commitJudgementIndicatorYInput(); });
  judgementIndicatorYControls->addView(
      makeInputFrame(metrics, judgementIndicatorYInput));
  auto *plusIndicatorYSmall =
      makeStepButton(metrics, metrics.offsetButtonWidthSmall, "+1%");
  plusIndicatorYSmall->setOnClickListener(
      [updateJudgementIndicatorY]() { updateJudgementIndicatorY(1); });
  judgementIndicatorYControls->addView(plusIndicatorYSmall);
  auto *plusIndicatorYLarge =
      makeStepButton(metrics, metrics.offsetButtonWidthLarge, "+10%");
  plusIndicatorYLarge->setOnClickListener(
      [updateJudgementIndicatorY]() { updateJudgementIndicatorY(10); });
  judgementIndicatorYControls->addView(plusIndicatorYLarge);
  auto *resetIndicatorY = makeResetButton(metrics);
  resetIndicatorY->setOnClickListener([this]() {
    context.settings.judgementIndicatorY =
        AppSettings::kDefaultJudgementIndicatorY;
    persistSettings();
    syncJudgementIndicatorYInputText(true);
  });
  judgementIndicatorYControls->addView(resetIndicatorY);
  judgementIndicatorControls->addView(judgementIndicatorYControls);

  judgementIndicatorControls->addView(
      makeText("Width", metrics.bodyTextSize, Color(168, 186, 209)));
  auto *judgementIndicatorWidthControls = new View();
  judgementIndicatorWidthControls->setFlexDirection(FlexDirection::Row);
  judgementIndicatorWidthControls->setFlexWrap(YGWrapWrap);
  judgementIndicatorWidthControls->setGap(metrics.compact ? 8.0f : 12.0f);
  judgementIndicatorWidthControls->setAlignItems(YGAlignFlexStart);
  auto updateJudgementIndicatorWidth = [this](int deltaPercent) {
    const int currentPercent = judgementIndicatorWidthScaleToPercent(
        context.settings.judgementIndicatorWidthScale);
    const int minPercent = judgementIndicatorWidthScaleToPercent(
        AppSettings::kMinJudgementIndicatorWidthScale);
    const int maxPercent = judgementIndicatorWidthScaleToPercent(
        AppSettings::kMaxJudgementIndicatorWidthScale);
    const int nextPercent =
        std::clamp(currentPercent + deltaPercent, minPercent, maxPercent);
    context.settings.judgementIndicatorWidthScale =
        judgementIndicatorWidthPercentToScale(nextPercent);
    persistSettings();
    syncJudgementIndicatorWidthInputText(true);
  };

  auto *minusIndicatorWidthLarge =
      makeStepButton(metrics, metrics.offsetButtonWidthLarge, "-10%");
  minusIndicatorWidthLarge->setOnClickListener(
      [updateJudgementIndicatorWidth]() {
        updateJudgementIndicatorWidth(-10);
      });
  judgementIndicatorWidthControls->addView(minusIndicatorWidthLarge);
  auto *minusIndicatorWidthSmall =
      makeStepButton(metrics, metrics.offsetButtonWidthSmall, "-1%");
  minusIndicatorWidthSmall->setOnClickListener(
      [updateJudgementIndicatorWidth]() { updateJudgementIndicatorWidth(-1); });
  judgementIndicatorWidthControls->addView(minusIndicatorWidthSmall);
  judgementIndicatorWidthInput = makeNumericInput(metrics);
  judgementIndicatorWidthInput->onEditingFinished(
      [this](const std::string &) { commitJudgementIndicatorWidthInput(); });
  judgementIndicatorWidthControls->addView(
      makeInputFrame(metrics, judgementIndicatorWidthInput));
  auto *plusIndicatorWidthSmall =
      makeStepButton(metrics, metrics.offsetButtonWidthSmall, "+1%");
  plusIndicatorWidthSmall->setOnClickListener(
      [updateJudgementIndicatorWidth]() { updateJudgementIndicatorWidth(1); });
  judgementIndicatorWidthControls->addView(plusIndicatorWidthSmall);
  auto *plusIndicatorWidthLarge =
      makeStepButton(metrics, metrics.offsetButtonWidthLarge, "+10%");
  plusIndicatorWidthLarge->setOnClickListener(
      [updateJudgementIndicatorWidth]() { updateJudgementIndicatorWidth(10); });
  judgementIndicatorWidthControls->addView(plusIndicatorWidthLarge);
  auto *resetIndicatorWidth = makeResetButton(metrics);
  resetIndicatorWidth->setOnClickListener([this]() {
    context.settings.judgementIndicatorWidthScale =
        AppSettings::kDefaultJudgementIndicatorWidthScale;
    persistSettings();
    syncJudgementIndicatorWidthInputText(true);
  });
  judgementIndicatorWidthControls->addView(resetIndicatorWidth);
  judgementIndicatorControls->addView(judgementIndicatorWidthControls);

  cardsColumn->addView(makeCard(
      metrics, "Judgement Indicator",
      metrics.compact
          ? "Y: 0% bottom, 50% center, 100% top. Width: 100% base."
          : "Y position is vertical placement: 0% is the "
            "judgement-line/bottom side, 50% is center, and 100% is top. "
            "Width percent scales the selected mode's base width; 100% is "
            "default. 3D draws on the lane plane; HUD draws screen-flat.",
      judgementIndicatorControls, metrics.visibleTimeCardHeight,
      metrics.cardsWidth));

  auto *secondaryCards = new View();
  secondaryCards->setFlexDirection(
      metrics.useDualCardRow ? FlexDirection::Row : FlexDirection::Column);
  secondaryCards->setGap(static_cast<float>(metrics.secondaryGap));

  auto *keysoundControls = new View();
  keysoundControls->setFlexDirection(FlexDirection::Column);
  keysoundControls->setGap(metrics.compact ? 12.0f : 16.0f);
  keysoundControls->setAlignItems(YGAlignFlexStart);
  keysoundControls->addView(makeWrappedText(
      metrics.compact
          ? "Switch between manual hits and chart-timed playback."
          : "Tap to switch modes. The current selection is shown on "
            "the right.",
      metrics.bodyTextSize, Color(150, 171, 193)));
  keysoundModeText =
      makeText("", metrics.bodyTextSize + 6, Color(245, 248, 252),
               TextView::CENTER, TextView::MIDDLE);
  keysoundModeButton = makeButton(
      metrics.actionButtonWidth, metrics.actionButtonHeight, keysoundModeText,
      Color(33, 56, 87, 255), Color(43, 72, 110, 255), Color(59, 98, 147, 255),
      Color(92, 131, 177, 255), Color(118, 163, 217, 255),
      Color(139, 189, 244, 255));
  keysoundModeButton->setOnClickListener([this]() {
    context.settings.inputKeysoundEnabled =
        !context.settings.inputKeysoundEnabled;
    persistSettings();
  });
  keysoundControls->addView(keysoundModeButton);
  secondaryCards->addView(makeCard(
      metrics, "Input Keysounds",
      metrics.compact
          ? "Manual hits keep classic feedback. Auto timed follows chart "
            "timing."
          : "Keep manual key clicks for classic BMS feedback, or switch to "
            "auto-timed playback for cleaner timing practice.",
      keysoundControls, metrics.modeCardHeight, metrics.secondaryCardWidth));

  auto *notePriorityControls = new View();
  notePriorityControls->setFlexDirection(FlexDirection::Column);
  notePriorityControls->setGap(metrics.compact ? 12.0f : 16.0f);
  notePriorityControls->setAlignItems(YGAlignFlexStart);
  notePriorityControls->addView(makeWrappedText(
      metrics.compact
          ? "Choose which hittable note a lane press judges first."
          : "Choose which hittable note a lane press judges first when "
            "multiple notes are inside the input window.",
      metrics.bodyTextSize, Color(150, 171, 193)));
  notePriorityModeText =
      makeText("", metrics.bodyTextSize + 6, Color(245, 248, 252),
               TextView::CENTER, TextView::MIDDLE);
  notePriorityModeButton = makeButton(
      metrics.actionButtonWidth, metrics.actionButtonHeight,
      notePriorityModeText, Color(33, 56, 87, 255), Color(43, 72, 110, 255),
      Color(59, 98, 147, 255), Color(92, 131, 177, 255),
      Color(118, 163, 217, 255), Color(139, 189, 244, 255));
  notePriorityModeButton->setOnClickListener([this]() {
    context.settings.notePriorityMode =
        nextNotePriorityMode(context.settings.notePriorityMode);
    persistSettings();
  });
  notePriorityControls->addView(notePriorityModeButton);
  secondaryCards->addView(makeCard(
      metrics, "Note Priority",
      metrics.compact ? "Lowest keeps the original frontmost-note behavior."
                      : "Lowest keeps the original frontmost-note behavior. "
                        "Other modes can prefer a following note based on "
                        "combo, timing distance, or score.",
      notePriorityControls, metrics.modeCardHeight,
      metrics.secondaryCardWidth));

  cardsColumn->addView(secondaryCards);
  return cardsColumn;
}

View *SettingsScene::buildVisualTab(const LayoutMetrics &metrics) {
  auto *cardsColumn = makeCardsColumn(metrics);
  auto *bgaControls = new View();
  bgaControls->setFlexDirection(FlexDirection::Column);
  bgaControls->setGap(metrics.compact ? 12.0f : 16.0f);
  bgaControls->setAlignItems(YGAlignFlexStart);
  bgaControls->addView(makeWrappedText(
      metrics.compact ? "Toggle BGA rendering for previews and gameplay."
                      : "Tap to switch BGA rendering on or off for future "
                        "previews and charts.",
      metrics.bodyTextSize, Color(150, 171, 193)));
  bgaModeText = makeText("", metrics.bodyTextSize + 6, Color(245, 248, 252),
                         TextView::CENTER, TextView::MIDDLE);
  bgaModeButton =
      makeButton(metrics.actionButtonWidth, metrics.actionButtonHeight,
                 bgaModeText, Color(33, 56, 87, 255), Color(43, 72, 110, 255),
                 Color(59, 98, 147, 255), Color(92, 131, 177, 255),
                 Color(118, 163, 217, 255), Color(139, 189, 244, 255));
  bgaModeButton->setOnClickListener([this]() {
    context.settings.bgaEnabled = !context.settings.bgaEnabled;
    persistSettings();
  });
  bgaControls->addView(bgaModeButton);
  cardsColumn->addView(makeCard(
      metrics, "BGA Playback",
      metrics.compact
          ? "Disable background animation for lower distraction or lighter "
            "rendering."
          : "Disable background animation if you want lower distraction or a "
            "lighter render path on slower hardware.",
      bgaControls, metrics.modeCardHeight, metrics.cardsWidth));

  auto *invisibleNoteControls = new View();
  invisibleNoteControls->setFlexDirection(FlexDirection::Column);
  invisibleNoteControls->setGap(metrics.compact ? 12.0f : 16.0f);
  invisibleNoteControls->setAlignItems(YGAlignFlexStart);
  invisibleNoteControls->addView(makeWrappedText(
      metrics.compact ? "Draw hidden notes as temporary lane markers."
                      : "Draw invisible chart notes as temporary lane "
                        "markers. Judgement and scoring stay unchanged.",
      metrics.bodyTextSize, Color(150, 171, 193)));
  showInvisibleNotesModeText =
      makeText("", metrics.bodyTextSize + 6, Color(245, 248, 252),
               TextView::CENTER, TextView::MIDDLE);
  showInvisibleNotesModeButton =
      makeButton(metrics.actionButtonWidth, metrics.actionButtonHeight,
                 showInvisibleNotesModeText, Color(33, 56, 87, 255),
                 Color(43, 72, 110, 255), Color(59, 98, 147, 255),
                 Color(92, 131, 177, 255), Color(118, 163, 217, 255),
                 Color(139, 189, 244, 255));
  showInvisibleNotesModeButton->setOnClickListener([this]() {
    context.settings.showInvisibleNotes = !context.settings.showInvisibleNotes;
    persistSettings();
    if (previewRenderer != nullptr) {
      previewRenderer->setShowInvisibleNotes(
          context.settings.showInvisibleNotes);
    }
  });
  invisibleNoteControls->addView(showInvisibleNotesModeButton);
  cardsColumn->addView(makeCard(
      metrics, "Show Invisible Notes",
      metrics.compact
          ? "Orange rectangles are placeholders until skin art exists."
          : "Invisible notes use orange placeholder rectangles until the "
            "skin system exposes dedicated artwork.",
      invisibleNoteControls, metrics.modeCardHeight, metrics.cardsWidth));

  auto *bgaDisplayControls = new View();
  bgaDisplayControls->setFlexDirection(FlexDirection::Column);
  bgaDisplayControls->setGap(metrics.compact ? 12.0f : 16.0f);
  bgaDisplayControls->setAlignItems(YGAlignFlexStart);
  bgaDisplayControls->addView(makeWrappedText(
      metrics.compact
          ? "Fit preserves the full image. Fill crops. Stretch ignores "
            "aspect."
          : "Fit preserves the whole BGA with letterboxing. Fill preserves "
            "aspect and crops edges. Stretch fills the screen without "
            "preserving aspect.",
      metrics.bodyTextSize, Color(150, 171, 193)));
  bgaDisplayModeText =
      makeText("", metrics.bodyTextSize + 6, Color(245, 248, 252),
               TextView::CENTER, TextView::MIDDLE);
  bgaDisplayModeButton = makeButton(
      metrics.actionButtonWidth, metrics.actionButtonHeight, bgaDisplayModeText,
      Color(33, 56, 87, 255), Color(43, 72, 110, 255), Color(59, 98, 147, 255),
      Color(92, 131, 177, 255), Color(118, 163, 217, 255),
      Color(139, 189, 244, 255));
  bgaDisplayModeButton->setOnClickListener([this]() {
    context.settings.bgaDisplayMode =
        nextBgaDisplayMode(context.settings.bgaDisplayMode);
    persistSettings();
  });
  bgaDisplayControls->addView(bgaDisplayModeButton);
  cardsColumn->addView(makeCard(
      metrics, "BGA Aspect",
      metrics.compact ? "Choose how BGA fits the playfield."
                      : "Choose how BGA media is fitted to the playfield.",
      bgaDisplayControls, metrics.modeCardHeight, metrics.cardsWidth));

  auto *brightnessControls = new View();
  brightnessControls->setFlexDirection(FlexDirection::Row);
  brightnessControls->setFlexWrap(YGWrapWrap);
  brightnessControls->setGap(metrics.compact ? 8.0f : 12.0f);
  brightnessControls->setAlignItems(YGAlignFlexStart);
  auto updateBgaBrightness = [this](int delta) {
    context.settings.bgaBrightnessPercent =
        clampBgaBrightness(context.settings.bgaBrightnessPercent + delta);
    persistSettings();
    syncBgaBrightnessInputText(true);
  };
  auto *minusBrightnessTen =
      makeStepButton(metrics, metrics.offsetButtonWidthLarge, "-10");
  minusBrightnessTen->setOnClickListener(
      [updateBgaBrightness]() { updateBgaBrightness(-10); });
  brightnessControls->addView(minusBrightnessTen);
  auto *minusBrightnessOne =
      makeStepButton(metrics, metrics.offsetButtonWidthSmall, "-1");
  minusBrightnessOne->setOnClickListener(
      [updateBgaBrightness]() { updateBgaBrightness(-1); });
  brightnessControls->addView(minusBrightnessOne);
  bgaBrightnessInput = makeNumericInput(metrics);
  bgaBrightnessInput->onEditingFinished(
      [this](const std::string &) { commitBgaBrightnessInput(); });
  brightnessControls->addView(makeInputFrame(metrics, bgaBrightnessInput));
  auto *plusBrightnessOne =
      makeStepButton(metrics, metrics.offsetButtonWidthSmall, "+1");
  plusBrightnessOne->setOnClickListener(
      [updateBgaBrightness]() { updateBgaBrightness(1); });
  brightnessControls->addView(plusBrightnessOne);
  auto *plusBrightnessTen =
      makeStepButton(metrics, metrics.offsetButtonWidthLarge, "+10");
  plusBrightnessTen->setOnClickListener(
      [updateBgaBrightness]() { updateBgaBrightness(10); });
  brightnessControls->addView(plusBrightnessTen);
  auto *resetBrightness = makeResetButton(metrics);
  resetBrightness->setOnClickListener([this]() {
    context.settings.bgaBrightnessPercent =
        AppSettings::kDefaultBgaBrightnessPercent;
    persistSettings();
    syncBgaBrightnessInputText(true);
  });
  brightnessControls->addView(resetBrightness);
  cardsColumn->addView(makeCard(
      metrics, "BGA Brightness",
      metrics.compact ? "Dim or restore the blurred BGA behind the lane."
                      : "Dim the BGA composite behind the lane when the "
                        "background competes with notes.",
      brightnessControls, metrics.offsetCardHeight, metrics.cardsWidth));

  auto *blurControls = new View();
  blurControls->setFlexDirection(FlexDirection::Row);
  blurControls->setFlexWrap(YGWrapWrap);
  blurControls->setGap(metrics.compact ? 8.0f : 12.0f);
  blurControls->setAlignItems(YGAlignFlexStart);
  auto updateBgaBlur = [this](float delta) {
    context.settings.bgaBlurStrength =
        clampBgaBlur(context.settings.bgaBlurStrength + delta);
    persistSettings();
    syncBgaBlurInputText(true);
  };
  auto *minusBlurLarge =
      makeStepButton(metrics, metrics.offsetButtonWidthLarge, "-1");
  minusBlurLarge->setOnClickListener(
      [updateBgaBlur]() { updateBgaBlur(-1.0f); });
  blurControls->addView(minusBlurLarge);
  auto *minusBlurSmall =
      makeStepButton(metrics, metrics.offsetButtonWidthSmall, "-0.5");
  minusBlurSmall->setOnClickListener(
      [updateBgaBlur]() { updateBgaBlur(-0.5f); });
  blurControls->addView(minusBlurSmall);
  bgaBlurInput = makeNumericInput(metrics);
  bgaBlurInput->onEditingFinished(
      [this](const std::string &) { commitBgaBlurInput(); });
  blurControls->addView(makeInputFrame(metrics, bgaBlurInput));
  auto *plusBlurSmall =
      makeStepButton(metrics, metrics.offsetButtonWidthSmall, "+0.5");
  plusBlurSmall->setOnClickListener([updateBgaBlur]() { updateBgaBlur(0.5f); });
  blurControls->addView(plusBlurSmall);
  auto *plusBlurLarge =
      makeStepButton(metrics, metrics.offsetButtonWidthLarge, "+1");
  plusBlurLarge->setOnClickListener([updateBgaBlur]() { updateBgaBlur(1.0f); });
  blurControls->addView(plusBlurLarge);
  auto *resetBlur = makeResetButton(metrics);
  resetBlur->setOnClickListener([this]() {
    context.settings.bgaBlurStrength = AppSettings::kDefaultBgaBlurStrength;
    persistSettings();
    syncBgaBlurInputText(true);
  });
  blurControls->addView(resetBlur);
  cardsColumn->addView(makeCard(
      metrics, "BGA Blur Strength",
      metrics.compact ? "Higher values soften background motion."
                      : "Higher values soften background motion before it is "
                        "composited behind the lane.",
      blurControls, metrics.offsetCardHeight, metrics.cardsWidth));
  return cardsColumn;
}

View *SettingsScene::buildLaneTab(const LayoutMetrics &metrics) {
  auto *cardsColumn = makeCardsColumn(metrics);
  auto *visibleTimeControls = buildVisibleTimeControls(metrics, true, false);
  cardsColumn->addView(makeCard(
      metrics, "Visible Time",
      metrics.compact
          ? "Controls how long notes stay on screen before the judgement "
            "line."
          : "Controls how long notes stay visible before reaching the "
            "judgement line. Switch units if you prefer legacy green number "
            "or direct milliseconds, and choose which BPM anchors that time.",
      visibleTimeControls, metrics.visibleTimeCardHeight, metrics.cardsWidth));

  auto *noteStartControls = new View();
  noteStartControls->setFlexDirection(FlexDirection::Row);
  noteStartControls->setFlexWrap(YGWrapWrap);
  noteStartControls->setGap(metrics.compact ? 8.0f : 12.0f);
  noteStartControls->setAlignItems(YGAlignFlexStart);
  auto updateNoteStartPosition = [this](int deltaPercent) {
    context.settings.noteStartPositionPercent = clampNoteStartPositionPercent(
        context.settings.noteStartPositionPercent + deltaPercent);
    persistSettings();
    syncNoteStartPositionInputText(true);
  };
  auto *minusNoteStartLarge =
      makeStepButton(metrics, metrics.offsetButtonWidthLarge, "-10%");
  minusNoteStartLarge->setOnClickListener(
      [updateNoteStartPosition]() { updateNoteStartPosition(-10); });
  noteStartControls->addView(minusNoteStartLarge);
  auto *minusNoteStartSmall =
      makeStepButton(metrics, metrics.offsetButtonWidthSmall, "-1%");
  minusNoteStartSmall->setOnClickListener(
      [updateNoteStartPosition]() { updateNoteStartPosition(-1); });
  noteStartControls->addView(minusNoteStartSmall);
  noteStartPositionInput = makeNumericInput(metrics);
  noteStartPositionInput->onEditingFinished(
      [this](const std::string &) { commitNoteStartPositionInput(); });
  noteStartControls->addView(makeInputFrame(metrics, noteStartPositionInput));
  auto *plusNoteStartSmall =
      makeStepButton(metrics, metrics.offsetButtonWidthSmall, "+1%");
  plusNoteStartSmall->setOnClickListener(
      [updateNoteStartPosition]() { updateNoteStartPosition(1); });
  noteStartControls->addView(plusNoteStartSmall);
  auto *plusNoteStartLarge =
      makeStepButton(metrics, metrics.offsetButtonWidthLarge, "+10%");
  plusNoteStartLarge->setOnClickListener(
      [updateNoteStartPosition]() { updateNoteStartPosition(10); });
  noteStartControls->addView(plusNoteStartLarge);
  auto *resetNoteStart = makeResetButton(metrics);
  resetNoteStart->setOnClickListener([this]() {
    context.settings.noteStartPositionPercent =
        AppSettings::kDefaultNoteStartPositionPercent;
    persistSettings();
    syncNoteStartPositionInputText(true);
  });
  noteStartControls->addView(resetNoteStart);
  cardsColumn->addView(makeCard(
      metrics, "Note Start Position",
      metrics.compact
          ? "Higher values make notes appear lower while preserving visible "
            "time."
          : "Move the note appearance point downward like a HIDDEN start "
            "position. The renderer scales scroll distance so the current "
            "green number still describes the time from appearance to the "
            "judgement line.",
      noteStartControls, metrics.offsetCardHeight, metrics.cardsWidth));

  auto *angleControls = new View();
  angleControls->setFlexDirection(FlexDirection::Row);
  angleControls->setFlexWrap(YGWrapWrap);
  angleControls->setGap(metrics.compact ? 8.0f : 12.0f);
  angleControls->setAlignItems(YGAlignFlexStart);
  auto updateLaneAngle = [this](float delta) {
    context.settings.laneAngleDegrees =
        clampLaneAngle(context.settings.laneAngleDegrees + delta);
    persistSettings();
    syncLaneAngleInputText(true);
  };
  auto *minusAngleLarge =
      makeStepButton(metrics, metrics.offsetButtonWidthLarge, "-5");
  minusAngleLarge->setOnClickListener(
      [updateLaneAngle]() { updateLaneAngle(-5.0f); });
  angleControls->addView(minusAngleLarge);
  auto *minusAngleSmall =
      makeStepButton(metrics, metrics.offsetButtonWidthSmall, "-1");
  minusAngleSmall->setOnClickListener(
      [updateLaneAngle]() { updateLaneAngle(-1.0f); });
  angleControls->addView(minusAngleSmall);
  laneAngleInput = makeNumericInput(metrics);
  laneAngleInput->onEditingFinished(
      [this](const std::string &) { commitLaneAngleInput(); });
  angleControls->addView(makeInputFrame(metrics, laneAngleInput));
  auto *plusAngleSmall =
      makeStepButton(metrics, metrics.offsetButtonWidthSmall, "+1");
  plusAngleSmall->setOnClickListener(
      [updateLaneAngle]() { updateLaneAngle(1.0f); });
  angleControls->addView(plusAngleSmall);
  auto *plusAngleLarge =
      makeStepButton(metrics, metrics.offsetButtonWidthLarge, "+5");
  plusAngleLarge->setOnClickListener(
      [updateLaneAngle]() { updateLaneAngle(5.0f); });
  angleControls->addView(plusAngleLarge);
  auto *resetAngle = makeResetButton(metrics);
  resetAngle->setOnClickListener([this]() {
    context.settings.laneAngleDegrees = AppSettings::kDefaultLaneAngleDegrees;
    persistSettings();
    syncLaneAngleInputText(true);
  });
  angleControls->addView(resetAngle);
  cardsColumn->addView(makeCard(
      metrics, "Lane Angle",
      metrics.compact ? "Adjust visual lane tilt and touch mapping together."
                      : "Adjust the gameplay camera pitch. Touch lane "
                        "conversion uses the same lane plane, so this stays "
                        "aligned for touch play.",
      angleControls, metrics.offsetCardHeight, metrics.cardsWidth));

  auto *lengthControls = new View();
  lengthControls->setFlexDirection(FlexDirection::Row);
  lengthControls->setFlexWrap(YGWrapWrap);
  lengthControls->setGap(metrics.compact ? 8.0f : 12.0f);
  lengthControls->setAlignItems(YGAlignFlexStart);
  auto updateLaneLength = [this](float delta) {
    context.settings.laneLength =
        clampLaneLength(context.settings.laneLength + delta);
    persistSettings();
    syncLaneLengthInputText(true);
  };
  auto *minusLengthLarge =
      makeStepButton(metrics, metrics.offsetButtonWidthLarge, "-1");
  minusLengthLarge->setOnClickListener(
      [updateLaneLength]() { updateLaneLength(-1.0f); });
  lengthControls->addView(minusLengthLarge);
  auto *minusLengthSmall =
      makeStepButton(metrics, metrics.offsetButtonWidthSmall, "-0.5");
  minusLengthSmall->setOnClickListener(
      [updateLaneLength]() { updateLaneLength(-0.5f); });
  lengthControls->addView(minusLengthSmall);
  laneLengthInput = makeNumericInput(metrics);
  laneLengthInput->onEditingFinished(
      [this](const std::string &) { commitLaneLengthInput(); });
  lengthControls->addView(makeInputFrame(metrics, laneLengthInput));
  auto *plusLengthSmall =
      makeStepButton(metrics, metrics.offsetButtonWidthSmall, "+0.5");
  plusLengthSmall->setOnClickListener(
      [updateLaneLength]() { updateLaneLength(0.5f); });
  lengthControls->addView(plusLengthSmall);
  auto *plusLengthLarge =
      makeStepButton(metrics, metrics.offsetButtonWidthLarge, "+1");
  plusLengthLarge->setOnClickListener(
      [updateLaneLength]() { updateLaneLength(1.0f); });
  lengthControls->addView(plusLengthLarge);
  auto *resetLength = makeResetButton(metrics);
  resetLength->setOnClickListener([this]() {
    context.settings.laneLength = AppSettings::kDefaultLaneLength;
    persistSettings();
    syncLaneLengthInputText(true);
  });
  lengthControls->addView(resetLength);
  cardsColumn->addView(makeCard(
      metrics, "Lane Length",
      metrics.compact ? "Adjust how far the visible lane reaches."
                      : "Adjust how far the visible lane reaches toward the "
                        "top of the screen.",
      lengthControls, metrics.offsetCardHeight, metrics.cardsWidth));

  auto *playAreaWidthControls = new View();
  playAreaWidthControls->setFlexDirection(FlexDirection::Column);
  playAreaWidthControls->setGap(metrics.compact ? 10.0f : 12.0f);
  playAreaWidthControls->setAlignItems(YGAlignFlexStart);

  auto makePlayAreaWidthRow = [this, &metrics](int keyMode) {
    auto *row = new View();
    row->setFlexDirection(FlexDirection::Row);
    row->setFlexWrap(YGWrapWrap);
    row->setGap(metrics.compact ? 8.0f : 10.0f);
    row->setAlignItems(YGAlignCenter);

    auto *label =
        makeText(std::to_string(keyMode) + "K", metrics.bodyTextSize + 4,
                 Color(244, 248, 255), TextView::CENTER, TextView::MIDDLE);
    label->setWidth(metrics.compact ? 54.0f : 64.0f);
    label->setHeight(static_cast<float>(metrics.actionButtonHeight));
    row->addView(label);

    auto *input = makeNumericInput(metrics);
    auto syncInput = [this, keyMode, input]() {
      input->setEditingText(formatPlayAreaWidthLabel(
          context.settings.playAreaWidthForKeyMode(keyMode)));
    };
    auto applyWidth = [this, keyMode, input](float width) {
      context.settings.setPlayAreaWidthForKeyMode(keyMode,
                                                  clampPlayAreaWidth(width));
      persistSettings();
      input->setEditingText(formatPlayAreaWidthLabel(
          context.settings.playAreaWidthForKeyMode(keyMode)));
    };

    auto *minusWidth =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "-0.5");
    minusWidth->setOnClickListener([this, keyMode, applyWidth]() {
      applyWidth(context.settings.playAreaWidthForKeyMode(keyMode) - 0.5f);
    });
    row->addView(minusWidth);

    input->onEditingFinished([this, keyMode, input,
                              applyWidth](const std::string &) {
      const std::string rawText = input->getText();
      if (rawText.empty()) {
        input->setEditingText(formatPlayAreaWidthLabel(
            context.settings.playAreaWidthForKeyMode(keyMode)));
        return;
      }
      try {
        applyWidth(std::stof(rawText));
      } catch (const std::exception &) {
        input->setEditingText(formatPlayAreaWidthLabel(
            context.settings.playAreaWidthForKeyMode(keyMode)));
      }
    });
    row->addView(makeInputFrame(metrics, input));

    auto *plusWidth =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "+0.5");
    plusWidth->setOnClickListener([this, keyMode, applyWidth]() {
      applyWidth(context.settings.playAreaWidthForKeyMode(keyMode) + 0.5f);
    });
    row->addView(plusWidth);

    auto *resetWidth = makeResetButton(metrics);
    resetWidth->setOnClickListener([applyWidth]() {
      applyWidth(AppSettings::kDefaultPlayAreaWidth);
    });
    row->addView(resetWidth);

    syncInput();
    return row;
  };

  for (int keyMode : {4, 5, 6, 7, 8, 10, 14}) {
    playAreaWidthControls->addView(makePlayAreaWidthRow(keyMode));
  }
  cardsColumn->addView(makeCard(
      metrics, "Play Area Width",
      metrics.compact
          ? "Set lane width separately for each key mode."
          : "Set the centered lane area width separately for each key mode. "
            "Touch input uses the same width as the rendered lanes.",
      playAreaWidthControls, metrics.visibleTimeCardHeight,
      metrics.cardsWidth));

  auto *beamControls = new View();
  beamControls->setFlexDirection(FlexDirection::Row);
  beamControls->setFlexWrap(YGWrapWrap);
  beamControls->setGap(metrics.compact ? 8.0f : 12.0f);
  beamControls->setAlignItems(YGAlignFlexStart);
  auto updateLaneBeamLength = [this](int deltaPercent) {
    context.settings.laneBeamLengthPercent = clampLaneBeamLengthPercent(
        context.settings.laneBeamLengthPercent + deltaPercent);
    persistSettings();
    syncLaneBeamLengthInputText(true);
  };
  auto *minusBeamLarge =
      makeStepButton(metrics, metrics.offsetButtonWidthLarge, "-10%");
  minusBeamLarge->setOnClickListener(
      [updateLaneBeamLength]() { updateLaneBeamLength(-10); });
  beamControls->addView(minusBeamLarge);
  auto *minusBeamSmall =
      makeStepButton(metrics, metrics.offsetButtonWidthSmall, "-1%");
  minusBeamSmall->setOnClickListener(
      [updateLaneBeamLength]() { updateLaneBeamLength(-1); });
  beamControls->addView(minusBeamSmall);
  laneBeamLengthInput = makeNumericInput(metrics);
  laneBeamLengthInput->onEditingFinished(
      [this](const std::string &) { commitLaneBeamLengthInput(); });
  beamControls->addView(makeInputFrame(metrics, laneBeamLengthInput));
  auto *plusBeamSmall =
      makeStepButton(metrics, metrics.offsetButtonWidthSmall, "+1%");
  plusBeamSmall->setOnClickListener(
      [updateLaneBeamLength]() { updateLaneBeamLength(1); });
  beamControls->addView(plusBeamSmall);
  auto *plusBeamLarge =
      makeStepButton(metrics, metrics.offsetButtonWidthLarge, "+10%");
  plusBeamLarge->setOnClickListener(
      [updateLaneBeamLength]() { updateLaneBeamLength(10); });
  beamControls->addView(plusBeamLarge);
  auto *resetBeam = makeResetButton(metrics);
  resetBeam->setOnClickListener([this]() {
    context.settings.laneBeamLengthPercent =
        AppSettings::kDefaultLaneBeamLengthPercent;
    persistSettings();
    syncLaneBeamLengthInputText(true);
  });
  beamControls->addView(resetBeam);
  cardsColumn->addView(makeCard(
      metrics, "Lane Beam Length",
      metrics.compact
          ? "Scale press beams from the judgement line upward."
          : "Scale the press feedback beam height from the judgement line "
            "toward the top of the lane. 100% keeps the original full lane "
            "beam.",
      beamControls, metrics.offsetCardHeight, metrics.cardsWidth));

  auto *previewControls = new View();
  previewControls->setFlexDirection(FlexDirection::Column);
  previewControls->setGap(metrics.compact ? 12.0f : 16.0f);
  previewControls->setAlignItems(YGAlignFlexStart);
  previewControls->addView(makeWrappedText(
      metrics.compact
          ? "Open a live gameplay preview with falling notes."
          : "Open a live gameplay preview with falling notes. It uses the "
            "same lane renderer, camera, viewport, and note textures as "
            "gameplay.",
      metrics.bodyTextSize, Color(150, 171, 193)));
  auto *previewButton = makeButton(
      metrics.actionButtonWidth, metrics.actionButtonHeight,
      makeText("Preview", metrics.bodyTextSize + 4, Color(239, 244, 251),
               TextView::CENTER, TextView::MIDDLE),
      Color(35, 68, 62, 255), Color(45, 88, 80, 255), Color(63, 118, 107, 255),
      Color(97, 157, 142, 255), Color(120, 187, 169, 255),
      Color(145, 214, 195, 255));
  previewButton->setOnClickListener([this]() { startLanePreview(); });
  previewControls->addView(previewButton);
  cardsColumn->addView(makeCard(
      metrics, "Gameplay Preview",
      metrics.compact ? "Test lane setup in the gameplay renderer."
                      : "Test lane setup in the gameplay renderer before "
                        "entering a chart.",
      previewControls, metrics.modeCardHeight, metrics.cardsWidth));
  return cardsColumn;
}

View *SettingsScene::buildTablesTab(const LayoutMetrics &metrics) {
  auto *cardsColumn = makeCardsColumn(metrics);
  loadDifficultyTables();
  loadChartEntries();

  auto *addControls = new View();
  addControls->setFlexDirection(FlexDirection::Column);
  addControls->setGap(metrics.compact ? 12.0f : 16.0f);
  addControls->setAlignItems(YGAlignFlexStart);

  const int addRowGap = metrics.compact ? 8 : 12;
  const int addButtonWidth = metrics.compact ? 150 : 170;
  const int minInputWidth = 180;
  auto *urlRow = new View();
  urlRow->setFlexDirection(FlexDirection::Row);
  urlRow->setFlexWrap(YGWrapWrap);
  urlRow->setGap(static_cast<float>(addRowGap));
  urlRow->setAlignItems(YGAlignFlexStart);
  urlRow->setAlignSelf(YGAlignStretch);

  tableUrlInput = makeTextInput(metrics, minInputWidth);
  tableUrlInput->setMinWidth(static_cast<float>(minInputWidth));
  tableUrlInput->setFlexGrow(1.0f);
  tableUrlInput->setFlexShrink(1.0f);
  tableUrlInput->setEditingText(tableUrlText);
  tableUrlInput->onTextChanged(
      [this](const std::string &text) { tableUrlText = text; });
  tableUrlInput->onSubmit([this](const std::string &text) {
    tableUrlText = text;
    addDifficultyTableFromUrl();
  });
  urlRow->addView(tableUrlInput);

  auto *addButton = makeButton(
      addButtonWidth, metrics.actionButtonHeight,
      makeText("Add Table", metrics.bodyTextSize + 4, Color(239, 244, 251),
               TextView::CENTER, TextView::MIDDLE),
      Color(35, 68, 62, 255), Color(45, 88, 80, 255), Color(63, 118, 107, 255),
      Color(97, 157, 142, 255), Color(120, 187, 169, 255),
      Color(145, 214, 195, 255));
  addButton->setOnClickListener([this]() { addDifficultyTableFromUrl(); });
  urlRow->addView(addButton);
  addControls->addView(urlRow);

  difficultyTableStatusText = makeWrappedText(
      difficultyTableStatusMessage, metrics.bodyTextSize,
      Color(difficultyTableStatusColor.r, difficultyTableStatusColor.g,
            difficultyTableStatusColor.b, difficultyTableStatusColor.a));
  addControls->addView(difficultyTableStatusText);

  cardsColumn->addView(makeCard(
      metrics, "Add Difficulty Table",
      metrics.compact ? "Import a bmstable page, header, or table list URL."
                      : "Import a bmstable page URL or a direct header JSON "
                        "URL. Table-list JSON URLs import each listed table.",
      addControls, metrics.modeCardHeight, metrics.cardsWidth));

  auto *folderList = new View();
  folderList->setFlexDirection(FlexDirection::Column);
  folderList->setGap(metrics.compact ? 10.0f : 12.0f);

  auto *refreshFoldersButton = makeButton(
      metrics.compact ? 160 : 180, metrics.actionButtonHeight,
      makeText("Refresh List", metrics.bodyTextSize + 2, Color(239, 244, 251),
               TextView::CENTER, TextView::MIDDLE),
      Color(35, 68, 62, 255), Color(45, 88, 80, 255), Color(63, 118, 107, 255),
      Color(97, 157, 142, 255), Color(120, 187, 169, 255),
      Color(145, 214, 195, 255));
  refreshFoldersButton->setOnClickListener([this]() { refreshChartLibrary(); });
  folderList->addView(refreshFoldersButton);

  if (chartEntries.empty()) {
    folderList->addView(makeWrappedText("No chart folders are installed.",
                                        metrics.bodyTextSize,
                                        Color(165, 185, 205)));
  } else {
    for (const auto &entry : chartEntries) {
      const std::string entryPathText = formatChartEntryPath(entry);

      auto *row = new View();
      row->setFlexDirection(FlexDirection::Column);
      row->setGap(metrics.compact ? 8.0f : 10.0f);
      row->setPadding(Edge::All, static_cast<float>(metrics.compact ? 14 : 16));
      row->setBackgroundColor(Color(12, 21, 34, 230));
      row->setBorderColor(Color(63, 86, 113, 255));
      row->setBorderWidth(2);

      row->addView(makeWrappedText(formatChartEntryName(entry),
                                   metrics.bodyTextSize + 6,
                                   Color(244, 248, 255)));
      row->addView(makeWrappedText(formatChartEntrySource(entry),
                                   metrics.smallTextSize,
                                   Color(142, 164, 189)));

      auto *actions = new View();
      actions->setFlexDirection(FlexDirection::Row);
      actions->setFlexWrap(YGWrapWrap);
      actions->setGap(metrics.compact ? 8.0f : 10.0f);

      const int folderActionWidth = metrics.compact ? 136 : 156;
      const bool confirmingDelete =
          pendingDeleteChartEntryPath == entryPathText;
      auto *deleteButton = makeButton(
          folderActionWidth, metrics.actionButtonHeight,
          makeText(confirmingDelete ? "Confirm" : "Delete",
                   metrics.bodyTextSize + 2, Color(248, 241, 236),
                   TextView::CENTER, TextView::MIDDLE),
          confirmingDelete ? Color(130, 58, 45, 255) : Color(96, 57, 44, 255),
          confirmingDelete ? Color(153, 75, 58, 255) : Color(117, 72, 55, 255),
          confirmingDelete ? Color(184, 96, 74, 255) : Color(153, 96, 74, 255),
          Color(165, 105, 79, 255), Color(193, 124, 93, 255),
          Color(219, 145, 108, 255));
      deleteButton->setOnClickListener(
          [this, entryPathText]() { deleteChartEntry(entryPathText); });
      actions->addView(deleteButton);

      row->addView(actions);
      folderList->addView(row);
    }
  }

  cardsColumn->addView(makeCard(
      metrics, "Chart Folders",
      metrics.compact ? "Remove folders from library scanning."
                      : "Remove a folder entry and cached charts under it "
                        "from the library database.",
      folderList, metrics.modeCardHeight, metrics.cardsWidth));

  auto *tableList = new View();
  tableList->setFlexDirection(FlexDirection::Column);
  tableList->setGap(metrics.compact ? 10.0f : 12.0f);

  if (difficultyTables.empty()) {
    tableList->addView(makeWrappedText("No difficulty tables are installed.",
                                       metrics.bodyTextSize,
                                       Color(165, 185, 205)));
  } else {
    for (const auto &table : difficultyTables) {
      auto *row = new View();
      row->setFlexDirection(FlexDirection::Column);
      row->setGap(metrics.compact ? 8.0f : 10.0f);
      row->setPadding(Edge::All, static_cast<float>(metrics.compact ? 14 : 16));
      row->setBackgroundColor(Color(12, 21, 34, 230));
      row->setBorderColor(Color(63, 86, 113, 255));
      row->setBorderWidth(2);

      auto *titleRow = new View();
      titleRow->setFlexDirection(FlexDirection::Row);
      titleRow->setFlexWrap(YGWrapWrap);
      titleRow->setGap(metrics.compact ? 8.0f : 12.0f);
      titleRow->setAlignItems(YGAlignCenter);
      titleRow->addView(makeWrappedText(table.name, metrics.bodyTextSize + 6,
                                        Color(244, 248, 255)));
      titleRow->addView(
          makeText(table.symbol, metrics.bodyTextSize, Color(181, 207, 236)));
      titleRow->addView(makeText(formatTableCount(table.chartCount),
                                 metrics.bodyTextSize, Color(165, 185, 205)));
      row->addView(titleRow);

      row->addView(makeWrappedText(formatTableSource(table.sourceUrl),
                                   metrics.smallTextSize,
                                   Color(142, 164, 189)));

      auto *actions = new View();
      actions->setFlexDirection(FlexDirection::Row);
      actions->setFlexWrap(YGWrapWrap);
      actions->setGap(metrics.compact ? 8.0f : 10.0f);

      const int smallActionWidth = metrics.compact ? 136 : 156;
      auto *updateButton = makeButton(
          smallActionWidth, metrics.actionButtonHeight,
          makeText("Update", metrics.bodyTextSize + 2, Color(239, 244, 251),
                   TextView::CENTER, TextView::MIDDLE),
          Color(33, 56, 87, 255), Color(43, 72, 110, 255),
          Color(59, 98, 147, 255), Color(92, 131, 177, 255),
          Color(118, 163, 217, 255), Color(139, 189, 244, 255));
      updateButton->setOnClickListener([this, tableId = table.id]() {
        updateDifficultyTableFromSource(tableId);
      });
      actions->addView(updateButton);

      const bool confirmingDelete = pendingDeleteDifficultyTableId == table.id;
      auto *deleteButton = makeButton(
          smallActionWidth, metrics.actionButtonHeight,
          makeText(confirmingDelete ? "Confirm" : "Delete",
                   metrics.bodyTextSize + 2, Color(248, 241, 236),
                   TextView::CENTER, TextView::MIDDLE),
          confirmingDelete ? Color(130, 58, 45, 255) : Color(96, 57, 44, 255),
          confirmingDelete ? Color(153, 75, 58, 255) : Color(117, 72, 55, 255),
          confirmingDelete ? Color(184, 96, 74, 255) : Color(153, 96, 74, 255),
          Color(165, 105, 79, 255), Color(193, 124, 93, 255),
          Color(219, 145, 108, 255));
      deleteButton->setOnClickListener(
          [this, tableId = table.id]() { deleteDifficultyTable(tableId); });
      actions->addView(deleteButton);

      row->addView(actions);
      tableList->addView(row);
    }
  }

  cardsColumn->addView(makeCard(
      metrics, "Installed Tables",
      metrics.compact ? "Update from source URL or remove a table."
                      : "Update a table from its stored source URL or remove "
                        "it from the chart database.",
      tableList, metrics.modeCardHeight, metrics.cardsWidth));
  return cardsColumn;
}

void SettingsScene::buildDifficultyTableImportModal(
    const LayoutMetrics &metrics) {
  difficultyTableImportModalRoot = new BlockingOverlayView(
      0, 0, rendering::window_width, rendering::window_height);
  difficultyTableImportModalRoot->setPositionType(YGPositionTypeAbsolute);
  difficultyTableImportModalRoot->setPosition(Edge::Left, 0);
  difficultyTableImportModalRoot->setPosition(Edge::Top, 0);
  difficultyTableImportModalRoot->setZIndex(1000);
  difficultyTableImportModalRoot->setVisible(false);
  difficultyTableImportModalRoot->setFlexDirection(FlexDirection::Column);
  difficultyTableImportModalRoot->setAlignItems(YGAlignCenter);
  difficultyTableImportModalRoot->setJustifyContent(YGJustifyCenter);
  difficultyTableImportModalRoot->setBackgroundColor(Color(0, 0, 0, 164));

  auto *importPanel = new View();
  importPanel
      ->setWidth(static_cast<float>(
          std::min(metrics.compact ? 620 : 760,
                   std::max(280, metrics.contentWidth - 32))))
      ->setMinHeight(static_cast<float>(metrics.compact ? 320 : 360))
      ->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setGap(metrics.compact ? 14.0f : 18.0f)
      ->setPadding(Edge::All, static_cast<float>(metrics.cardPadding))
      ->setBackgroundColor(Color(17, 27, 42, 248))
      ->setBorderColor(Color(88, 118, 154, 255))
      ->setBorderWidth(2);

  difficultyTableImportTitleText =
      makeWrappedText("Importing Difficulty Tables", metrics.sectionTitleSize,
                      Color(244, 248, 255));
  importPanel->addView(difficultyTableImportTitleText);

  difficultyTableImportStatusText = makeWrappedText(
      "Preparing import...", metrics.bodyTextSize, Color(181, 207, 236));
  importPanel->addView(difficultyTableImportStatusText);

  difficultyTableImportTableText =
      makeWrappedText("Current table: Resolving table URL",
                      metrics.bodyTextSize, Color(239, 244, 251));
  importPanel->addView(difficultyTableImportTableText);

  auto *progressRow = new View();
  progressRow->setFlexDirection(FlexDirection::Column);
  progressRow->setGap(metrics.compact ? 8.0f : 10.0f);
  difficultyTableImportProgressText =
      makeText("0 / 1 table", metrics.bodyTextSize, Color(165, 185, 205));
  progressRow->addView(difficultyTableImportProgressText);

  auto *progressTrack = new View();
  progressTrack->setHeight(static_cast<float>(metrics.compact ? 16 : 18));
  progressTrack->setAlignSelf(YGAlignStretch);
  progressTrack->setFlexDirection(FlexDirection::Row);
  progressTrack->setBackgroundColor(Color(8, 14, 24, 255));
  progressTrack->setBorderColor(Color(66, 91, 122, 255));
  progressTrack->setBorderWidth(2);
  difficultyTableImportProgressFill = new View();
  difficultyTableImportProgressFill->setWidthPercent(0.0f);
  difficultyTableImportProgressFill->setHeight(
      static_cast<float>(metrics.compact ? 16 : 18));
  difficultyTableImportProgressFill->setBackgroundColor(
      Color(97, 157, 142, 255));
  progressTrack->addView(difficultyTableImportProgressFill);
  progressRow->addView(progressTrack);
  importPanel->addView(progressRow);

  auto *modalActions = new View();
  modalActions->setFlexDirection(FlexDirection::Row);
  modalActions->setJustifyContent(YGJustifyFlexEnd);
  difficultyTableImportCloseButton = makeButton(
      160, 60,
      makeText("Close", metrics.bodyTextSize + 2, Color(239, 244, 251),
               TextView::CENTER, TextView::MIDDLE),
      Color(33, 56, 87, 255), Color(43, 72, 110, 255), Color(59, 98, 147, 255),
      Color(92, 131, 177, 255), Color(118, 163, 217, 255),
      Color(139, 189, 244, 255));
  difficultyTableImportCloseButton->setOnClickListener(
      [this]() { hideDifficultyTableImportModal(); });
  modalActions->addView(difficultyTableImportCloseButton);
  importPanel->addView(modalActions);

  difficultyTableImportModalRoot->addView(importPanel);
  rootLayout->addView(difficultyTableImportModalRoot);
}

void SettingsScene::initView() {
  LayoutMetrics metrics = resolveLayoutMetrics();
  View::LayoutBatchScope layoutBatch;

  rootLayout =
      new View(0, 0, rendering::window_width, rendering::window_height);
  rootLayout->setFlexDirection(FlexDirection::Column);
  rootLayout->setPadding(
      Edge::Top,
      static_cast<float>(metrics.safe.top + metrics.verticalPadding));
  rootLayout->setPadding(
      Edge::Left,
      static_cast<float>(metrics.safe.left + metrics.horizontalPadding));
  rootLayout->setPadding(
      Edge::Right,
      static_cast<float>(metrics.safe.right + metrics.horizontalPadding));
  rootLayout->setPadding(
      Edge::Bottom,
      static_cast<float>(metrics.safe.bottom + metrics.verticalPadding));
  rootLayout->setGap(static_cast<float>(metrics.rootGap));

  if (previewActive) {
    buildPreviewLayout(metrics);
    return;
  }

  rootLayout->setBackgroundColor(Color(10, 18, 30));

  if (!metrics.compact) {
    auto *accentA = new View(110, 86, 480, 180);
    accentA->setPositionType(YGPositionTypeAbsolute);
    accentA->setBackgroundColor(Color(39, 101, 160, 96));
    rootLayout->addView(accentA);

    auto *accentB = new View(rendering::window_width - 520,
                             rendering::window_height - 250, 420, 160);
    accentB->setPositionType(YGPositionTypeAbsolute);
    accentB->setBackgroundColor(Color(207, 110, 62, 72));
    rootLayout->addView(accentB);
  }

  auto *header = new View();
  header->setFlexDirection(FlexDirection::Row);
  header->setAlignItems(YGAlignCenter);
  header->setJustifyContent(YGJustifySpaceBetween);

  auto *headerText = new View();
  headerText->setFlexDirection(FlexDirection::Column);
  headerText->setGap(static_cast<float>(metrics.headerGap));
  headerText->addView(
      makeText("Settings", metrics.titleSize, Color(244, 248, 255)));
  headerText->addView(makeWrappedText(
      metrics.compact
          ? "Timing, keysound, and visual preferences."
          : "Persistent player preferences for timing, keysounds, and visual "
            "load.",
      metrics.subtitleSize, Color(162, 183, 205)));
  header->addView(headerText);

  auto *backLabel =
      makeText("Back", metrics.bodyTextSize + 6, Color(237, 243, 252),
               TextView::CENTER, TextView::MIDDLE);
  auto *backButton =
      makeButton(metrics.backButtonWidth, metrics.backButtonHeight, backLabel,
                 Color(22, 33, 49, 255), Color(31, 46, 67, 255),
                 Color(53, 78, 110, 255), Color(96, 121, 156, 255),
                 Color(120, 151, 190, 255), Color(148, 186, 231, 255));
  backButton->setOnClickListener(
      [this]() { context.sceneManager->changeScene("MainMenu"); });
  header->addView(backButton);
  rootLayout->addView(header);

  const int tabColumnWidth = std::min(
      metrics.contentWidth,
      metrics.compact ? std::clamp(metrics.contentWidth / 4, 150, 190)
                      : std::clamp(metrics.contentWidth / 6, 220, 280));
  metrics.cardsWidth =
      std::max(0, metrics.contentWidth - tabColumnWidth - metrics.bodyGap);
  metrics.useDualCardRow = !metrics.compact && metrics.cardsWidth >= 980;
  metrics.secondaryCardWidth =
      metrics.useDualCardRow
          ? std::max(0, (metrics.cardsWidth - metrics.secondaryGap) / 2)
          : metrics.cardsWidth;

  auto *content = new View();
  content->setFlexDirection(FlexDirection::Row);
  content->setGap(static_cast<float>(metrics.bodyGap));
  content->setFlex(1.0f);
  content->setAlignItems(YGAlignStretch);

  auto *tabControls = new View();
  tabControls->setFlexDirection(FlexDirection::Column);
  tabControls->setGap(metrics.compact ? 8.0f : 12.0f);
  tabControls->setWidth(static_cast<float>(tabColumnWidth));
  tabControls->setFlexShrink(0.0f);
  auto makeTabButton = [&](SettingsTab tab, const std::string &label) {
    auto *button = makeButton(
        tabColumnWidth, metrics.actionButtonHeight,
        makeText(label, metrics.bodyTextSize + 4, Color(239, 244, 251),
                 TextView::CENTER, TextView::MIDDLE),
        Color(28, 40, 58, 255), Color(36, 52, 75, 255), Color(61, 87, 118, 255),
        Color(84, 107, 139, 255), Color(108, 136, 174, 255),
        Color(139, 172, 217, 255));
    button->setOnClickListener([this, tab]() {
      if (activeTab == tab) {
        return;
      }
      activeTab = tab;
      lastLayoutWidth = -1;
    });
    return button;
  };
  timingTabButton = makeTabButton(SettingsTab::Timing, "Timing");
  visualTabButton = makeTabButton(SettingsTab::Visual, "Visual");
  laneTabButton = makeTabButton(SettingsTab::Lane, "Lane");
  tablesTabButton = makeTabButton(SettingsTab::Tables, "Tables");
  tabControls->addView(timingTabButton);
  tabControls->addView(visualTabButton);
  tabControls->addView(laneTabButton);
  tabControls->addView(tablesTabButton);
  content->addView(tabControls);

  scrollView = new ScrollView();
  scrollView->setFlex(1.0f);

  auto *scrollContent = new View();
  scrollContent->setFlexDirection(FlexDirection::Column);
  scrollContent->setGap(static_cast<float>(metrics.rootGap));

  View *cardsColumn = nullptr;
  switch (activeTab) {
  case SettingsTab::Timing:
    cardsColumn = buildTimingTab(metrics);
    break;
  case SettingsTab::Visual:
    cardsColumn = buildVisualTab(metrics);
    break;
  case SettingsTab::Lane:
    cardsColumn = buildLaneTab(metrics);
    break;
  case SettingsTab::Tables:
    cardsColumn = buildTablesTab(metrics);
    break;
  }
  if (cardsColumn != nullptr) {
    scrollContent->addView(cardsColumn);
  }

  auto *footer = new View();
  footer->setPadding(Edge::All, static_cast<float>(metrics.cardPadding - 4));
  footer->setBackgroundColor(Color(14, 22, 34, 220));
  footer->setBorderColor(Color(59, 80, 108, 255));
  footer->setBorderWidth(2);
  footer->addView(makeWrappedText(
      metrics.compact
          ? "Settings save automatically in the app documents directory."
          : "Settings are saved automatically in the app documents directory.",
      metrics.bodyTextSize, Color(165, 185, 205)));
  scrollContent->addView(footer);

  scrollView->setContentView(scrollContent);
  content->addView(scrollView);
  rootLayout->addView(content);

  buildDifficultyTableImportModal(metrics);

  addView(rootLayout);
  rootLayout->applyYogaLayout();
  refreshDifficultyTableImportModal();
  refreshSettingsText();
}
