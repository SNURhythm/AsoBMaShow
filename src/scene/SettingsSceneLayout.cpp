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

int resolvePreviewPanelWidth(const LayoutMetrics &metrics, int foldButtonSize,
                             bool folded) {
  if (folded) {
    return foldButtonSize;
  }
  if (metrics.compact) {
    return std::min(metrics.contentWidth, 520);
  }

  const int contentGap = metrics.compact ? 8 : 10;
  const int requiredForTwoActions =
      metrics.actionButtonWidth * 2 + contentGap + metrics.cardPadding * 2;
  const int availableWidth = std::max(0, metrics.contentWidth);
  const int maxPanelWidth = std::min(availableWidth, 760);
  if (maxPanelWidth <= requiredForTwoActions) {
    return maxPanelWidth;
  }
  return std::clamp(720, requiredForTwoActions, maxPanelWidth);
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
  summaryJudgementTextYValueText = nullptr;
  summaryJudgementIndicatorYValueText = nullptr;
  summaryJudgementIndicatorWidthValueText = nullptr;
  summaryJudgementCounterPositionValueText = nullptr;
  summaryJudgementTimingFastSlowValueText = nullptr;
  summaryJudgementTimingMillisecondsValueText = nullptr;
  summaryGaugeBarPositionValueText = nullptr;
  summaryNotePriorityValueText = nullptr;
  summaryUiThemeValueText = nullptr;
  judgementIndicatorYInput = nullptr;
  judgementIndicatorWidthInput = nullptr;
  visibleTimeModeText = nullptr;
  visibleTimeBpmStrategyText = nullptr;
  keysoundModeText = nullptr;
  showInvisibleNotesModeText = nullptr;
  archiveChartPreviewModeText = nullptr;
  notePriorityModeText = nullptr;
  judgementIndicatorModeText = nullptr;
  judgementIndicatorRenderModeText = nullptr;
  judgementTimingFastSlowCriteriaText = nullptr;
  judgementTimingMillisecondsCriteriaText = nullptr;
  judgementCounterModeText = nullptr;
  judgementCounterPositionText = nullptr;
  gaugeBarPositionText = nullptr;
  bgaModeText = nullptr;
  bgaDisplayModeText = nullptr;
  uiThemeModeText = nullptr;
  archiveCacheCleanupButtonText = nullptr;
  archiveCacheCleanupStatusText = nullptr;
  visibleTimeModeButton = nullptr;
  visibleTimeBpmStrategyButton = nullptr;
  keysoundModeButton = nullptr;
  showInvisibleNotesModeButton = nullptr;
  archiveChartPreviewModeButton = nullptr;
  notePriorityModeButton = nullptr;
  judgementIndicatorModeButton = nullptr;
  judgementIndicatorRenderModeButton = nullptr;
  judgementTimingFastSlowCriteriaButton = nullptr;
  judgementTimingMillisecondsCriteriaButton = nullptr;
  judgementCounterModeButton = nullptr;
  judgementCounterPositionButton = nullptr;
  gaugeBarPositionButton = nullptr;
  bgaModeButton = nullptr;
  bgaDisplayModeButton = nullptr;
  uiThemeModeButton = nullptr;
  archiveCacheCleanupButton = nullptr;
  timingTabButton = nullptr;
  visualTabButton = nullptr;
  laneTabButton = nullptr;
  miscTabButton = nullptr;
  difficultyTablesTabButton = nullptr;
  bmsLibraryTabButton = nullptr;
  timingTabText = nullptr;
  visualTabText = nullptr;
  laneTabText = nullptr;
  miscTabText = nullptr;
  difficultyTablesTabText = nullptr;
  bmsLibraryTabText = nullptr;
  bgaBrightnessInput = nullptr;
  bgaBlurInput = nullptr;
  laneAngleInput = nullptr;
  laneLengthInput = nullptr;
  laneBeamLengthInput = nullptr;
  noteStartPositionInput = nullptr;
  tableUrlInput = nullptr;
  difficultyTableStatusText = nullptr;
  chartFolderStatusText = nullptr;
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

  const bool preserveScroll =
      rootLayout != nullptr && scrollView != nullptr &&
      activeTab == lastLaidOutTab;
  const float preservedScrollOffset =
      preserveScroll ? scrollView->getScrollOffset() : 0.0f;

  resetViewState();
  lastLayoutWidth = rendering::window_width;
  lastLayoutHeight = rendering::window_height;
  lastSafeTop = safe.top;
  lastSafeLeft = safe.left;
  lastSafeBottom = safe.bottom;
  lastSafeRight = safe.right;
  initView();
  lastLaidOutTab = activeTab;
  if (preserveScroll && scrollView != nullptr) {
    scrollView->setScrollOffset(preservedScrollOffset);
  }
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
        metrics.bodyTextSize, ui_theme::textSecondary()));
  }

  visibleTimeModeText =
      makeText("", metrics.bodyTextSize + 6, ui_theme::textPrimary(),
               TextView::CENTER, TextView::MIDDLE);
  visibleTimeModeButton =
      makeControlButton(metrics.actionButtonWidth, metrics.actionButtonHeight,
                        visibleTimeModeText);
  visibleTimeModeButton->setOnClickListener([this]() {
    context.settings.visibleTimeUseMilliseconds =
        !context.settings.visibleTimeUseMilliseconds;
    persistSettings();
    syncVisibleTimeInputText(true);
  });
  View *visibleTimeModeRow = nullptr;
  if (compactAdjustments) {
    visibleTimeModeRow = new View();
    visibleTimeModeRow->setFlexDirection(FlexDirection::Row);
    visibleTimeModeRow->setFlexWrap(YGWrapWrap);
    visibleTimeModeRow->setGap(metrics.compact ? 8.0f : 10.0f);
    visibleTimeModeRow->setAlignItems(YGAlignCenter);
    visibleTimeModeRow->setWidthPercent(100.0f);
    visibleTimeModeRow->setJustifyContent(YGJustifyCenter);
    visibleTimeModeRow->addView(visibleTimeModeButton);
  } else {
    visibleTimeControls->addView(visibleTimeModeButton);
  }

  visibleTimeBpmStrategyText =
      makeText("", metrics.bodyTextSize + 6, ui_theme::textPrimary(),
               TextView::CENTER, TextView::MIDDLE);
  visibleTimeBpmStrategyButton =
      makeControlButton(metrics.actionButtonWidth, metrics.actionButtonHeight,
                        visibleTimeBpmStrategyText);
  visibleTimeBpmStrategyButton->setOnClickListener([this]() {
    context.settings.visibleTimeBpmStrategy =
        nextVisibleTimeBpmStrategy(context.settings.visibleTimeBpmStrategy);
    persistSettings();
    if (previewRenderer != nullptr) {
      previewRenderer->setVisibleTimeBpmStrategy(
          context.settings.visibleTimeBpmStrategy);
    }
  });
  if (visibleTimeModeRow != nullptr) {
    visibleTimeModeRow->addView(visibleTimeBpmStrategyButton);
    visibleTimeControls->addView(visibleTimeModeRow);
  } else {
    visibleTimeControls->addView(visibleTimeBpmStrategyButton);
  }

  auto *visibleTimeValueControls = new View();
  visibleTimeValueControls->setFlexDirection(FlexDirection::Row);
  visibleTimeValueControls->setFlexWrap(YGWrapWrap);
  visibleTimeValueControls->setGap(metrics.compact ? 8.0f : 12.0f);
  visibleTimeValueControls->setAlignItems(compactAdjustments ? YGAlignCenter
                                                             : YGAlignFlexStart);
  if (compactAdjustments) {
    visibleTimeValueControls->setWidthPercent(100.0f);
    visibleTimeValueControls->setJustifyContent(YGJustifyCenter);
  }

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
  constexpr int previewPanelPageCount = 3;
  if (previewPanelPage < 0 || previewPanelPage >= previewPanelPageCount) {
    previewPanelPage = 0;
  }
  const int panelWidth =
      resolvePreviewPanelWidth(metrics, foldButtonSize, previewPanelFolded);
  const int panelHeight = std::max(
      foldButtonSize,
      rendering::window_height - metrics.safe.top - metrics.safe.bottom -
          metrics.verticalPadding * 2);
  auto *previewPanel = new View();
  previewPanel->setWidth(static_cast<float>(panelWidth));
  previewPanel->setPadding(
      Edge::All,
      static_cast<float>(previewPanelFolded ? 0 : metrics.cardPadding));
  previewPanel->setGap(metrics.compact ? 12.0f : 16.0f);
  previewPanel->setFlexDirection(FlexDirection::Column);
  previewPanel->setAlignItems(previewPanelFolded ? YGAlignFlexEnd
                                                 : YGAlignStretch);
  previewPanel->setThemedBackgroundColor(ui_theme::panel);
  previewPanel->setCornerRadius(ui_theme::panelRadius());
  previewPanel->setThemedShadow(ui_theme::shadow, ui_theme::kPanelShadow);
  previewPanel->setThemedBorderColor(ui_theme::hairline);
  previewPanel->setBorderWidth(1);

  auto makeFoldButton = [this, foldButtonSize](const std::string &label) {
    auto *button =
        makeControlButton(foldButtonSize, foldButtonSize,
                          makeText(label, 18, ui_theme::textPrimary(),
                                   TextView::CENTER, TextView::MIDDLE));
    button->setOnClickListener([this]() {
      previewPanelFolded = !previewPanelFolded;
      lastLayoutWidth = -1;
    });
    return button;
  };

  if (previewPanelFolded) {
    previewPanel->addView(makeFoldButton("Open"));
    rootLayout->addView(previewPanel);
    rootLayout->applyYogaLayout();
    refreshSettingsText();
    return;
  }

  previewPanel->setHeight(static_cast<float>(panelHeight));

  auto *previewHeader = new View();
  previewHeader->setFlexDirection(FlexDirection::Row);
  previewHeader->setAlignItems(YGAlignCenter);
  previewHeader->setJustifyContent(YGJustifySpaceBetween);
  previewHeader->addView(
      makeText("Preview", metrics.sectionTitleSize, ui_theme::textPrimary()));

  previewHeader->addView(makeFoldButton("Hide"));
  previewPanel->addView(previewHeader);

  auto *previewTabs = new View();
  previewTabs->setFlexDirection(FlexDirection::Row);
  previewTabs->setGap(metrics.compact ? 8.0f : 10.0f);
  previewTabs->setAlignItems(YGAlignCenter);
  previewTabs->setWidthPercent(100.0f);
  previewTabs->setJustifyContent(YGJustifyCenter);
  const int previewTabGap = metrics.compact ? 8 : 10;
  const int previewTabWidth = std::max(
      0, (panelWidth - metrics.cardPadding * 2 -
          previewTabGap * (previewPanelPageCount - 1)) /
             previewPanelPageCount);
  auto makePreviewTab = [this, &metrics, previewTabWidth](int page,
                                                          const char *label) {
    auto *labelText =
        makeText(label, metrics.bodyTextSize + 2, ui_theme::textPrimary(),
                 TextView::CENTER, TextView::MIDDLE);
    auto *button =
        previewPanelPage == page
            ? makeAccentButton(previewTabWidth, metrics.actionButtonHeight,
                               labelText, ui_theme::cyan())
            : makeControlButton(previewTabWidth, metrics.actionButtonHeight,
                                labelText);
    button->setOnClickListener([this, page]() {
      if (previewPanelPage == page) {
        return;
      }
      previewPanelPage = page;
      lastLayoutWidth = -1;
    });
    return button;
  };
  previewTabs->addView(makePreviewTab(0, "Scroll"));
  previewTabs->addView(makePreviewTab(1, "Lane"));
  previewTabs->addView(makePreviewTab(2, "HUD"));
  previewPanel->addView(previewTabs);

  auto *previewScroll = new ScrollView();
  previewScroll->setFlex(1.0f);
  previewScroll->setFlexShrink(1.0f);
  previewScroll->setWidthPercent(100.0f);

  auto *previewControls = new View();
  previewControls->setFlexDirection(FlexDirection::Column);
  previewControls->setGap(metrics.compact ? 12.0f : 16.0f);
  previewControls->setAlignItems(YGAlignStretch);
  previewScroll->setContentView(previewControls);
  previewPanel->addView(previewScroll);

  if (previewPanelPage == 0) {
    previewControls->addView(
        makeSummaryRow(metrics, "Visible Time", &summaryVisibleTimeValueText));
    previewControls->addView(buildVisibleTimeControls(metrics, false, true));

    previewControls->addView(makeSummaryRow(
        metrics, "Note Start", &summaryNoteStartPositionValueText));
    auto *noteStartControls = new View();
    noteStartControls->setFlexDirection(FlexDirection::Row);
    noteStartControls->setFlexWrap(YGWrapWrap);
    noteStartControls->setGap(metrics.compact ? 8.0f : 10.0f);
    noteStartControls->setAlignItems(YGAlignCenter);
    noteStartControls->setWidthPercent(100.0f);
    noteStartControls->setJustifyContent(YGJustifyCenter);
    auto updateNoteStartPosition = [this](int deltaPercent) {
      context.settings.noteStartPositionPercent = clampNoteStartPositionPercent(
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
    previewControls->addView(noteStartControls);
  } else if (previewPanelPage == 1) {
    previewControls->addView(
        makeSummaryRow(metrics, "Lane Angle", &summaryLaneAngleValueText));
    auto *angleControls = new View();
    angleControls->setFlexDirection(FlexDirection::Row);
    angleControls->setFlexWrap(YGWrapWrap);
    angleControls->setGap(metrics.compact ? 8.0f : 10.0f);
    angleControls->setAlignItems(YGAlignCenter);
    angleControls->setWidthPercent(100.0f);
    angleControls->setJustifyContent(YGJustifyCenter);
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
      context.settings.laneAngleDegrees = AppSettings::kDefaultLaneAngleDegrees;
      persistSettings();
    });
    angleControls->addView(resetAngle);
    previewControls->addView(angleControls);

    previewControls->addView(
        makeSummaryRow(metrics, "Lane Length", &summaryLaneLengthValueText));
    auto *lengthControls = new View();
    lengthControls->setFlexDirection(FlexDirection::Row);
    lengthControls->setFlexWrap(YGWrapWrap);
    lengthControls->setGap(metrics.compact ? 8.0f : 10.0f);
    lengthControls->setAlignItems(YGAlignCenter);
    lengthControls->setWidthPercent(100.0f);
    lengthControls->setJustifyContent(YGJustifyCenter);
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
    previewControls->addView(lengthControls);

    previewControls->addView(makeSummaryRow(
        metrics, "Beam Length", &summaryLaneBeamLengthValueText));
    auto *beamControls = new View();
    beamControls->setFlexDirection(FlexDirection::Row);
    beamControls->setFlexWrap(YGWrapWrap);
    beamControls->setGap(metrics.compact ? 8.0f : 10.0f);
    beamControls->setAlignItems(YGAlignCenter);
    beamControls->setWidthPercent(100.0f);
    beamControls->setJustifyContent(YGJustifyCenter);
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
    previewControls->addView(beamControls);

    previewControls->addView(makeSummaryRow(
        metrics, "Play Width 7K", &summaryPreviewPlayAreaWidthValueText));
    auto *playAreaWidthControls = new View();
    playAreaWidthControls->setFlexDirection(FlexDirection::Row);
    playAreaWidthControls->setFlexWrap(YGWrapWrap);
    playAreaWidthControls->setGap(metrics.compact ? 8.0f : 10.0f);
    playAreaWidthControls->setAlignItems(YGAlignCenter);
    playAreaWidthControls->setWidthPercent(100.0f);
    playAreaWidthControls->setJustifyContent(YGJustifyCenter);
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
    previewControls->addView(playAreaWidthControls);
  } else {
    auto makePreviewStepRow = [&metrics](Button *minus, Button *plus,
                                         Button *reset) {
      auto *row = new View();
      row->setFlexDirection(FlexDirection::Row);
      row->setFlexWrap(YGWrapWrap);
      row->setGap(metrics.compact ? 8.0f : 10.0f);
      row->setAlignItems(YGAlignCenter);
      row->setWidthPercent(100.0f);
      row->setJustifyContent(YGJustifyCenter);
      row->addView(minus);
      row->addView(plus);
      row->addView(reset);
      return row;
    };

    previewControls->addView(makeSummaryRow(
        metrics, "Judge Text Y", &summaryJudgementTextYValueText));
    auto updateJudgementTextY = [this](int deltaPercent) {
      const int currentPercent =
          judgementTextYToPercent(context.settings.judgementTextY);
      const int nextPercent =
          std::clamp(currentPercent + deltaPercent, 0, 100);
      context.settings.judgementTextY = judgementTextPercentToY(nextPercent);
      persistSettings();
    };
    auto *minusJudgementTextY =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "-10%");
    minusJudgementTextY->setOnClickListener(
        [updateJudgementTextY]() { updateJudgementTextY(-10); });
    auto *plusJudgementTextY =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "+10%");
    plusJudgementTextY->setOnClickListener(
        [updateJudgementTextY]() { updateJudgementTextY(10); });
    auto *resetJudgementTextY = makeResetButton(metrics);
    resetJudgementTextY->setOnClickListener([this]() {
      context.settings.judgementTextY = AppSettings::kDefaultJudgementTextY;
      persistSettings();
    });
    previewControls->addView(makePreviewStepRow(
        minusJudgementTextY, plusJudgementTextY, resetJudgementTextY));

    previewControls->addView(makeSummaryRow(
        metrics, "FAST/SLOW", &summaryJudgementTimingFastSlowValueText));
    auto *timingFastSlowControls = new View();
    timingFastSlowControls->setFlexDirection(FlexDirection::Row);
    timingFastSlowControls->setFlexWrap(YGWrapWrap);
    timingFastSlowControls->setGap(metrics.compact ? 8.0f : 10.0f);
    timingFastSlowControls->setAlignItems(YGAlignCenter);
    timingFastSlowControls->setWidthPercent(100.0f);
    timingFastSlowControls->setJustifyContent(YGJustifyCenter);
    judgementTimingFastSlowCriteriaText =
        makeText("", metrics.bodyTextSize, ui_theme::textPrimary(),
                 TextView::CENTER, TextView::MIDDLE);
    judgementTimingFastSlowCriteriaButton =
        makeControlButton(metrics.actionButtonWidth,
                          metrics.actionButtonHeight,
                          judgementTimingFastSlowCriteriaText);
    judgementTimingFastSlowCriteriaButton->setOnClickListener([this]() {
      context.settings.judgementTimingFastSlowCriteria =
          nextJudgementTimingDisplayCriteria(
              context.settings.judgementTimingFastSlowCriteria);
      persistSettings();
    });
    timingFastSlowControls->addView(judgementTimingFastSlowCriteriaButton);
    previewControls->addView(timingFastSlowControls);

    previewControls->addView(makeSummaryRow(
        metrics, "Milliseconds",
        &summaryJudgementTimingMillisecondsValueText));
    auto *timingMillisecondsControls = new View();
    timingMillisecondsControls->setFlexDirection(FlexDirection::Row);
    timingMillisecondsControls->setFlexWrap(YGWrapWrap);
    timingMillisecondsControls->setGap(metrics.compact ? 8.0f : 10.0f);
    timingMillisecondsControls->setAlignItems(YGAlignCenter);
    timingMillisecondsControls->setWidthPercent(100.0f);
    timingMillisecondsControls->setJustifyContent(YGJustifyCenter);
    judgementTimingMillisecondsCriteriaText =
        makeText("", metrics.bodyTextSize, ui_theme::textPrimary(),
                 TextView::CENTER, TextView::MIDDLE);
    judgementTimingMillisecondsCriteriaButton =
        makeControlButton(metrics.actionButtonWidth,
                          metrics.actionButtonHeight,
                          judgementTimingMillisecondsCriteriaText);
    judgementTimingMillisecondsCriteriaButton->setOnClickListener([this]() {
      context.settings.judgementTimingMillisecondsCriteria =
          nextJudgementTimingDisplayCriteria(
              context.settings.judgementTimingMillisecondsCriteria);
      persistSettings();
    });
    timingMillisecondsControls->addView(
        judgementTimingMillisecondsCriteriaButton);
    previewControls->addView(timingMillisecondsControls);

    previewControls->addView(
        makeText("Indicator", metrics.summaryValueSize,
                 ui_theme::textSecondary()));
    auto *indicatorModeControls = new View();
    indicatorModeControls->setFlexDirection(FlexDirection::Row);
    indicatorModeControls->setFlexWrap(YGWrapWrap);
    indicatorModeControls->setGap(metrics.compact ? 8.0f : 10.0f);
    indicatorModeControls->setAlignItems(YGAlignCenter);
    indicatorModeControls->setWidthPercent(100.0f);
    indicatorModeControls->setJustifyContent(YGJustifyCenter);
    judgementIndicatorModeText =
        makeText("", metrics.bodyTextSize + 4, ui_theme::textPrimary(),
                 TextView::CENTER, TextView::MIDDLE);
    judgementIndicatorModeButton =
        makeAccentButton(metrics.actionButtonWidth, metrics.actionButtonHeight,
                         judgementIndicatorModeText, ui_theme::lime());
    judgementIndicatorModeButton->setOnClickListener([this]() {
      context.settings.judgementIndicatorEnabled =
          !context.settings.judgementIndicatorEnabled;
      persistSettings();
    });
    indicatorModeControls->addView(judgementIndicatorModeButton);
    judgementIndicatorRenderModeText =
        makeText("", metrics.bodyTextSize + 4, ui_theme::textPrimary(),
                 TextView::CENTER, TextView::MIDDLE);
    judgementIndicatorRenderModeButton =
        makeControlButton(metrics.actionButtonWidth,
                          metrics.actionButtonHeight,
                          judgementIndicatorRenderModeText);
    judgementIndicatorRenderModeButton->setOnClickListener([this]() {
      context.settings.judgementIndicatorRenderMode =
          nextJudgementIndicatorRenderMode(
              context.settings.judgementIndicatorRenderMode);
      persistSettings();
    });
    indicatorModeControls->addView(judgementIndicatorRenderModeButton);
    previewControls->addView(indicatorModeControls);

    previewControls->addView(makeSummaryRow(
        metrics, "Indicator Y", &summaryJudgementIndicatorYValueText));
    auto updateIndicatorY = [this](int deltaPercent) {
      const int currentPercent =
          judgementIndicatorYToPercent(context.settings.judgementIndicatorY);
      const int nextPercent =
          std::clamp(currentPercent + deltaPercent, 0, 100);
      context.settings.judgementIndicatorY =
          judgementIndicatorPercentToY(nextPercent);
      persistSettings();
    };
    auto *minusIndicatorY =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "-10%");
    minusIndicatorY->setOnClickListener(
        [updateIndicatorY]() { updateIndicatorY(-10); });
    auto *plusIndicatorY =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "+10%");
    plusIndicatorY->setOnClickListener(
        [updateIndicatorY]() { updateIndicatorY(10); });
    auto *resetIndicatorY = makeResetButton(metrics);
    resetIndicatorY->setOnClickListener([this]() {
      context.settings.judgementIndicatorY =
          AppSettings::kDefaultJudgementIndicatorY;
      persistSettings();
    });
    previewControls->addView(
        makePreviewStepRow(minusIndicatorY, plusIndicatorY, resetIndicatorY));

    previewControls->addView(makeSummaryRow(
        metrics, "Indicator Width", &summaryJudgementIndicatorWidthValueText));
    auto updateIndicatorWidth = [this](int deltaPercent) {
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
    };
    auto *minusIndicatorWidth =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "-10%");
    minusIndicatorWidth->setOnClickListener(
        [updateIndicatorWidth]() { updateIndicatorWidth(-10); });
    auto *plusIndicatorWidth =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "+10%");
    plusIndicatorWidth->setOnClickListener(
        [updateIndicatorWidth]() { updateIndicatorWidth(10); });
    auto *resetIndicatorWidth = makeResetButton(metrics);
    resetIndicatorWidth->setOnClickListener([this]() {
      context.settings.judgementIndicatorWidthScale =
          AppSettings::kDefaultJudgementIndicatorWidthScale;
      persistSettings();
    });
    previewControls->addView(makePreviewStepRow(
        minusIndicatorWidth, plusIndicatorWidth, resetIndicatorWidth));

    previewControls->addView(makeSummaryRow(
        metrics, "Counter", &summaryJudgementCounterPositionValueText));
    auto *counterControls = new View();
    counterControls->setFlexDirection(FlexDirection::Row);
    counterControls->setFlexWrap(YGWrapWrap);
    counterControls->setGap(metrics.compact ? 8.0f : 10.0f);
    counterControls->setAlignItems(YGAlignCenter);
    counterControls->setWidthPercent(100.0f);
    counterControls->setJustifyContent(YGJustifyCenter);
    judgementCounterModeText =
        makeText("", metrics.bodyTextSize + 4, ui_theme::textPrimary(),
                 TextView::CENTER, TextView::MIDDLE);
    judgementCounterModeButton =
        makeAccentButton(metrics.actionButtonWidth, metrics.actionButtonHeight,
                         judgementCounterModeText, ui_theme::lime());
    judgementCounterModeButton->setOnClickListener([this]() {
      context.settings.judgementCounterEnabled =
          !context.settings.judgementCounterEnabled;
      persistSettings();
    });
    counterControls->addView(judgementCounterModeButton);
    judgementCounterPositionText =
        makeText("", metrics.bodyTextSize + 4, ui_theme::textPrimary(),
                 TextView::CENTER, TextView::MIDDLE);
    judgementCounterPositionButton =
        makeControlButton(metrics.actionButtonWidth,
                          metrics.actionButtonHeight,
                          judgementCounterPositionText);
    judgementCounterPositionButton->setOnClickListener([this]() {
      context.settings.judgementCounterPosition =
          nextJudgementCounterPosition(
              context.settings.judgementCounterPosition);
      persistSettings();
    });
    counterControls->addView(judgementCounterPositionButton);
    previewControls->addView(counterControls);

    previewControls->addView(makeSummaryRow(
        metrics, "Gauge", &summaryGaugeBarPositionValueText));
    auto *gaugeControls = new View();
    gaugeControls->setFlexDirection(FlexDirection::Row);
    gaugeControls->setFlexWrap(YGWrapWrap);
    gaugeControls->setGap(metrics.compact ? 8.0f : 10.0f);
    gaugeControls->setAlignItems(YGAlignCenter);
    gaugeControls->setWidthPercent(100.0f);
    gaugeControls->setJustifyContent(YGJustifyCenter);
    gaugeBarPositionText =
        makeText("", metrics.bodyTextSize + 4, ui_theme::textPrimary(),
                 TextView::CENTER, TextView::MIDDLE);
    gaugeBarPositionButton =
        makeControlButton(metrics.actionButtonWidth,
                          metrics.actionButtonHeight,
                          gaugeBarPositionText);
    gaugeBarPositionButton->setOnClickListener([this]() {
      context.settings.gaugeBarPosition =
          nextGaugeBarPosition(context.settings.gaugeBarPosition);
      persistSettings();
    });
    gaugeControls->addView(gaugeBarPositionButton);
    previewControls->addView(gaugeControls);
  }

  auto *restartButton = makeButton(
      metrics.actionButtonWidth, metrics.actionButtonHeight,
      makeText("Restart", metrics.bodyTextSize + 4, ui_theme::textPrimary(),
               TextView::CENTER, TextView::MIDDLE),
      ui_theme::control(), ui_theme::controlHover(), ui_theme::controlPressed(),
      ui_theme::hairline(), ui_theme::cyan(), ui_theme::cyan());
  restartButton->setOnClickListener([this]() { resetPreviewSimulation(); });

  auto *doneButton = makeButton(
      metrics.actionButtonWidth, metrics.actionButtonHeight,
      makeText("Done", metrics.bodyTextSize + 4, ui_theme::textPrimary(),
               TextView::CENTER, TextView::MIDDLE),
      ui_theme::control(), ui_theme::controlHover(), ui_theme::controlPressed(),
      ui_theme::hairline(), ui_theme::cyan(), ui_theme::cyan());
  doneButton->setOnClickListener([this]() { stopLanePreview(); });

  auto *previewActions = new View();
  previewActions->setFlexDirection(metrics.compact ? FlexDirection::Column
                                                   : FlexDirection::Row);
  previewActions->setFlexWrap(YGWrapWrap);
  previewActions->setGap(metrics.compact ? 12.0f : 10.0f);
  previewActions->setAlignItems(YGAlignCenter);
  previewActions->setWidthPercent(100.0f);
  previewActions->setJustifyContent(YGJustifyCenter);
  previewActions->addView(restartButton);
  previewActions->addView(doneButton);
  previewPanel->addView(previewActions);

  rootLayout->addView(previewPanel);
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
      makeStepButton(metrics, metrics.offsetButtonWidthLarge, "-10");
  minusTen->setOnClickListener([updateOffset]() { updateOffset(-10); });
  offsetControls->addView(minusTen);

  auto *minusOne =
      makeStepButton(metrics, metrics.offsetButtonWidthSmall, "-1");
  minusOne->setOnClickListener([updateOffset]() { updateOffset(-1); });
  offsetControls->addView(minusOne);

  auto *offsetValue = new View();
  offsetValue->setWidth(static_cast<float>(metrics.offsetValueWidth));
  offsetValue->setHeight(static_cast<float>(metrics.actionButtonHeight));
  offsetValue->setThemedBackgroundColor(ui_theme::control);
  offsetValue->setCornerRadius(ui_theme::controlRadius());
  offsetValue->setThemedBorderColor(ui_theme::hairline);
  offsetValue->setBorderWidth(1);
  offsetInput = new TextInputBox(kFontPath, metrics.bodyTextSize + 6);
  offsetInput->setText("");
  offsetInput->setSize(metrics.offsetValueWidth, metrics.actionButtonHeight);
  offsetInput->setBackgroundColor(Color(0, 0, 0, 0));
  offsetInput->setBorderWidth(0);
  offsetInput->setAlign(TextView::CENTER);
  offsetInput->setVAlign(TextView::MIDDLE);
  offsetInput->setThemedColor(ui_theme::textPrimary);
  offsetInput->onEditingFinished(
      [this](const std::string &) { commitOffsetInput(); });
  offsetValue->addView(offsetInput);
  offsetControls->addView(offsetValue);

  auto *plusOne = makeStepButton(metrics, metrics.offsetButtonWidthSmall, "+1");
  plusOne->setOnClickListener([updateOffset]() { updateOffset(1); });
  offsetControls->addView(plusOne);

  auto *plusTen =
      makeStepButton(metrics, metrics.offsetButtonWidthLarge, "+10");
  plusTen->setOnClickListener([updateOffset]() { updateOffset(10); });
  offsetControls->addView(plusTen);

  auto *resetOffset = makeResetButton(metrics);
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
      makeStepButton(metrics, metrics.offsetButtonWidthLarge, "-10");
  minusVisualTen->setOnClickListener(
      [updateVisualOffset]() { updateVisualOffset(-10); });
  visualOffsetControls->addView(minusVisualTen);

  auto *minusVisualOne =
      makeStepButton(metrics, metrics.offsetButtonWidthSmall, "-1");
  minusVisualOne->setOnClickListener(
      [updateVisualOffset]() { updateVisualOffset(-1); });
  visualOffsetControls->addView(minusVisualOne);

  auto *visualOffsetValue = new View();
  visualOffsetValue->setWidth(static_cast<float>(metrics.offsetValueWidth));
  visualOffsetValue->setHeight(static_cast<float>(metrics.actionButtonHeight));
  visualOffsetValue->setThemedBackgroundColor(ui_theme::control);
  visualOffsetValue->setCornerRadius(ui_theme::controlRadius());
  visualOffsetValue->setThemedBorderColor(ui_theme::hairline);
  visualOffsetValue->setBorderWidth(1);
  visualOffsetInput = new TextInputBox(kFontPath, metrics.bodyTextSize + 6);
  visualOffsetInput->setText("");
  visualOffsetInput->setSize(metrics.offsetValueWidth,
                             metrics.actionButtonHeight);
  visualOffsetInput->setBackgroundColor(Color(0, 0, 0, 0));
  visualOffsetInput->setBorderWidth(0);
  visualOffsetInput->setAlign(TextView::CENTER);
  visualOffsetInput->setVAlign(TextView::MIDDLE);
  visualOffsetInput->setThemedColor(ui_theme::textPrimary);
  visualOffsetInput->onEditingFinished(
      [this](const std::string &) { commitVisualOffsetInput(); });
  visualOffsetValue->addView(visualOffsetInput);
  visualOffsetControls->addView(visualOffsetValue);

  auto *plusVisualOne =
      makeStepButton(metrics, metrics.offsetButtonWidthSmall, "+1");
  plusVisualOne->setOnClickListener(
      [updateVisualOffset]() { updateVisualOffset(1); });
  visualOffsetControls->addView(plusVisualOne);

  auto *plusVisualTen =
      makeStepButton(metrics, metrics.offsetButtonWidthLarge, "+10");
  plusVisualTen->setOnClickListener(
      [updateVisualOffset]() { updateVisualOffset(10); });
  visualOffsetControls->addView(plusVisualTen);

  auto *resetVisualOffset = makeResetButton(metrics);
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

  auto *judgementFeedbackControls = new View();
  judgementFeedbackControls->setFlexDirection(FlexDirection::Column);
  judgementFeedbackControls->setGap(metrics.compact ? 12.0f : 16.0f);
  judgementFeedbackControls->setAlignItems(YGAlignFlexStart);
  judgementFeedbackControls->addView(makeWrappedText(
      metrics.compact
          ? "Place judgement text and choose timing feedback thresholds."
          : "Place the judgement/combo text and choose when FAST/SLOW and "
            "millisecond timing feedback appear.",
      metrics.bodyTextSize, ui_theme::textSecondary()));

  judgementFeedbackControls->addView(makeSummaryRow(
      metrics, "Judge Text Y", &summaryJudgementTextYValueText));
  auto *judgementTextYControls = new View();
  judgementTextYControls->setFlexDirection(FlexDirection::Row);
  judgementTextYControls->setFlexWrap(YGWrapWrap);
  judgementTextYControls->setGap(metrics.compact ? 8.0f : 12.0f);
  judgementTextYControls->setAlignItems(YGAlignFlexStart);
  auto updateJudgementTextY = [this](int deltaPercent) {
    const int currentPercent =
        judgementTextYToPercent(context.settings.judgementTextY);
    const int nextPercent = std::clamp(currentPercent + deltaPercent, 0, 100);
    context.settings.judgementTextY = judgementTextPercentToY(nextPercent);
    persistSettings();
  };
  auto *minusJudgementTextYLarge =
      makeStepButton(metrics, metrics.offsetButtonWidthLarge, "-10%");
  minusJudgementTextYLarge->setOnClickListener(
      [updateJudgementTextY]() { updateJudgementTextY(-10); });
  judgementTextYControls->addView(minusJudgementTextYLarge);
  auto *minusJudgementTextYSmall =
      makeStepButton(metrics, metrics.offsetButtonWidthSmall, "-1%");
  minusJudgementTextYSmall->setOnClickListener(
      [updateJudgementTextY]() { updateJudgementTextY(-1); });
  judgementTextYControls->addView(minusJudgementTextYSmall);
  auto *plusJudgementTextYSmall =
      makeStepButton(metrics, metrics.offsetButtonWidthSmall, "+1%");
  plusJudgementTextYSmall->setOnClickListener(
      [updateJudgementTextY]() { updateJudgementTextY(1); });
  judgementTextYControls->addView(plusJudgementTextYSmall);
  auto *plusJudgementTextYLarge =
      makeStepButton(metrics, metrics.offsetButtonWidthLarge, "+10%");
  plusJudgementTextYLarge->setOnClickListener(
      [updateJudgementTextY]() { updateJudgementTextY(10); });
  judgementTextYControls->addView(plusJudgementTextYLarge);
  auto *resetJudgementTextY = makeResetButton(metrics);
  resetJudgementTextY->setOnClickListener([this]() {
    context.settings.judgementTextY = AppSettings::kDefaultJudgementTextY;
    persistSettings();
  });
  judgementTextYControls->addView(resetJudgementTextY);
  judgementFeedbackControls->addView(judgementTextYControls);

  auto *timingCriteriaControls = new View();
  timingCriteriaControls->setFlexDirection(FlexDirection::Row);
  timingCriteriaControls->setFlexWrap(YGWrapWrap);
  timingCriteriaControls->setGap(metrics.compact ? 8.0f : 12.0f);
  timingCriteriaControls->setAlignItems(YGAlignFlexStart);

  auto *timingFastSlowGroup = new View();
  timingFastSlowGroup->setFlexDirection(FlexDirection::Column);
  timingFastSlowGroup->setGap(metrics.compact ? 6.0f : 8.0f);
  timingFastSlowGroup->addView(
      makeText("FAST/SLOW", metrics.bodyTextSize, ui_theme::textSecondary()));
  judgementTimingFastSlowCriteriaText =
      makeText("", metrics.bodyTextSize + 4, ui_theme::textPrimary(),
               TextView::CENTER, TextView::MIDDLE);
  judgementTimingFastSlowCriteriaButton =
      makeControlButton(metrics.actionButtonWidth, metrics.actionButtonHeight,
                        judgementTimingFastSlowCriteriaText);
  judgementTimingFastSlowCriteriaButton->setOnClickListener([this]() {
    context.settings.judgementTimingFastSlowCriteria =
        nextJudgementTimingDisplayCriteria(
            context.settings.judgementTimingFastSlowCriteria);
    persistSettings();
  });
  timingFastSlowGroup->addView(judgementTimingFastSlowCriteriaButton);
  timingCriteriaControls->addView(timingFastSlowGroup);

  auto *timingMillisecondsGroup = new View();
  timingMillisecondsGroup->setFlexDirection(FlexDirection::Column);
  timingMillisecondsGroup->setGap(metrics.compact ? 6.0f : 8.0f);
  timingMillisecondsGroup->addView(makeText(
      "Milliseconds", metrics.bodyTextSize, ui_theme::textSecondary()));
  judgementTimingMillisecondsCriteriaText =
      makeText("", metrics.bodyTextSize + 4, ui_theme::textPrimary(),
               TextView::CENTER, TextView::MIDDLE);
  judgementTimingMillisecondsCriteriaButton =
      makeControlButton(metrics.actionButtonWidth, metrics.actionButtonHeight,
                        judgementTimingMillisecondsCriteriaText);
  judgementTimingMillisecondsCriteriaButton->setOnClickListener([this]() {
    context.settings.judgementTimingMillisecondsCriteria =
        nextJudgementTimingDisplayCriteria(
            context.settings.judgementTimingMillisecondsCriteria);
    persistSettings();
  });
  timingMillisecondsGroup->addView(judgementTimingMillisecondsCriteriaButton);
  timingCriteriaControls->addView(timingMillisecondsGroup);
  judgementFeedbackControls->addView(timingCriteriaControls);

  cardsColumn->addView(makeCard(
      metrics, "Judgement Feedback",
      metrics.compact
          ? "Configure judgement text and timing feedback display."
          : "Configure judgement/combo placement and separate FAST/SLOW and "
            "millisecond display thresholds.",
      judgementFeedbackControls, metrics.visibleTimeCardHeight,
      metrics.cardsWidth));

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
      makeText("", metrics.bodyTextSize + 6, ui_theme::textPrimary(),
               TextView::CENTER, TextView::MIDDLE);
  judgementIndicatorModeButton =
      makeAccentButton(metrics.actionButtonWidth, metrics.actionButtonHeight,
                       judgementIndicatorModeText, ui_theme::lime());
  judgementIndicatorModeButton->setOnClickListener([this]() {
    context.settings.judgementIndicatorEnabled =
        !context.settings.judgementIndicatorEnabled;
    persistSettings();
  });
  judgementIndicatorModeControls->addView(judgementIndicatorModeButton);

  judgementIndicatorRenderModeText =
      makeText("", metrics.bodyTextSize + 6, ui_theme::textPrimary(),
               TextView::CENTER, TextView::MIDDLE);
  judgementIndicatorRenderModeButton =
      makeControlButton(metrics.actionButtonWidth, metrics.actionButtonHeight,
                        judgementIndicatorRenderModeText);
  judgementIndicatorRenderModeButton->setOnClickListener([this]() {
    context.settings.judgementIndicatorRenderMode =
        nextJudgementIndicatorRenderMode(
            context.settings.judgementIndicatorRenderMode);
    persistSettings();
  });
  judgementIndicatorModeControls->addView(judgementIndicatorRenderModeButton);
  judgementIndicatorControls->addView(judgementIndicatorModeControls);

  judgementIndicatorControls->addView(
      makeText("Y Position", metrics.bodyTextSize, ui_theme::textSecondary()));
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
      makeText("Width", metrics.bodyTextSize, ui_theme::textSecondary()));
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
      metrics.bodyTextSize, ui_theme::textSecondary()));
  keysoundModeText =
      makeText("", metrics.bodyTextSize + 6, ui_theme::textPrimary(),
               TextView::CENTER, TextView::MIDDLE);
  keysoundModeButton = makeControlButton(
      metrics.actionButtonWidth, metrics.actionButtonHeight, keysoundModeText);
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
      metrics.bodyTextSize, ui_theme::textSecondary()));
  notePriorityModeText =
      makeText("", metrics.bodyTextSize + 6, ui_theme::textPrimary(),
               TextView::CENTER, TextView::MIDDLE);
  notePriorityModeButton =
      makeControlButton(metrics.actionButtonWidth, metrics.actionButtonHeight,
                        notePriorityModeText);
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
      metrics.bodyTextSize, ui_theme::textSecondary()));
  bgaModeText = makeText("", metrics.bodyTextSize + 6, ui_theme::textPrimary(),
                         TextView::CENTER, TextView::MIDDLE);
  bgaModeButton = makeControlButton(metrics.actionButtonWidth,
                                    metrics.actionButtonHeight, bgaModeText);
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
      metrics.bodyTextSize, ui_theme::textSecondary()));
  showInvisibleNotesModeText =
      makeText("", metrics.bodyTextSize + 6, ui_theme::textPrimary(),
               TextView::CENTER, TextView::MIDDLE);
  showInvisibleNotesModeButton =
      makeControlButton(metrics.actionButtonWidth, metrics.actionButtonHeight,
                        showInvisibleNotesModeText);
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

  auto *judgementCounterControls = new View();
  judgementCounterControls->setFlexDirection(FlexDirection::Column);
  judgementCounterControls->setGap(metrics.compact ? 12.0f : 16.0f);
  judgementCounterControls->setAlignItems(YGAlignFlexStart);
  judgementCounterControls->addView(makeWrappedText(
      metrics.compact
          ? "Enable totals and choose where they sit."
          : "Enable realtime judgement totals and choose where they sit "
            "during gameplay. Top uses a horizontal strip; Left and Right use "
            "vertical stacks.",
      metrics.bodyTextSize, ui_theme::textSecondary()));
  auto *judgementCounterModeControls = new View();
  judgementCounterModeControls->setFlexDirection(FlexDirection::Row);
  judgementCounterModeControls->setFlexWrap(YGWrapWrap);
  judgementCounterModeControls->setGap(metrics.compact ? 8.0f : 10.0f);
  judgementCounterModeControls->setAlignItems(YGAlignFlexStart);
  judgementCounterModeText =
      makeText("", metrics.bodyTextSize + 6, ui_theme::textPrimary(),
               TextView::CENTER, TextView::MIDDLE);
  judgementCounterModeButton =
      makeAccentButton(metrics.actionButtonWidth, metrics.actionButtonHeight,
                       judgementCounterModeText, ui_theme::lime());
  judgementCounterModeButton->setOnClickListener([this]() {
    context.settings.judgementCounterEnabled =
        !context.settings.judgementCounterEnabled;
    persistSettings();
  });
  judgementCounterModeControls->addView(judgementCounterModeButton);
  judgementCounterPositionText =
      makeText("", metrics.bodyTextSize + 6, ui_theme::textPrimary(),
               TextView::CENTER, TextView::MIDDLE);
  judgementCounterPositionButton =
      makeControlButton(metrics.actionButtonWidth, metrics.actionButtonHeight,
                        judgementCounterPositionText);
  judgementCounterPositionButton->setOnClickListener([this]() {
    context.settings.judgementCounterPosition =
        nextJudgementCounterPosition(
            context.settings.judgementCounterPosition);
    persistSettings();
  });
  judgementCounterModeControls->addView(judgementCounterPositionButton);
  judgementCounterControls->addView(judgementCounterModeControls);
  cardsColumn->addView(makeCard(
      metrics, "Judgement Counter",
      metrics.compact ? "Realtime judgement counts during gameplay."
                      : "Show realtime judgement totals in the gameplay HUD "
                        "without changing lane rendering.",
      judgementCounterControls, metrics.modeCardHeight, metrics.cardsWidth));

  auto *gaugeControls = new View();
  gaugeControls->setFlexDirection(FlexDirection::Column);
  gaugeControls->setGap(metrics.compact ? 12.0f : 16.0f);
  gaugeControls->setAlignItems(YGAlignFlexStart);
  gaugeControls->addView(makeWrappedText(
      metrics.compact
          ? "Choose world-space bar or side HUD bar."
          : "World draws a horizontal gauge below the judgement line. Left "
            "and Right draw a vertical 2D HUD gauge on that side.",
      metrics.bodyTextSize, ui_theme::textSecondary()));
  auto *gaugePositionControls = new View();
  gaugePositionControls->setFlexDirection(FlexDirection::Row);
  gaugePositionControls->setFlexWrap(YGWrapWrap);
  gaugePositionControls->setGap(metrics.compact ? 8.0f : 10.0f);
  gaugePositionControls->setAlignItems(YGAlignFlexStart);
  gaugeBarPositionText =
      makeText("", metrics.bodyTextSize + 6, ui_theme::textPrimary(),
               TextView::CENTER, TextView::MIDDLE);
  gaugeBarPositionButton =
      makeControlButton(metrics.actionButtonWidth, metrics.actionButtonHeight,
                        gaugeBarPositionText);
  gaugeBarPositionButton->setOnClickListener([this]() {
    context.settings.gaugeBarPosition =
        nextGaugeBarPosition(context.settings.gaugeBarPosition);
    persistSettings();
  });
  gaugePositionControls->addView(gaugeBarPositionButton);
  gaugeControls->addView(gaugePositionControls);
  cardsColumn->addView(makeCard(
      metrics, "Gauge Bar",
      metrics.compact ? "Gameplay gauge placement."
                      : "Render the current gauge as a live bar instead of a "
                        "text-only HUD line.",
      gaugeControls, metrics.modeCardHeight, metrics.cardsWidth));

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
      metrics.bodyTextSize, ui_theme::textSecondary()));
  bgaDisplayModeText =
      makeText("", metrics.bodyTextSize + 6, ui_theme::textPrimary(),
               TextView::CENTER, TextView::MIDDLE);
  bgaDisplayModeButton =
      makeControlButton(metrics.actionButtonWidth, metrics.actionButtonHeight,
                        bgaDisplayModeText);
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

  auto *previewControls = new View();
  previewControls->setFlexDirection(FlexDirection::Column);
  previewControls->setGap(metrics.compact ? 12.0f : 16.0f);
  previewControls->setAlignItems(YGAlignFlexStart);
  previewControls->addView(makeWrappedText(
      metrics.compact
          ? "Open a live gameplay preview with falling notes."
          : "Open a live gameplay preview with falling notes. It uses the "
            "same lane renderer, camera, viewport, notes, and HUD as "
            "gameplay.",
      metrics.bodyTextSize, ui_theme::textSecondary()));
  auto *previewButton = makeAccentButton(
      metrics.actionButtonWidth, metrics.actionButtonHeight,
      makeText("Preview", metrics.bodyTextSize + 4, ui_theme::textPrimary(),
               TextView::CENTER, TextView::MIDDLE),
      ui_theme::lime());
  previewButton->setOnClickListener([this]() { startLanePreview(); });
  previewControls->addView(previewButton);
  cardsColumn->addView(makeCard(
      metrics, "Gameplay Preview",
      metrics.compact ? "Test lane and HUD setup in the gameplay renderer."
                      : "Test lane and HUD setup in the gameplay renderer "
                        "before entering a chart.",
      previewControls, metrics.modeCardHeight, metrics.cardsWidth));

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
                 ui_theme::textPrimary(), TextView::CENTER, TextView::MIDDLE);
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

    input->onEditingFinished(
        [this, keyMode, input, applyWidth](const std::string &) {
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
    resetWidth->setOnClickListener(
        [applyWidth]() { applyWidth(AppSettings::kDefaultPlayAreaWidth); });
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

  return cardsColumn;
}

View *SettingsScene::buildMiscTab(const LayoutMetrics &metrics) {
  auto *cardsColumn = makeCardsColumn(metrics);
  measureTemporaryArchiveCache();

  auto *themeControls = new View();
  themeControls->setFlexDirection(FlexDirection::Column);
  themeControls->setGap(metrics.compact ? 12.0f : 16.0f);
  themeControls->setAlignItems(YGAlignFlexStart);
  themeControls->addView(makeWrappedText(
      metrics.compact ? "Switch the UI palette."
                      : "Switch between the dark prismatic palette and a "
                        "light high-contrast palette.",
      metrics.bodyTextSize, ui_theme::textSecondary()));
  uiThemeModeText =
      makeText("", metrics.bodyTextSize + 6, ui_theme::textPrimary(),
               TextView::CENTER, TextView::MIDDLE);
  uiThemeModeButton =
      makeAccentButton(metrics.actionButtonWidth, metrics.actionButtonHeight,
                       uiThemeModeText, ui_theme::cyan());
  uiThemeModeButton->setOnClickListener([this]() {
    context.settings.uiThemeMode =
        nextUiThemeMode(context.settings.uiThemeMode);
    persistSettings();
    lastLayoutWidth = -1;
  });
  themeControls->addView(uiThemeModeButton);
  cardsColumn->addView(makeCard(
      metrics, "Theme",
      metrics.compact ? "Dark stays colorful without the old graphite panels."
                      : "Dark keeps the stage-light rhythm-game mood without "
                        "the old graphite panels. Light is available for "
                        "brighter environments.",
      themeControls, metrics.modeCardHeight, metrics.cardsWidth));

  auto *archivePreviewControls = new View();
  archivePreviewControls->setFlexDirection(FlexDirection::Column);
  archivePreviewControls->setGap(metrics.compact ? 12.0f : 16.0f);
  archivePreviewControls->setAlignItems(YGAlignFlexStart);
  archivePreviewControls->addView(makeWrappedText(
      metrics.compact ? "Preview selected charts inside non-solid archives."
                      : "Preview selected charts inside non-solid archives "
                        "from the song select screen.",
      metrics.bodyTextSize, ui_theme::textSecondary()));
  archiveChartPreviewModeText =
      makeText("", metrics.bodyTextSize + 6, ui_theme::textPrimary(),
               TextView::CENTER, TextView::MIDDLE);
  archiveChartPreviewModeButton =
      makeAccentButton(metrics.actionButtonWidth, metrics.actionButtonHeight,
                       archiveChartPreviewModeText, ui_theme::lime());
  archiveChartPreviewModeButton->setOnClickListener([this]() {
    context.settings.archiveChartPreviewEnabled =
        !context.settings.archiveChartPreviewEnabled;
    persistSettings();
  });
  archivePreviewControls->addView(archiveChartPreviewModeButton);
  cardsColumn->addView(makeCard(
      metrics, "Archive Chart Preview",
      metrics.compact ? "Disable this if archive previews make selection feel "
                        "heavy."
                      : "Disable this if archive previews make song selection "
                        "feel heavy on large packs.",
      archivePreviewControls, metrics.modeCardHeight, metrics.cardsWidth));

  auto *cacheCleanupControls = new View();
  cacheCleanupControls->setFlexDirection(FlexDirection::Column);
  cacheCleanupControls->setGap(metrics.compact ? 12.0f : 16.0f);
  cacheCleanupControls->setAlignItems(YGAlignFlexStart);
  cacheCleanupControls->addView(makeWrappedText(
      metrics.compact ? "Remove temporary files extracted from archives."
                      : "Remove temporary BGA and video files extracted from "
                        "archives. They will be recreated when needed.",
      metrics.bodyTextSize, ui_theme::textSecondary()));
  archiveCacheCleanupButtonText =
      makeText(archiveCacheCleanupRunning.load() ? "Cleaning..." : "Clean Up",
               metrics.bodyTextSize + 4, ui_theme::textPrimary(),
               TextView::CENTER, TextView::MIDDLE);
  archiveCacheCleanupButton =
      makeAccentButton(metrics.actionButtonWidth, metrics.actionButtonHeight,
                       archiveCacheCleanupButtonText, ui_theme::coral());
  archiveCacheCleanupButton->setOnClickListener(
      [this]() { cleanupTemporaryArchiveCache(); });
  cacheCleanupControls->addView(archiveCacheCleanupButton);
  archiveCacheCleanupStatusText =
      makeWrappedText(archiveCacheCleanupStatusMessage, metrics.bodyTextSize,
                      ui_theme::textSecondary());
  archiveCacheCleanupStatusText->setColor(archiveCacheCleanupStatusColor);
  cacheCleanupControls->addView(archiveCacheCleanupStatusText);
  cardsColumn->addView(makeCard(
      metrics, "Archive Temporary Cache",
      metrics.compact ? "Clear extracted archive media."
                      : "Clear temporary files made while playing media from "
                        "archives.",
      cacheCleanupControls, metrics.modeCardHeight, metrics.cardsWidth));

  return cardsColumn;
}

View *SettingsScene::buildDifficultyTablesTab(const LayoutMetrics &metrics) {
  auto *cardsColumn = makeCardsColumn(metrics);
  loadDifficultyTables();

  const std::string tableCardDescription =
      metrics.compact ? "Import a bmstable page, header, or table list URL."
                      : "Import a bmstable page URL or a direct header JSON "
                        "URL. Table-list JSON URLs import each listed table.";

  auto *addControls = new View();
  addControls->setFlexDirection(FlexDirection::Column);
  addControls->setGap(metrics.compact ? 12.0f : 16.0f);
  addControls->setAlignItems(YGAlignFlexStart);
  addControls->setAlignSelf(YGAlignStretch);

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

  auto *addButton = makeAccentButton(
      addButtonWidth, metrics.actionButtonHeight,
      makeText("Add Table", metrics.bodyTextSize + 4, ui_theme::textPrimary(),
               TextView::CENTER, TextView::MIDDLE),
      ui_theme::lime());
  addButton->setOnClickListener([this]() { addDifficultyTableFromUrl(); });
  urlRow->addView(addButton);
  addControls->addView(urlRow);

  difficultyTableStatusText = makeWrappedText(
      difficultyTableStatusMessage, metrics.bodyTextSize,
      Color(difficultyTableStatusColor.r, difficultyTableStatusColor.g,
            difficultyTableStatusColor.b, difficultyTableStatusColor.a));
  addControls->addView(difficultyTableStatusText);

  auto *tableList = new View();
  tableList->setFlexDirection(FlexDirection::Column);
  tableList->setGap(metrics.compact ? 10.0f : 12.0f);
  tableList->setAlignSelf(YGAlignStretch);

  if (difficultyTables.empty()) {
    tableList->addView(makeWrappedText("No difficulty tables are installed.",
                                       metrics.bodyTextSize,
                                       ui_theme::textSecondary()));
  } else {
    for (const auto &table : difficultyTables) {
      auto *row = new View();
      row->setFlexDirection(FlexDirection::Column);
      row->setGap(metrics.compact ? 8.0f : 10.0f);
      row->setPadding(Edge::All, static_cast<float>(metrics.compact ? 14 : 16));
      row->setThemedBackgroundColor(ui_theme::panelSubtle);
      row->setCornerRadius(ui_theme::controlRadius());
      row->setThemedBorderColor(ui_theme::hairline);
      row->setBorderWidth(1);

      auto *titleRow = new View();
      titleRow->setFlexDirection(FlexDirection::Row);
      titleRow->setFlexWrap(YGWrapWrap);
      titleRow->setGap(metrics.compact ? 8.0f : 12.0f);
      titleRow->setAlignItems(YGAlignCenter);
      titleRow->addView(makeWrappedText(table.name, metrics.bodyTextSize + 6,
                                        ui_theme::textPrimary()));
      titleRow->addView(
          makeText(table.symbol, metrics.bodyTextSize, ui_theme::cyan()));
      titleRow->addView(makeText(formatTableCount(table.chartCount),
                                 metrics.bodyTextSize,
                                 ui_theme::textSecondary()));
      row->addView(titleRow);

      row->addView(makeWrappedText(formatTableSource(table.sourceUrl),
                                   metrics.smallTextSize,
                                   ui_theme::textMuted()));

      auto *actions = new View();
      actions->setFlexDirection(FlexDirection::Row);
      actions->setFlexWrap(YGWrapWrap);
      actions->setGap(metrics.compact ? 8.0f : 10.0f);

      const int smallActionWidth = metrics.compact ? 136 : 156;
      auto *updateButton = makeControlButton(
          smallActionWidth, metrics.actionButtonHeight,
          makeText("Update", metrics.bodyTextSize + 2, ui_theme::textPrimary(),
                   TextView::CENTER, TextView::MIDDLE));
      updateButton->setOnClickListener([this, tableId = table.id]() {
        updateDifficultyTableFromSource(tableId);
      });
      actions->addView(updateButton);

      const bool confirmingDelete = pendingDeleteDifficultyTableId == table.id;
      auto *deleteButton = makeAccentButton(
          smallActionWidth, metrics.actionButtonHeight,
          makeText(confirmingDelete ? "Confirm" : "Delete",
                   metrics.bodyTextSize + 2, ui_theme::textPrimary(),
                   TextView::CENTER, TextView::MIDDLE),
          ui_theme::coral());
      deleteButton->setOnClickListener(
          [this, tableId = table.id]() { deleteDifficultyTable(tableId); });
      actions->addView(deleteButton);

      row->addView(actions);
      tableList->addView(row);
    }
  }

  auto *installedTablesBody = new View();
  installedTablesBody->setFlexDirection(FlexDirection::Column);
  installedTablesBody->setGap(metrics.compact ? 14.0f : 18.0f);
  installedTablesBody->setAlignSelf(YGAlignStretch);
  installedTablesBody->addView(addControls);
  installedTablesBody->addView(tableList);

  cardsColumn->addView(makeCard(metrics, "Installed Difficulty Tables",
                                tableCardDescription, installedTablesBody,
                                metrics.modeCardHeight, metrics.cardsWidth));
  return cardsColumn;
}

View *SettingsScene::buildBmsLibraryTab(const LayoutMetrics &metrics) {
  auto *cardsColumn = makeCardsColumn(metrics);
  loadChartEntries();
  refreshChartEntryBackupStatuses();

  auto *folderList = new View();
  folderList->setFlexDirection(FlexDirection::Column);
  folderList->setGap(metrics.compact ? 10.0f : 12.0f);

  auto *folderActions = new View();
  folderActions->setFlexDirection(FlexDirection::Row);
  folderActions->setFlexWrap(YGWrapWrap);
  folderActions->setGap(metrics.compact ? 8.0f : 10.0f);
  folderActions->setAlignItems(YGAlignFlexStart);

  auto *refreshFoldersButton = makeAccentButton(
      metrics.compact ? 150 : 170, metrics.actionButtonHeight,
      makeText("Refresh List", metrics.bodyTextSize + 2,
               ui_theme::textPrimary(), TextView::CENTER, TextView::MIDDLE),
      ui_theme::lime());
  refreshFoldersButton->setOnClickListener([this]() { refreshChartLibrary(); });
  folderActions->addView(refreshFoldersButton);

#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  auto *addFolderButton = makeAccentButton(
      metrics.compact ? 150 : 170, metrics.actionButtonHeight,
      makeText("Add Folder", metrics.bodyTextSize + 2,
               ui_theme::textPrimary(), TextView::CENTER, TextView::MIDDLE),
      ui_theme::cyan());
  addFolderButton->setOnClickListener([this]() {
    if (context.requestAddChartFolderFromFiles) {
      context.requestAddChartFolderFromFiles();
      chartFolderStatusMessage = "Choose a folder to add.";
      chartFolderStatusColor = ui_theme::sdl(ui_theme::textSecondary());
    } else {
      chartFolderStatusMessage = "Add Folder is unavailable.";
      chartFolderStatusColor = {255, 177, 170, 255};
    }

    if (chartFolderStatusText != nullptr) {
      chartFolderStatusText->setText(chartFolderStatusMessage);
      chartFolderStatusText->setColor(chartFolderStatusColor);
    }
  });
  folderActions->addView(addFolderButton);
#endif

  folderList->addView(folderActions);

  chartFolderStatusText = makeWrappedText(
      chartFolderStatusMessage, metrics.bodyTextSize,
      Color(chartFolderStatusColor.r, chartFolderStatusColor.g,
            chartFolderStatusColor.b, chartFolderStatusColor.a));
  folderList->addView(chartFolderStatusText);

  if (chartEntries.empty()) {
    folderList->addView(makeWrappedText("No chart folders are installed.",
                                        metrics.bodyTextSize,
                                        ui_theme::textSecondary()));
  } else {
    for (const auto &entry : chartEntries) {
      const std::string entryPathText = formatChartEntryPath(entry);

      auto *row = new View();
      row->setFlexDirection(FlexDirection::Column);
      row->setGap(metrics.compact ? 8.0f : 10.0f);
      row->setPadding(Edge::All, static_cast<float>(metrics.compact ? 14 : 16));
      row->setThemedBackgroundColor(ui_theme::panelSubtle);
      row->setCornerRadius(ui_theme::controlRadius());
      row->setThemedBorderColor(ui_theme::hairline);
      row->setBorderWidth(1);

      row->addView(makeWrappedText(formatChartEntryName(entry),
                                   metrics.bodyTextSize + 6,
                                   ui_theme::textPrimary()));
      row->addView(makeWrappedText(formatChartEntrySource(entry),
                                   metrics.smallTextSize,
                                   ui_theme::textMuted()));

      auto *actions = new View();
      actions->setFlexDirection(FlexDirection::Row);
      actions->setFlexWrap(YGWrapWrap);
      actions->setGap(metrics.compact ? 8.0f : 10.0f);

      const int folderActionWidth = metrics.compact ? 136 : 156;
      const bool confirmingDelete =
          pendingDeleteChartEntryPath == entryPathText;
      auto *deleteButton = makeAccentButton(
          folderActionWidth, metrics.actionButtonHeight,
          makeText(confirmingDelete ? "Confirm" : "Delete",
                   metrics.bodyTextSize + 2, ui_theme::textPrimary(),
                   TextView::CENTER, TextView::MIDDLE),
          ui_theme::coral());
      deleteButton->setOnClickListener(
          [this, entryPathText]() { deleteChartEntry(entryPathText); });
      actions->addView(deleteButton);

#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
      const auto backupStatusIt =
          chartEntryICloudBackupExcluded.find(entryPathText);
      const bool backupExcluded =
          backupStatusIt != chartEntryICloudBackupExcluded.end() &&
          backupStatusIt->second;
      const int backupActionWidth = metrics.compact ? 224 : 260;
      auto *backupButton = makeAccentButton(
          backupActionWidth, metrics.actionButtonHeight,
          makeText(backupExcluded ? "Enable iCloud Backup"
                                  : "Disable iCloud Backup",
                   metrics.smallTextSize, ui_theme::textPrimary(),
                   TextView::CENTER, TextView::MIDDLE),
          backupExcluded ? ui_theme::cyan() : ui_theme::lime());
      backupButton->setOnClickListener([this, entryPathText]() {
        toggleChartEntryICloudBackup(entryPathText);
      });
      actions->addView(backupButton);
#endif

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
  difficultyTableImportModalRoot->setThemedBackgroundColor(ui_theme::scrim);

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
      ->setThemedBackgroundColor(ui_theme::panelStrong)
      ->setCornerRadius(ui_theme::panelRadius())
      ->setThemedShadow(ui_theme::shadow, ui_theme::kModalShadow)
      ->setThemedBorderColor(ui_theme::hairline)
      ->setBorderWidth(1);

  difficultyTableImportTitleText =
      makeWrappedText("Importing Difficulty Tables", metrics.sectionTitleSize,
                      ui_theme::textPrimary());
  importPanel->addView(difficultyTableImportTitleText);

  difficultyTableImportStatusText = makeWrappedText(
      "Preparing import...", metrics.bodyTextSize, ui_theme::textSecondary());
  importPanel->addView(difficultyTableImportStatusText);

  difficultyTableImportTableText =
      makeWrappedText("Current table: Resolving table URL",
                      metrics.bodyTextSize, ui_theme::textPrimary());
  importPanel->addView(difficultyTableImportTableText);

  auto *progressRow = new View();
  progressRow->setFlexDirection(FlexDirection::Column);
  progressRow->setGap(metrics.compact ? 8.0f : 10.0f);
  difficultyTableImportProgressText =
      makeText("0 / 1 table", metrics.bodyTextSize, ui_theme::textMuted());
  progressRow->addView(difficultyTableImportProgressText);

  auto *progressTrack = new View();
  progressTrack->setHeight(static_cast<float>(metrics.compact ? 16 : 18));
  progressTrack->setAlignSelf(YGAlignStretch);
  progressTrack->setFlexDirection(FlexDirection::Row);
  progressTrack->setThemedBackgroundColor(ui_theme::control);
  progressTrack->setCornerRadius(ui_theme::controlRadius());
  progressTrack->setThemedBorderColor(ui_theme::hairline);
  progressTrack->setBorderWidth(1);
  difficultyTableImportProgressFill = new View();
  difficultyTableImportProgressFill->setWidthPercent(0.0f);
  difficultyTableImportProgressFill->setHeight(
      static_cast<float>(metrics.compact ? 16 : 18));
  difficultyTableImportProgressFill->setThemedBackgroundColor(
      ui_theme::progressFill);
  progressTrack->addView(difficultyTableImportProgressFill);
  progressRow->addView(progressTrack);
  importPanel->addView(progressRow);

  auto *modalActions = new View();
  modalActions->setFlexDirection(FlexDirection::Row);
  modalActions->setJustifyContent(YGJustifyFlexEnd);
  difficultyTableImportCloseButton = makeControlButton(
      160, 60,
      makeText("Close", metrics.bodyTextSize + 2, ui_theme::textPrimary(),
               TextView::CENTER, TextView::MIDDLE));
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
  addView(rootLayout);
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

  rootLayout->setThemedBackgroundColor(ui_theme::backdrop);

  auto *header = new View();
  header->setFlexDirection(FlexDirection::Row);
  header->setAlignItems(YGAlignCenter);
  header->setJustifyContent(YGJustifySpaceBetween);

  auto *headerText = new View();
  headerText->setFlexDirection(FlexDirection::Column);
  headerText->setGap(static_cast<float>(metrics.headerGap));
  headerText->addView(
      makeText("Settings", metrics.titleSize, ui_theme::textPrimary()));
  headerText->addView(makeWrappedText(
      metrics.compact
          ? "Timing, keysound, and visual preferences."
          : "Persistent player preferences for timing, keysounds, and visual "
            "load.",
      metrics.subtitleSize, ui_theme::textSecondary()));
  header->addView(headerText);

  auto *backLabel =
      makeText("Back", metrics.bodyTextSize + 6, ui_theme::textPrimary(),
               TextView::CENTER, TextView::MIDDLE);
  auto *backButton = makeButton(
      metrics.backButtonWidth, metrics.backButtonHeight, backLabel,
      ui_theme::control(), ui_theme::controlHover(), ui_theme::controlPressed(),
      ui_theme::hairline(), ui_theme::cyan(), ui_theme::cyan());
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
  auto makeTabButton = [&](SettingsTab tab, const std::string &label,
                           TextView **labelOut) {
    auto *labelText =
        makeText(label, metrics.bodyTextSize + 4, ui_theme::textPrimary(),
                 TextView::CENTER, TextView::MIDDLE);
    if (labelOut != nullptr) {
      *labelOut = labelText;
    }
    auto *button =
        makeButton(tabColumnWidth, metrics.actionButtonHeight, labelText,
                   ui_theme::control(), ui_theme::controlHover(),
                   ui_theme::controlPressed(), ui_theme::hairline(),
                   ui_theme::accentBorder(), ui_theme::accentBorderStrong());
    button->setOnClickListener([this, tab]() {
      if (activeTab == tab) {
        return;
      }
      activeTab = tab;
      lastLayoutWidth = -1;
    });
    return button;
  };
  timingTabButton =
      makeTabButton(SettingsTab::Timing, "Timing", &timingTabText);
  visualTabButton =
      makeTabButton(SettingsTab::Visual, "Visual", &visualTabText);
  laneTabButton = makeTabButton(SettingsTab::Lane, "Lane", &laneTabText);
  miscTabButton = makeTabButton(SettingsTab::Misc, "Misc", &miscTabText);
  difficultyTablesTabButton = makeTabButton(
      SettingsTab::DifficultyTables, "Difficulty Tables",
      &difficultyTablesTabText);
  bmsLibraryTabButton = makeTabButton(SettingsTab::BmsLibrary, "BMS Library",
                                      &bmsLibraryTabText);
  tabControls->addView(timingTabButton);
  tabControls->addView(visualTabButton);
  tabControls->addView(laneTabButton);
  tabControls->addView(miscTabButton);
  tabControls->addView(difficultyTablesTabButton);
  tabControls->addView(bmsLibraryTabButton);
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
  case SettingsTab::Misc:
    cardsColumn = buildMiscTab(metrics);
    break;
  case SettingsTab::DifficultyTables:
    cardsColumn = buildDifficultyTablesTab(metrics);
    break;
  case SettingsTab::BmsLibrary:
    cardsColumn = buildBmsLibraryTab(metrics);
    break;
  }
  if (cardsColumn != nullptr) {
    scrollContent->addView(cardsColumn);
  }

  auto *footer = new View();
  footer->setPadding(Edge::All, static_cast<float>(metrics.cardPadding - 4));
  footer->setThemedBackgroundColor(ui_theme::panelSubtle);
  footer->setCornerRadius(ui_theme::panelRadius());
  footer->setThemedBorderColor(ui_theme::hairline);
  footer->setBorderWidth(1);
  footer->addView(makeWrappedText(
      metrics.compact
          ? "Settings save automatically in the app documents directory."
          : "Settings are saved automatically in the app documents directory.",
      metrics.bodyTextSize, ui_theme::textMuted()));
  scrollContent->addView(footer);

  scrollView->setContentView(scrollContent);
  content->addView(scrollView);
  rootLayout->addView(content);

  buildDifficultyTableImportModal(metrics);

  rootLayout->applyYogaLayout();
  refreshDifficultyTableImportModal();
  refreshSettingsText();
}
