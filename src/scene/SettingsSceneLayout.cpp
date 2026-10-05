#include "SettingsSceneShared.h"
#include "../i18n/Localization.h"
#include "../i18n/PlatformLocale.h"
#include "../BmsSearchService.h"
#include "../input/InputCaptureController.h"
#include "../view/BlockingOverlayView.h"
#include "../view/DropdownView.h"
#include "../view/OverlayPortal.h"
#include "../view/ScrollView.h"
#include "play/BMSRenderer.h"
#include <charconv>
#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
#include "../iOSNatives.hpp"
#endif
#if TARGET_OS_ANDROID
#include "../AndroidNatives.h"
#endif

using namespace settings_scene;

namespace {
constexpr const char *kRepositoryUrl =
    "https://github.com/SNURhythm/AsoBMaShow";

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
  overlayPortal = nullptr;
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
  summaryJudgementIndicatorYValueText = nullptr;
  summaryJudgementIndicatorWidthValueText = nullptr;
  summaryJudgementIndicatorRangeValueText = nullptr;
  summaryJudgementCounterPositionValueText = nullptr;
  summaryJudgementTimingFastSlowValueText = nullptr;
  summaryJudgementTimingMillisecondsValueText = nullptr;
  summaryGaugeBarPositionValueText = nullptr;
  summaryNotePriorityValueText = nullptr;
  summaryUiThemeValueText = nullptr;
  judgementIndicatorYInput = nullptr;
  judgementIndicatorWidthInput = nullptr;
  judgementIndicatorRangeInput = nullptr;
  visibleTimeModeText = nullptr;
  keysoundModeText = nullptr;
  prepMetronomeModeText = nullptr;
  startLaneIndicatorsModeText = nullptr;
  ipadGestureReminderModeText = nullptr;
  showInvisibleNotesModeText = nullptr;
  markProcessedNotesModeText = nullptr;
  touchVisualizationModeText = nullptr;
  hispeedAutoAdjustModeText = nullptr;
  archiveChartPreviewModeText = nullptr;
  findBmsSkipUnarchivingModeText = nullptr;
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
  profileTabText = nullptr;
  profileStatusText = nullptr;
  profileCreateNameInput = nullptr;
  visibleTimeModeButton = nullptr;
  keysoundModeButton = nullptr;
  prepMetronomeModeButton = nullptr;
  startLaneIndicatorsModeButton = nullptr;
  showInvisibleNotesModeButton = nullptr;
  markProcessedNotesModeButton = nullptr;
  ipadGestureReminderModeButton = nullptr;
  touchVisualizationModeButton = nullptr;
  hispeedAutoAdjustModeButton = nullptr;
  archiveChartPreviewModeButton = nullptr;
  findBmsSkipUnarchivingModeButton = nullptr;
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
  profileTabButton = nullptr;
  timingTabButton = nullptr;
  visualTabButton = nullptr;
  laneTabButton = nullptr;
  inputTabButton = nullptr;
  miscTabButton = nullptr;
  audioTabButton = nullptr;
  displayTabButton = nullptr;
  difficultyTablesTabButton = nullptr;
  bmsLibraryTabButton = nullptr;
  gameplaySkinsTabButton = nullptr;
  irTabButton = nullptr;
  timingTabText = nullptr;
  visualTabText = nullptr;
  laneTabText = nullptr;
  inputTabText = nullptr;
  miscTabText = nullptr;
  audioTabText = nullptr;
  displayTabText = nullptr;
  difficultyTablesTabText = nullptr;
  bmsLibraryTabText = nullptr;
  gameplaySkinsTabText = nullptr;
  irTabText = nullptr;
  irPendingCountText = nullptr;
  irAwaitingCountText = nullptr;
  irBlockedCountText = nullptr;
  irFailedCountText = nullptr;
  irStatusText = nullptr;
  irServerOriginInput = nullptr;
  irApiKeyInput = nullptr;
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
  audioDeviceDropdown = nullptr;
  audioSampleRateDropdown = nullptr;
  audioBufferDropdown = nullptr;
  displayModeDropdown = nullptr;
  displayIndexDropdown = nullptr;
  displayResolutionDropdown = nullptr;
  displayVsyncDropdown = nullptr;
  displayFrameCapDropdown = nullptr;
  masterVolumeInput = nullptr;
  bgmVolumeInput = nullptr;
  keysoundVolumeInput = nullptr;
  audioEffectiveText = nullptr;
  audioStatusText = nullptr;
  displayStatusText = nullptr;
  displayPreviewOverlayRoot = nullptr;
  displayPreviewCountdownText = nullptr;
  displayPreviewStatusText = nullptr;
  displayPreviewKeepButton = nullptr;
  inputPlayerDropdown = nullptr;
  inputKeyModeDropdown = nullptr;
  inputDeviceDropdown = nullptr;
  inputMonitorText = nullptr;
  inputCaptureStateText = nullptr;
  inputErrorText = nullptr;
  inputConflictOverlayRoot = nullptr;
  inputVirtualControllerEditorOverlayRoot = nullptr;
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  gameplaySkinControlsBuiltDisabled = false;
  gameplaySkinStatusText = nullptr;
  gameplaySkinUiMessageText = nullptr;
  skinSelectSoundSetInput = nullptr;
  gameplaySkinConfigurationDigestText = nullptr;
  gameplaySkinSafetyOverlayRoot = nullptr;
  gameplaySkinBusyOverlayRoot = nullptr;
  gameplaySkinBusyOverlayStatusText = nullptr;
  gameplaySkinBusyOverlayCancelButton = nullptr;
#endif
}

void SettingsScene::ensureLayoutUpToDate() {
  const SafeAreaInsets safe = getSafeAreaInsetsUi();
  if (rendering::window_width == lastLayoutWidth &&
      rendering::window_height == lastLayoutHeight && safe.top == lastSafeTop &&
      safe.left == lastSafeLeft && safe.bottom == lastSafeBottom &&
      safe.right == lastSafeRight && rootLayout != nullptr) {
    return;
  }

  const bool preserveScroll = rootLayout != nullptr && scrollView != nullptr &&
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
        metrics.compact ? i18n::message("settings.visible_time_controls.note_duration.green_number_help")
                        : i18n::message("settings.visible_time_controls.green_number.unit_help"),
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
    syncPreviewPresentationConfiguration();
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

  if (visibleTimeModeRow != nullptr) {
    visibleTimeControls->addView(visibleTimeModeRow);
  }

  if (compactAdjustments) {
    auto *fixedHispeed = new DropdownView(
        {.onOptionSelected = [this](const std::string &id) {
          context.settings.hispeedFixMode =
              static_cast<AppSettings::HiSpeedFixMode>(std::stoi(id));
          persistSettings();
          syncPreviewPresentationConfiguration();
        }}, overlayPortal);
    std::vector<DropdownView::Option> modes;
    for (const auto mode :
         {AppSettings::HiSpeedFixMode::Off, AppSettings::HiSpeedFixMode::Start,
          AppSettings::HiSpeedFixMode::Max, AppSettings::HiSpeedFixMode::Main,
          AppSettings::HiSpeedFixMode::Min}) {
      modes.push_back({.id = std::to_string(static_cast<int>(mode)),
                       .label = formatVisibleTimeBpmStrategyLabel(mode)});
    }
    visibleTimeControls->addView(makeWrappedText(
        i18n::message("settings.visible_time_controls.fixed_hi_speed.label"),
        metrics.smallTextSize, ui_theme::textSecondary()));
    fixedHispeed->refresh({.selectedId = std::to_string(static_cast<int>(context.settings.hispeedFixMode)),
                           .options = std::move(modes), .maxVisibleItems = 5});
    fixedHispeed->setWidthPercent(100)->setMinWidth(0);
    fixedHispeed->setHeight(metrics.actionButtonHeight);
    visibleTimeControls->addView(fixedHispeed);
  } else {
    auto *fixedHispeedChoices = new View();
    fixedHispeedChoices->setFlexDirection(FlexDirection::Row);
    fixedHispeedChoices->setFlexWrap(YGWrapWrap);
    fixedHispeedChoices->setAlignItems(YGAlignCenter);
    fixedHispeedChoices->setGap(metrics.compact ? 6.0F : 8.0F);
    auto *fixedHispeedLabel =
        makeText(i18n::message("settings.visible_time_controls.fixed_hi_speed.label"), metrics.smallTextSize,
                 ui_theme::textSecondary(), TextView::LEFT, TextView::MIDDLE);
    fixedHispeedLabel->setMinWidth(0.0F);
    fixedHispeedLabel->setFlexShrink(1.0F);
    fixedHispeedChoices->addView(fixedHispeedLabel);
    for (const auto mode :
         {AppSettings::HiSpeedFixMode::Off, AppSettings::HiSpeedFixMode::Start,
          AppSettings::HiSpeedFixMode::Max, AppSettings::HiSpeedFixMode::Main,
          AppSettings::HiSpeedFixMode::Min}) {
      auto *choiceLabel = makeText(formatVisibleTimeBpmStrategyLabel(mode),
                                   metrics.smallTextSize, ui_theme::textPrimary(),
                                   TextView::CENTER, TextView::MIDDLE);
      const int width =
          std::max(metrics.compact ? 84 : 96, choiceLabel->textureWidth() + 28);
      auto *choice =
          context.settings.hispeedFixMode == mode
              ? makeAccentButton(width, metrics.actionButtonHeight,
                                 choiceLabel,
                                 ui_theme::cyan())
              : makeControlButton(width, metrics.actionButtonHeight, choiceLabel);
      choice->setOnClickListener([this, mode]() {
        if (context.settings.hispeedFixMode == mode) {
          return;
        }
        context.settings.hispeedFixMode = mode;
        persistSettings();
        syncPreviewPresentationConfiguration();
        lastLayoutWidth = -1;
      });
      fixedHispeedChoices->addView(choice);
    }
    visibleTimeControls->addView(fixedHispeedChoices);
  }
  auto *visibleTimeValueControls = new View();
  visibleTimeValueControls->setFlexDirection(FlexDirection::Row);
  visibleTimeValueControls->setFlexWrap(YGWrapWrap);
  visibleTimeValueControls->setGap(metrics.compact ? 8.0f : 12.0f);
  visibleTimeValueControls->setAlignItems(
      compactAdjustments ? YGAlignCenter : YGAlignFlexStart);
  if (compactAdjustments) {
    visibleTimeValueControls->setWidthPercent(100.0f);
    visibleTimeValueControls->setJustifyContent(YGJustifyCenter);
  }

  auto updateVisibleTime = [this](int delta) {
    context.settings.visibleTimeDurationMilliseconds =
        adjustVisibleTimeDurationMilliseconds(
        context.settings.visibleTimeDurationMilliseconds,
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
      context.settings.setVisibleTimeGreenNumber(400);
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
      foldButtonSize, rendering::window_height - metrics.safe.top -
                          metrics.safe.bottom - metrics.verticalPadding * 2);
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

  auto makeFoldButton = [this, foldButtonSize](const i18n::Text &label) {
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
    previewPanel->addView(makeFoldButton(i18n::message("settings.preview_layout.open.label")));
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
      makeText(i18n::message("settings.preview_layout.preview.label"), metrics.sectionTitleSize, ui_theme::textPrimary()));

  previewHeader->addView(makeFoldButton(i18n::message("settings.preview_layout.hide.label")));
  previewPanel->addView(previewHeader);

  auto *previewTabs = new View();
  previewTabs->setFlexDirection(FlexDirection::Row);
  previewTabs->setGap(metrics.compact ? 8.0f : 10.0f);
  previewTabs->setAlignItems(YGAlignCenter);
  previewTabs->setWidthPercent(100.0f);
  previewTabs->setJustifyContent(YGJustifyCenter);
  const int previewTabGap = metrics.compact ? 8 : 10;
  const int previewTabWidth =
      std::max(0, (panelWidth - metrics.cardPadding * 2 -
                   previewTabGap * (previewPanelPageCount - 1)) /
                      previewPanelPageCount);
  auto makePreviewTab = [this, &metrics, previewTabWidth](int page,
                                                          const i18n::Text &label) {
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
  previewTabs->addView(makePreviewTab(0, i18n::message("settings.preview_layout.scroll.label")));
  previewTabs->addView(makePreviewTab(1, i18n::message("settings.preview_layout.lane.label")));
  previewTabs->addView(makePreviewTab(2, "HUD"));
  previewPanel->addView(previewTabs);

  auto *previewScroll = new ScrollView();
  previewScroll->setFlex(1.0f);
  previewScroll->setFlexShrink(1.0f);
  previewScroll->setWidthPercent(100.0f);
  previewScroll->setContentPadding(Edge::Right,
                                   metrics.compact ? 10.0f : 12.0f);

  auto *previewControls = new View();
  previewControls->setFlexDirection(FlexDirection::Column);
  previewControls->setGap(metrics.compact ? 12.0f : 16.0f);
  previewControls->setAlignItems(YGAlignStretch);
  previewScroll->setContentView(previewControls);
  previewPanel->addView(previewScroll);

  if (previewPanelPage == 0) {
    previewControls->addView(
        makeSummaryRow(metrics, i18n::message("settings.preview_layout.visible_time.label"), &summaryVisibleTimeValueText));
    previewControls->addView(buildVisibleTimeControls(metrics, false, true));

    previewControls->addView(makeSummaryRow(
        metrics, i18n::message("settings.preview_layout.note_start.label"), &summaryNoteStartPositionValueText));
    auto *noteStartControls = new View();
    noteStartControls->setFlexDirection(FlexDirection::Row);
    noteStartControls->setFlexWrap(YGWrapWrap);
    noteStartControls->setGap(metrics.compact ? 8.0f : 10.0f);
    noteStartControls->setAlignItems(YGAlignCenter);
    noteStartControls->setWidthPercent(100.0f);
    noteStartControls->setJustifyContent(YGJustifyCenter);
    auto updateNoteStartPosition = [this](int deltaPercent) {
      context.settings.presentation().noteStartPositionPercent = clampNoteStartPositionPercent(
          context.settings.presentation().noteStartPositionPercent + deltaPercent);
      persistSettings();
    };
    auto *minusNoteStart =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "-5%");
    minusNoteStart->setOnClickListener(
        [updateNoteStartPosition]() { updateNoteStartPosition(-5); });
    noteStartControls->addView(minusNoteStart);
    auto *plusNoteStart =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "+5%");
    plusNoteStart->setOnClickListener(
        [updateNoteStartPosition]() { updateNoteStartPosition(5); });
    noteStartControls->addView(plusNoteStart);
    auto *resetNoteStart = makeResetButton(metrics);
    resetNoteStart->setOnClickListener([this]() {
      context.settings.presentation().noteStartPositionPercent =
          AppSettings::kDefaultNoteStartPositionPercent;
      persistSettings();
    });
    noteStartControls->addView(resetNoteStart);
    previewControls->addView(noteStartControls);
  } else if (previewPanelPage == 1) {
    previewControls->addView(buildScratchLanePositionControl(metrics));
    previewControls->addView(
        makeSummaryRow(metrics, i18n::message("settings.preview_layout.lane_angle.label"), &summaryLaneAngleValueText));
    auto *angleControls = new View();
    angleControls->setFlexDirection(FlexDirection::Row);
    angleControls->setFlexWrap(YGWrapWrap);
    angleControls->setGap(metrics.compact ? 8.0f : 10.0f);
    angleControls->setAlignItems(YGAlignCenter);
    angleControls->setWidthPercent(100.0f);
    angleControls->setJustifyContent(YGJustifyCenter);
    auto updateLaneAngle = [this](float delta) {
      context.settings.presentation().laneAngleDegrees =
          clampLaneAngle(context.settings, context.settings.presentation().laneAngleDegrees + delta);
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
      context.settings.presentation().laneAngleDegrees = context.settings.geometryPolicy().angle.defaultValue;
      persistSettings();
    });
    angleControls->addView(resetAngle);
    previewControls->addView(angleControls);

    previewControls->addView(
        makeSummaryRow(metrics, i18n::message("settings.preview_layout.lane_length.label"), &summaryLaneLengthValueText));
    auto *lengthControls = new View();
    lengthControls->setFlexDirection(FlexDirection::Row);
    lengthControls->setFlexWrap(YGWrapWrap);
    lengthControls->setGap(metrics.compact ? 8.0f : 10.0f);
    lengthControls->setAlignItems(YGAlignCenter);
    lengthControls->setWidthPercent(100.0f);
    lengthControls->setJustifyContent(YGJustifyCenter);
    auto updateLaneLength = [this](float delta) {
      context.settings.presentation().laneLength =
          clampLaneLength(context.settings, context.settings.presentation().laneLength + delta);
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
      context.settings.presentation().laneLength = context.settings.geometryPolicy().length.defaultValue;
      persistSettings();
    });
    lengthControls->addView(resetLength);
    previewControls->addView(lengthControls);

    previewControls->addView(makeSummaryRow(metrics, i18n::message("settings.preview_layout.beam_length.label"),
                                            &summaryLaneBeamLengthValueText));
    auto *beamControls = new View();
    beamControls->setFlexDirection(FlexDirection::Row);
    beamControls->setFlexWrap(YGWrapWrap);
    beamControls->setGap(metrics.compact ? 8.0f : 10.0f);
    beamControls->setAlignItems(YGAlignCenter);
    beamControls->setWidthPercent(100.0f);
    beamControls->setJustifyContent(YGJustifyCenter);
    auto updateLaneBeamLength = [this](int deltaPercent) {
      context.settings.presentation().laneBeamLengthPercent = clampLaneBeamLengthPercent(
          context.settings.presentation().laneBeamLengthPercent + deltaPercent);
      persistSettings();
    };
    auto *minusBeam =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "-5%");
    minusBeam->setOnClickListener(
        [updateLaneBeamLength]() { updateLaneBeamLength(-5); });
    beamControls->addView(minusBeam);
    auto *plusBeam =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "+5%");
    plusBeam->setOnClickListener(
        [updateLaneBeamLength]() { updateLaneBeamLength(5); });
    beamControls->addView(plusBeam);
    auto *resetBeam = makeResetButton(metrics);
    resetBeam->setOnClickListener([this]() {
      context.settings.presentation().laneBeamLengthPercent =
          AppSettings::kDefaultLaneBeamLengthPercent;
      persistSettings();
    });
    beamControls->addView(resetBeam);
    previewControls->addView(beamControls);

    previewControls->addView(makeSummaryRow(
        metrics, i18n::message("settings.preview_layout.play_width_7_k.label"), &summaryPreviewPlayAreaWidthValueText));
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
          clampPlayAreaWidth(context.settings,
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
          7, context.settings.geometryPolicy().width.defaultValue);
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

    previewControls->addView(buildJudgementFeedbackPositionControls(metrics, true));
    previewControls->addView(buildJudgementFeedbackStyleControls(metrics, true));

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
        makeControlButton(metrics.actionButtonWidth, metrics.actionButtonHeight,
                          judgementTimingFastSlowCriteriaText);
    judgementTimingFastSlowCriteriaButton->setOnClickListener([this]() {
      context.settings.presentation().judgementTimingFastSlowCriteria =
          nextJudgementTimingDisplayCriteria(
              context.settings.presentation().judgementTimingFastSlowCriteria);
      persistSettings();
    });
    timingFastSlowControls->addView(judgementTimingFastSlowCriteriaButton);
    previewControls->addView(timingFastSlowControls);

    previewControls->addView(makeSummaryRow(
        metrics, i18n::message("settings.preview_layout.milliseconds.label"), &summaryJudgementTimingMillisecondsValueText));
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
        makeControlButton(metrics.actionButtonWidth, metrics.actionButtonHeight,
                          judgementTimingMillisecondsCriteriaText);
    judgementTimingMillisecondsCriteriaButton->setOnClickListener([this]() {
      context.settings.presentation().judgementTimingMillisecondsCriteria =
          nextJudgementTimingDisplayCriteria(
              context.settings.presentation().judgementTimingMillisecondsCriteria);
      persistSettings();
    });
    timingMillisecondsControls->addView(
        judgementTimingMillisecondsCriteriaButton);
    previewControls->addView(timingMillisecondsControls);

    previewControls->addView(makeText(i18n::message("settings.preview_layout.indicator.label"), metrics.summaryValueSize,
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
      context.settings.presentation().judgementIndicatorEnabled =
          !context.settings.presentation().judgementIndicatorEnabled;
      persistSettings();
    });
    indicatorModeControls->addView(judgementIndicatorModeButton);
    judgementIndicatorRenderModeText =
        makeText("", metrics.bodyTextSize + 4, ui_theme::textPrimary(),
                 TextView::CENTER, TextView::MIDDLE);
    judgementIndicatorRenderModeButton =
        makeControlButton(metrics.actionButtonWidth, metrics.actionButtonHeight,
                          judgementIndicatorRenderModeText);
    judgementIndicatorRenderModeButton->setOnClickListener([this]() {
      context.settings.presentation().judgementIndicatorRenderMode =
          nextJudgementIndicatorRenderMode(
              context.settings.presentation().judgementIndicatorRenderMode);
      persistSettings();
    });
    indicatorModeControls->addView(judgementIndicatorRenderModeButton);
    previewControls->addView(indicatorModeControls);

    previewControls->addView(makeSummaryRow(
        metrics, i18n::message("settings.preview_layout.indicator_y.label"), &summaryJudgementIndicatorYValueText));
    auto updateIndicatorY = [this](int deltaPercent) {
      const int currentPercent =
          judgementIndicatorYToPercent(context.settings.presentation().judgementIndicatorY);
      const int nextPercent = std::clamp(currentPercent + deltaPercent, 0, 100);
      context.settings.presentation().judgementIndicatorY =
          judgementIndicatorPercentToY(nextPercent);
      persistSettings();
    };
    auto *minusIndicatorY =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "-5%");
    minusIndicatorY->setOnClickListener(
        [updateIndicatorY]() { updateIndicatorY(-5); });
    auto *plusIndicatorY =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "+5%");
    plusIndicatorY->setOnClickListener(
        [updateIndicatorY]() { updateIndicatorY(5); });
    auto *resetIndicatorY = makeResetButton(metrics);
    resetIndicatorY->setOnClickListener([this]() {
      context.settings.presentation().judgementIndicatorY =
          AppSettings::kDefaultJudgementIndicatorY;
      persistSettings();
    });
    previewControls->addView(
        makePreviewStepRow(minusIndicatorY, plusIndicatorY, resetIndicatorY));

    previewControls->addView(makeSummaryRow(
        metrics, i18n::message("settings.preview_layout.indicator_width.label"), &summaryJudgementIndicatorWidthValueText));
    auto updateIndicatorWidth = [this](int deltaPercent) {
      const int currentPercent = judgementIndicatorWidthScaleToPercent(
          context.settings.presentation().judgementIndicatorWidthScale);
      const int minPercent = judgementIndicatorWidthScaleToPercent(
          AppSettings::kMinJudgementIndicatorWidthScale);
      const int maxPercent = judgementIndicatorWidthScaleToPercent(
          AppSettings::kMaxJudgementIndicatorWidthScale);
      const int nextPercent =
          std::clamp(currentPercent + deltaPercent, minPercent, maxPercent);
      context.settings.presentation().judgementIndicatorWidthScale =
          judgementIndicatorWidthPercentToScale(nextPercent);
      persistSettings();
    };
    auto *minusIndicatorWidth =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "-5%");
    minusIndicatorWidth->setOnClickListener(
        [updateIndicatorWidth]() { updateIndicatorWidth(-5); });
    auto *plusIndicatorWidth =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "+5%");
    plusIndicatorWidth->setOnClickListener(
        [updateIndicatorWidth]() { updateIndicatorWidth(5); });
    auto *resetIndicatorWidth = makeResetButton(metrics);
    resetIndicatorWidth->setOnClickListener([this]() {
      context.settings.presentation().judgementIndicatorWidthScale =
          AppSettings::kDefaultJudgementIndicatorWidthScale;
      persistSettings();
    });
    previewControls->addView(makePreviewStepRow(
        minusIndicatorWidth, plusIndicatorWidth, resetIndicatorWidth));

    previewControls->addView(makeSummaryRow(
        metrics, i18n::message("settings.preview_layout.indicator_range.label"),
        &summaryJudgementIndicatorRangeValueText));
    auto updateIndicatorRange = [this](int deltaMilliseconds) {
      context.settings.presentation().judgementIndicatorRangeMilliseconds =
          clampJudgementIndicatorRangeMilliseconds(
              context.settings.presentation().judgementIndicatorRangeMilliseconds +
              deltaMilliseconds);
      persistSettings();
    };
    auto *minusIndicatorRange =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "-10 ms");
    minusIndicatorRange->setOnClickListener(
        [updateIndicatorRange]() { updateIndicatorRange(-10); });
    auto *plusIndicatorRange =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "+10 ms");
    plusIndicatorRange->setOnClickListener(
        [updateIndicatorRange]() { updateIndicatorRange(10); });
    auto *resetIndicatorRange = makeResetButton(metrics);
    resetIndicatorRange->setOnClickListener([this]() {
      context.settings.presentation().judgementIndicatorRangeMilliseconds =
          AppSettings::kDefaultJudgementIndicatorRangeMilliseconds;
      persistSettings();
    });
    previewControls->addView(makePreviewStepRow(
        minusIndicatorRange, plusIndicatorRange, resetIndicatorRange));

    previewControls->addView(makeSummaryRow(
        metrics, i18n::message("settings.preview_layout.counter.label"), &summaryJudgementCounterPositionValueText));
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
      context.settings.presentation().judgementCounterEnabled =
          !context.settings.presentation().judgementCounterEnabled;
      persistSettings();
    });
    counterControls->addView(judgementCounterModeButton);
    judgementCounterPositionText =
        makeText("", metrics.bodyTextSize + 4, ui_theme::textPrimary(),
                 TextView::CENTER, TextView::MIDDLE);
    judgementCounterPositionButton =
        makeControlButton(metrics.actionButtonWidth, metrics.actionButtonHeight,
                          judgementCounterPositionText);
    judgementCounterPositionButton->setOnClickListener([this]() {
      context.settings.presentation().judgementCounterPosition = nextJudgementCounterPosition(
          context.settings.presentation().judgementCounterPosition);
      persistSettings();
    });
    counterControls->addView(judgementCounterPositionButton);
    previewControls->addView(counterControls);

    previewControls->addView(
        makeSummaryRow(metrics, i18n::message("settings.preview_layout.gauge.label"), &summaryGaugeBarPositionValueText));
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
        makeControlButton(metrics.actionButtonWidth, metrics.actionButtonHeight,
                          gaugeBarPositionText);
    gaugeBarPositionButton->setOnClickListener([this]() {
      context.settings.presentation().gaugeBarPosition =
          nextGaugeBarPosition(context.settings.presentation().gaugeBarPosition);
      persistSettings();
    });
    gaugeControls->addView(gaugeBarPositionButton);
    previewControls->addView(gaugeControls);
  }

  auto *restartButton = makeButton(
      metrics.actionButtonWidth, metrics.actionButtonHeight,
      makeText(i18n::message("settings.preview_layout.restart.label"), metrics.bodyTextSize + 4, ui_theme::textPrimary(),
               TextView::CENTER, TextView::MIDDLE),
      ui_theme::control(), ui_theme::controlHover(), ui_theme::controlPressed(),
      ui_theme::hairline(), ui_theme::cyan(), ui_theme::cyan());
  restartButton->setOnClickListener([this]() { resetPreviewSimulation(); });

  auto *doneButton = makeButton(
      metrics.actionButtonWidth, metrics.actionButtonHeight,
      makeText(i18n::message("settings.preview_layout.done.label"), metrics.bodyTextSize + 4, ui_theme::textPrimary(),
               TextView::CENTER, TextView::MIDDLE),
      ui_theme::control(), ui_theme::controlHover(), ui_theme::controlPressed(),
      ui_theme::hairline(), ui_theme::cyan(), ui_theme::cyan());
  doneButton->setOnClickListener([this]() { stopLanePreview(); });

  auto *previewActions = new View();
  previewActions->setFlexDirection(FlexDirection::Row);
  for (auto *button : {restartButton, doneButton}) {
    button->setWidth(0)->setMinWidth(0)->setFlex(1);
  }
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

View *SettingsScene::buildScratchLanePositionControl(const LayoutMetrics &metrics) {
  auto *row = new View();
  row->setWidthPercent(100);
  row->setFlexDirection(FlexDirection::Row);
  row->setAlignItems(YGAlignCenter);
  row->setGap(12);
  auto *label = makeWrappedText(i18n::message("settings.skins.scratch_position.label"),
                                metrics.smallTextSize, ui_theme::textSecondary());
  label->setFlex(1)->setMinWidth(0);
  row->addView(label);
  auto *value = makeText("", metrics.bodyTextSize, ui_theme::textPrimary(),
                         TextView::CENTER, TextView::MIDDLE);
  const auto refresh = [this, value]() {
    value->setLocalizedText(i18n::message(context.settings.presentation().scratchLaneOnRight
        ? "settings.skins.right.label" : "settings.skins.left.label"));
  };
  refresh();
  auto *toggle = makeControlButton(metrics.compact ? 132 : 156,
                                   metrics.actionButtonHeight, value);
  toggle->setOnClickListener([this, refresh]() {
    auto &enabled = context.settings.presentation().scratchLaneOnRight;
    enabled = !enabled;
    refresh();
    persistSettings();
  });
  row->addView(toggle);
  return row;
}

View *SettingsScene::buildJudgementFeedbackPositionControls(const LayoutMetrics &metrics,
                                                            bool previewStyle) {
  auto *body = new View();
  body->setFlexDirection(FlexDirection::Column);
  body->setWidthPercent(100);
  body->setGap(metrics.compact ? 12.0f : 16.0f);
  const auto appendPosition = [this, body, &metrics, previewStyle](const i18n::Text &label,
      float AppSettings::PresentationSettings::*member, float defaultValue) {
    TextView *valueText = nullptr;
    body->addView(makeSummaryRow(metrics, label, &valueText));
    auto *row = new View();
    row->setWidthPercent(100);
    row->setFlexDirection(FlexDirection::Row);
    row->setFlexWrap(YGWrapWrap);
    row->setAlignItems(YGAlignCenter);
    row->setGap(metrics.compact ? 8.0f : 10.0f);
    if (previewStyle) row->setJustifyContent(YGJustifyCenter);
    auto *input = previewStyle ? nullptr
        : makeTextInput(metrics, metrics.compact ? 116 : 136);
    const auto refresh = [this, valueText, input, member]() {
      const auto value = std::to_string(judgementTextYToPercent(context.settings.presentation().*member));
      valueText->setText(value + "%");
      if (input) input->setEditingText(value);
    };
    refresh();
    if (previewStyle) {
      for (const int delta : {-5, 5}) {
        auto *step = makeStepButton(metrics, metrics.offsetButtonWidthSmall,
                                    delta < 0 ? "-5%" : "+5%");
        step->setOnClickListener([this, member, delta, refresh]() {
          auto &value = context.settings.presentation().*member;
          value = judgementTextPercentToY(judgementTextYToPercent(value) + delta);
          refresh();
          persistSettings();
        });
        row->addView(step);
      }
    } else {
      input->onEditingFinished([this, input, member, refresh](const std::string &) {
        auto &value = context.settings.presentation().*member;
        const auto &text = input->getText();
        int parsed = judgementTextYToPercent(value);
        const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
        if (result.ec == std::errc{} && result.ptr == text.data() + text.size())
          value = judgementTextPercentToY(parsed);
        refresh();
        persistSettings();
      });
      row->addView(input);
    }
    auto *reset = makeResetButton(metrics);
    reset->setOnClickListener([this, member, defaultValue, refresh]() {
      context.settings.presentation().*member = defaultValue;
      refresh();
      persistSettings();
    });
    row->addView(reset);
    body->addView(row);
  };
  appendPosition(i18n::message("settings.preview_layout.judge_text_y.label"),
                 &AppSettings::PresentationSettings::judgementTextY, AppSettings::kDefaultJudgementTextY);
  appendPosition(i18n::message("settings.feedback.timing_y.label"),
                 &AppSettings::PresentationSettings::judgementTimingY, AppSettings::kDefaultJudgementTimingY);
  appendPosition(i18n::message("settings.feedback.pacemaker_y.label"),
                 &AppSettings::PresentationSettings::pacemakerDiffY, AppSettings::kDefaultPacemakerDiffY);
  return body;
}

View *SettingsScene::buildJudgementFeedbackStyleControls(const LayoutMetrics &metrics,
                                                         bool previewStyle) {
  auto *body = new View();
  body->setFlexDirection(FlexDirection::Column);
  body->setWidthPercent(100);
  body->setGap(previewStyle ? (metrics.compact ? 12.0f : 16.0f)
                            : (metrics.compact ? 8.0f : 12.0f));
  const auto appendStyle = [this, body, &metrics, previewStyle](const i18n::Text &label,
      int AppSettings::PresentationSettings::*sizeMember,
      bool AppSettings::PresentationSettings::*boldMember) {
    TextView *sizeText = nullptr;
    auto *input = previewStyle ? nullptr
        : makeTextInput(metrics, metrics.compact ? 116 : 136);
    if (previewStyle) {
      body->addView(makeSummaryRow(metrics, label, &sizeText));
    } else {
      auto *heading = makeText(label, metrics.smallTextSize, ui_theme::textSecondary(),
                               TextView::LEFT, TextView::MIDDLE);
      heading->setWidthPercent(100);
      heading->setWrap(true);
      body->addView(heading);
    }
    const auto refreshSize = [this, input, sizeText, sizeMember]() {
      const auto value = std::to_string(context.settings.presentation().*sizeMember);
      if (input) input->setEditingText(value);
      if (sizeText) sizeText->setText(value + "%");
    };
    refreshSize();
    auto *row = new View();
    row->setWidthPercent(100);
    row->setFlexDirection(FlexDirection::Row);
    row->setFlexWrap(YGWrapWrap);
    row->setAlignItems(YGAlignCenter);
    row->setGap(previewStyle ? (metrics.compact ? 8.0f : 10.0f) : 8.0f);
    if (previewStyle) {
      row->setJustifyContent(YGJustifyCenter);
      for (const int delta : {-5, 5}) {
        auto *step = makeStepButton(metrics, metrics.offsetButtonWidthSmall,
                                    delta < 0 ? "-5%" : "+5%");
        step->setOnClickListener([this, sizeMember, delta, refreshSize]() {
          auto &value = context.settings.presentation().*sizeMember;
          value = std::clamp(value + delta, AppSettings::kMinJudgementFeedbackSizePercent,
                             AppSettings::kMaxJudgementFeedbackSizePercent);
          refreshSize();
          persistSettings();
        });
        row->addView(step);
      }
    } else {
      input->onEditingFinished([this, input, sizeMember, refreshSize](const std::string &) {
        auto &value = context.settings.presentation().*sizeMember;
        const auto &text = input->getText();
        int parsed = value;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
        if (result.ec == std::errc{} && result.ptr == text.data() + text.size()) {
          value = std::clamp(parsed, AppSettings::kMinJudgementFeedbackSizePercent,
                             AppSettings::kMaxJudgementFeedbackSizePercent);
        }
        refreshSize();
        persistSettings();
      });
      row->addView(input);
    }
    auto *weightText = makeText("", metrics.bodyTextSize, ui_theme::textPrimary(),
                                TextView::CENTER, TextView::MIDDLE);
    const auto refreshWeight = [this, weightText, boldMember]() {
      weightText->setLocalizedText(i18n::message(context.settings.presentation().*boldMember
          ? "settings.skins.feedback.bold.label" : "settings.skins.feedback.regular.label"));
    };
    refreshWeight();
    auto *weight = makeControlButton(previewStyle ? metrics.actionButtonWidth : (metrics.compact ? 132 : 156),
                                     metrics.actionButtonHeight, weightText);
    weight->setOnClickListener([this, boldMember, refreshWeight]() {
      auto &bold = context.settings.presentation().*boldMember;
      bold = !bold;
      refreshWeight();
      persistSettings();
    });
    if (!previewStyle) row->addView(weight);
    auto *reset = makeResetButton(metrics);
    reset->setOnClickListener([this, sizeMember, boldMember, refreshSize, refreshWeight]() {
      context.settings.presentation().*sizeMember = AppSettings::kDefaultJudgementFeedbackSizePercent;
      context.settings.presentation().*boldMember = true;
      refreshSize();
      refreshWeight();
      persistSettings();
    });
    row->addView(reset);
    body->addView(row);
    if (previewStyle) {
      auto *weightRow = new View();
      weightRow->setFlexDirection(FlexDirection::Row);
      weightRow->setJustifyContent(YGJustifyCenter);
      weightRow->setWidthPercent(100);
      weightRow->addView(weight);
      body->addView(weightRow);
    }
  };
  appendStyle(i18n::message("settings.skins.feedback.judgement_size.percent_label"),
              &AppSettings::PresentationSettings::judgementTextSizePercent,
              &AppSettings::PresentationSettings::judgementTextBold);
  appendStyle(i18n::message("settings.skins.feedback.timing_size.percent_label"),
              &AppSettings::PresentationSettings::judgementTimingSizePercent,
              &AppSettings::PresentationSettings::judgementTimingBold);
  appendStyle(i18n::message("settings.skins.feedback.pacemaker_size.percent_label"),
              &AppSettings::PresentationSettings::pacemakerDiffSizePercent,
              &AppSettings::PresentationSettings::pacemakerDiffBold);
  return body;
}

View *SettingsScene::buildTimingTab(const LayoutMetrics &metrics) {
  auto *cardsColumn = makeCardsColumn(metrics);
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  const bool showLegacyBuiltInGameplayControls =
      !gameplaySkinTraitsRuntimeAvailable();
#else
  constexpr bool showLegacyBuiltInGameplayControls = true;
#endif
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
      makeCard(metrics, i18n::message("settings.timing.audio_offset.label"), i18n::message("settings.timing.negative_values_make_audio_earlier.message"),
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
      metrics, i18n::message("settings.timing.visual_offset.label"), i18n::message("settings.timing.move_notes_without_changing_audio_bga.message"),
      visualOffsetControls, metrics.offsetCardHeight, metrics.cardsWidth));

  if (showLegacyBuiltInGameplayControls) {
  auto *judgementFeedbackControls = new View();
  judgementFeedbackControls->setFlexDirection(FlexDirection::Column);
  judgementFeedbackControls->setGap(metrics.compact ? 12.0f : 16.0f);
  judgementFeedbackControls->setAlignItems(YGAlignFlexStart);
  judgementFeedbackControls->addView(buildJudgementFeedbackPositionControls(metrics));
  judgementFeedbackControls->addView(buildJudgementFeedbackStyleControls(metrics));

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
    context.settings.presentation().judgementTimingFastSlowCriteria =
        nextJudgementTimingDisplayCriteria(
            context.settings.presentation().judgementTimingFastSlowCriteria);
    persistSettings();
  });
  timingFastSlowGroup->addView(judgementTimingFastSlowCriteriaButton);
  timingCriteriaControls->addView(timingFastSlowGroup);

  auto *timingMillisecondsGroup = new View();
  timingMillisecondsGroup->setFlexDirection(FlexDirection::Column);
  timingMillisecondsGroup->setGap(metrics.compact ? 6.0f : 8.0f);
  timingMillisecondsGroup->addView(makeText(
      i18n::message("settings.timing.milliseconds.label"), metrics.bodyTextSize, ui_theme::textSecondary()));
  judgementTimingMillisecondsCriteriaText =
      makeText("", metrics.bodyTextSize + 4, ui_theme::textPrimary(),
               TextView::CENTER, TextView::MIDDLE);
  judgementTimingMillisecondsCriteriaButton =
      makeControlButton(metrics.actionButtonWidth, metrics.actionButtonHeight,
                        judgementTimingMillisecondsCriteriaText);
  judgementTimingMillisecondsCriteriaButton->setOnClickListener([this]() {
    context.settings.presentation().judgementTimingMillisecondsCriteria =
        nextJudgementTimingDisplayCriteria(
            context.settings.presentation().judgementTimingMillisecondsCriteria);
    persistSettings();
  });
  timingMillisecondsGroup->addView(judgementTimingMillisecondsCriteriaButton);
  timingCriteriaControls->addView(timingMillisecondsGroup);
  judgementFeedbackControls->addView(timingCriteriaControls);

  cardsColumn->addView(makeCard(
      metrics, i18n::message("settings.timing.judgement_feedback.label"), i18n::message("settings.timing.position_text_timing_details.message"),
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
    context.settings.presentation().judgementIndicatorEnabled =
        !context.settings.presentation().judgementIndicatorEnabled;
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
    context.settings.presentation().judgementIndicatorRenderMode =
        nextJudgementIndicatorRenderMode(
            context.settings.presentation().judgementIndicatorRenderMode);
    persistSettings();
  });
  judgementIndicatorModeControls->addView(judgementIndicatorRenderModeButton);
  judgementIndicatorControls->addView(judgementIndicatorModeControls);

  judgementIndicatorControls->addView(
      makeText(i18n::message("settings.timing.y_position.label"), metrics.bodyTextSize, ui_theme::textSecondary()));
  auto *judgementIndicatorYControls = new View();
  judgementIndicatorYControls->setFlexDirection(FlexDirection::Row);
  judgementIndicatorYControls->setFlexWrap(YGWrapWrap);
  judgementIndicatorYControls->setGap(metrics.compact ? 8.0f : 12.0f);
  judgementIndicatorYControls->setAlignItems(YGAlignFlexStart);
  auto updateJudgementIndicatorY = [this](int deltaPercent) {
    const int currentPercent =
        judgementIndicatorYToPercent(context.settings.presentation().judgementIndicatorY);
    const int nextPercent = std::clamp(currentPercent + deltaPercent, 0, 100);
    context.settings.presentation().judgementIndicatorY =
        judgementIndicatorPercentToY(nextPercent);
    persistSettings();
    syncJudgementIndicatorYInputText(true);
  };

  auto *minusIndicatorYLarge =
      makeStepButton(metrics, metrics.offsetButtonWidthLarge, "-5%");
  minusIndicatorYLarge->setOnClickListener(
      [updateJudgementIndicatorY]() { updateJudgementIndicatorY(-5); });
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
      makeStepButton(metrics, metrics.offsetButtonWidthLarge, "+5%");
  plusIndicatorYLarge->setOnClickListener(
      [updateJudgementIndicatorY]() { updateJudgementIndicatorY(5); });
  judgementIndicatorYControls->addView(plusIndicatorYLarge);
  auto *resetIndicatorY = makeResetButton(metrics);
  resetIndicatorY->setOnClickListener([this]() {
    context.settings.presentation().judgementIndicatorY =
        AppSettings::kDefaultJudgementIndicatorY;
    persistSettings();
    syncJudgementIndicatorYInputText(true);
  });
  judgementIndicatorYControls->addView(resetIndicatorY);
  judgementIndicatorControls->addView(judgementIndicatorYControls);

  judgementIndicatorControls->addView(
      makeText(i18n::message("settings.timing.width.label"), metrics.bodyTextSize, ui_theme::textSecondary()));
  auto *judgementIndicatorWidthControls = new View();
  judgementIndicatorWidthControls->setFlexDirection(FlexDirection::Row);
  judgementIndicatorWidthControls->setFlexWrap(YGWrapWrap);
  judgementIndicatorWidthControls->setGap(metrics.compact ? 8.0f : 12.0f);
  judgementIndicatorWidthControls->setAlignItems(YGAlignFlexStart);
  auto updateJudgementIndicatorWidth = [this](int deltaPercent) {
    const int currentPercent = judgementIndicatorWidthScaleToPercent(
        context.settings.presentation().judgementIndicatorWidthScale);
    const int minPercent = judgementIndicatorWidthScaleToPercent(
        AppSettings::kMinJudgementIndicatorWidthScale);
    const int maxPercent = judgementIndicatorWidthScaleToPercent(
        AppSettings::kMaxJudgementIndicatorWidthScale);
    const int nextPercent =
        std::clamp(currentPercent + deltaPercent, minPercent, maxPercent);
    context.settings.presentation().judgementIndicatorWidthScale =
        judgementIndicatorWidthPercentToScale(nextPercent);
    persistSettings();
    syncJudgementIndicatorWidthInputText(true);
  };

  auto *minusIndicatorWidthLarge =
      makeStepButton(metrics, metrics.offsetButtonWidthLarge, "-5%");
  minusIndicatorWidthLarge->setOnClickListener(
      [updateJudgementIndicatorWidth]() {
        updateJudgementIndicatorWidth(-5);
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
      makeStepButton(metrics, metrics.offsetButtonWidthLarge, "+5%");
  plusIndicatorWidthLarge->setOnClickListener(
      [updateJudgementIndicatorWidth]() { updateJudgementIndicatorWidth(5); });
  judgementIndicatorWidthControls->addView(plusIndicatorWidthLarge);
  auto *resetIndicatorWidth = makeResetButton(metrics);
  resetIndicatorWidth->setOnClickListener([this]() {
    context.settings.presentation().judgementIndicatorWidthScale =
        AppSettings::kDefaultJudgementIndicatorWidthScale;
    persistSettings();
    syncJudgementIndicatorWidthInputText(true);
  });
  judgementIndicatorWidthControls->addView(resetIndicatorWidth);
  judgementIndicatorControls->addView(judgementIndicatorWidthControls);

  judgementIndicatorControls->addView(makeText(
      i18n::message("settings.timing.range_ms.label"), metrics.bodyTextSize, ui_theme::textSecondary()));
  auto *judgementIndicatorRangeControls = new View();
  judgementIndicatorRangeControls->setFlexDirection(FlexDirection::Row);
  judgementIndicatorRangeControls->setFlexWrap(YGWrapWrap);
  judgementIndicatorRangeControls->setGap(metrics.compact ? 8.0f : 12.0f);
  judgementIndicatorRangeControls->setAlignItems(YGAlignFlexStart);
  auto updateJudgementIndicatorRange = [this](int deltaMilliseconds) {
    context.settings.presentation().judgementIndicatorRangeMilliseconds =
        clampJudgementIndicatorRangeMilliseconds(
            context.settings.presentation().judgementIndicatorRangeMilliseconds +
            deltaMilliseconds);
    persistSettings();
    syncJudgementIndicatorRangeInputText(true);
  };

  auto *minusIndicatorRangeLarge =
      makeStepButton(metrics, metrics.offsetButtonWidthLarge, "-10");
  minusIndicatorRangeLarge->setOnClickListener(
      [updateJudgementIndicatorRange]() {
        updateJudgementIndicatorRange(-10);
      });
  judgementIndicatorRangeControls->addView(minusIndicatorRangeLarge);
  auto *minusIndicatorRangeSmall =
      makeStepButton(metrics, metrics.offsetButtonWidthSmall, "-1");
  minusIndicatorRangeSmall->setOnClickListener(
      [updateJudgementIndicatorRange]() {
        updateJudgementIndicatorRange(-1);
      });
  judgementIndicatorRangeControls->addView(minusIndicatorRangeSmall);
  judgementIndicatorRangeInput = makeNumericInput(metrics);
  judgementIndicatorRangeInput->onEditingFinished(
      [this](const std::string &) { commitJudgementIndicatorRangeInput(); });
  judgementIndicatorRangeControls->addView(
      makeInputFrame(metrics, judgementIndicatorRangeInput));
  auto *plusIndicatorRangeSmall =
      makeStepButton(metrics, metrics.offsetButtonWidthSmall, "+1");
  plusIndicatorRangeSmall->setOnClickListener(
      [updateJudgementIndicatorRange]() {
        updateJudgementIndicatorRange(1);
      });
  judgementIndicatorRangeControls->addView(plusIndicatorRangeSmall);
  auto *plusIndicatorRangeLarge =
      makeStepButton(metrics, metrics.offsetButtonWidthLarge, "+10");
  plusIndicatorRangeLarge->setOnClickListener(
      [updateJudgementIndicatorRange]() {
        updateJudgementIndicatorRange(10);
      });
  judgementIndicatorRangeControls->addView(plusIndicatorRangeLarge);
  auto *resetIndicatorRange = makeResetButton(metrics);
  resetIndicatorRange->setOnClickListener([this]() {
    context.settings.presentation().judgementIndicatorRangeMilliseconds =
        AppSettings::kDefaultJudgementIndicatorRangeMilliseconds;
    persistSettings();
    syncJudgementIndicatorRangeInputText(true);
  });
  judgementIndicatorRangeControls->addView(resetIndicatorRange);
  judgementIndicatorControls->addView(judgementIndicatorRangeControls);

  cardsColumn->addView(makeCard(
      metrics, i18n::message("settings.timing.judgement_indicator.label"),
      i18n::message("settings.timing.set_position_size_timing_range_render_mode.message"),
      judgementIndicatorControls, metrics.visibleTimeCardHeight,
      metrics.cardsWidth));

  }

  auto *secondaryCards = new View();
  secondaryCards->setFlexDirection(
      metrics.useDualCardRow ? FlexDirection::Row : FlexDirection::Column);
  secondaryCards->setGap(static_cast<float>(metrics.secondaryGap));

  auto *keysoundControls = new View();
  keysoundControls->setFlexDirection(FlexDirection::Column);
  keysoundControls->setGap(metrics.compact ? 12.0f : 16.0f);
  keysoundControls->setAlignItems(YGAlignFlexStart);
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
  prepMetronomeModeText =
      makeText("", metrics.bodyTextSize + 6, ui_theme::textPrimary(),
               TextView::CENTER, TextView::MIDDLE);
  prepMetronomeModeButton =
      makeControlButton(kFitContentWidth, metrics.actionButtonHeight,
                        prepMetronomeModeText);
  prepMetronomeModeButton->setOnClickListener([this]() {
    context.settings.prepMetronomeEnabled =
        !context.settings.prepMetronomeEnabled;
    persistSettings();
  });
  keysoundControls->addView(prepMetronomeModeButton);
  secondaryCards->addView(makeCard(
      metrics, i18n::message("settings.timing.input_audio.label"), i18n::message("settings.timing.choose_hit_sounds_count_in.message"),
      keysoundControls, metrics.modeCardHeight, metrics.secondaryCardWidth));

  auto *notePriorityControls = new View();
  notePriorityControls->setFlexDirection(FlexDirection::Column);
  notePriorityControls->setGap(metrics.compact ? 12.0f : 16.0f);
  notePriorityControls->setAlignItems(YGAlignFlexStart);
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
      metrics, i18n::message("settings.timing.note_priority.label"), i18n::message("settings.timing.choose_which_nearby_note_press_judges.message"),
      notePriorityControls, metrics.modeCardHeight,
      metrics.secondaryCardWidth));

  cardsColumn->addView(secondaryCards);
  return cardsColumn;
}

View *SettingsScene::buildVisualTab(const LayoutMetrics &metrics) {
  auto *cardsColumn = makeCardsColumn(metrics);
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  const bool showLegacyBuiltInGameplayControls =
      !gameplaySkinTraitsRuntimeAvailable();
#else
  constexpr bool showLegacyBuiltInGameplayControls = true;
#endif
  auto *bgaControls = new View();
  bgaControls->setFlexDirection(FlexDirection::Column);
  bgaControls->setGap(metrics.compact ? 12.0f : 16.0f);
  bgaControls->setAlignItems(YGAlignFlexStart);
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
      metrics, i18n::message("settings.visual.bga_playback.label"), i18n::message("settings.visual.show_background_animation.message"),
      bgaControls, metrics.modeCardHeight, metrics.cardsWidth));

  auto *invisibleNoteControls = new View();
  invisibleNoteControls->setFlexDirection(FlexDirection::Column);
  invisibleNoteControls->setGap(metrics.compact ? 12.0f : 16.0f);
  invisibleNoteControls->setAlignItems(YGAlignFlexStart);
  showInvisibleNotesModeText =
      makeText("", metrics.bodyTextSize + 6, ui_theme::textPrimary(),
               TextView::CENTER, TextView::MIDDLE);
  showInvisibleNotesModeButton =
      makeControlButton(metrics.actionButtonWidth, metrics.actionButtonHeight,
                        showInvisibleNotesModeText);
  showInvisibleNotesModeButton->setOnClickListener([this]() {
    context.settings.showInvisibleNotes = !context.settings.showInvisibleNotes;
    persistSettings();
    syncPreviewPresentationConfiguration();
  });
  invisibleNoteControls->addView(showInvisibleNotesModeButton);
  cardsColumn->addView(makeCard(
      metrics, i18n::message("settings.visual.invisible_notes.label"), i18n::message("settings.visual.show_invisible_notes_as_lane_markers.message"),
      invisibleNoteControls, metrics.modeCardHeight, metrics.cardsWidth));

  auto *processedNoteControls = new View();
  processedNoteControls->setFlexDirection(FlexDirection::Column);
  processedNoteControls->setGap(metrics.compact ? 12.0f : 16.0f);
  processedNoteControls->setAlignItems(YGAlignFlexStart);
  markProcessedNotesModeText =
      makeText("", metrics.bodyTextSize + 6, ui_theme::textPrimary(),
               TextView::CENTER, TextView::MIDDLE);
  markProcessedNotesModeButton =
      makeControlButton(metrics.actionButtonWidth, metrics.actionButtonHeight,
                        markProcessedNotesModeText);
  markProcessedNotesModeButton->setOnClickListener([this]() {
    context.settings.markProcessedNotes = !context.settings.markProcessedNotes;
    persistSettings();
  });
  processedNoteControls->addView(markProcessedNotesModeButton);
  cardsColumn->addView(makeCard(
      metrics, i18n::message("settings.visual.mark_processed_notes.label"),
      i18n::message("settings.visual.processed_notes.description"),
      processedNoteControls, metrics.modeCardHeight, metrics.cardsWidth));

#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
  if (IsIOSPad()) {
    auto *controls = new View();
    ipadGestureReminderModeText =
        makeText("", metrics.bodyTextSize + 6, ui_theme::textPrimary(),
                 TextView::CENTER, TextView::MIDDLE);
    ipadGestureReminderModeButton = makeControlButton(metrics.actionButtonWidth,
                                                     metrics.actionButtonHeight,
                                                     ipadGestureReminderModeText);
    ipadGestureReminderModeButton->setOnClickListener([this]() {
      context.settings.ipadGestureReminderEnabled =
          !context.settings.ipadGestureReminderEnabled;
      persistSettings();
    });
    controls->addView(ipadGestureReminderModeButton);
    cardsColumn->addView(makeCard(
        metrics, i18n::message("settings.visual.ipad_gesture_reminder.label"),
        i18n::message("settings.visual.ipad_gesture_reminder.description"),
        controls, metrics.modeCardHeight, metrics.cardsWidth));
  }
#endif

  auto *startLaneIndicatorControls = new View();
  startLaneIndicatorControls->setFlexDirection(FlexDirection::Column);
  startLaneIndicatorControls->setGap(metrics.compact ? 12.0f : 16.0f);
  startLaneIndicatorControls->setAlignItems(YGAlignFlexStart);
  startLaneIndicatorsModeText =
      makeText("", metrics.bodyTextSize + 6, ui_theme::textPrimary(),
               TextView::CENTER, TextView::MIDDLE);
  startLaneIndicatorsModeButton = makeControlButton(
      metrics.actionButtonWidth, metrics.actionButtonHeight,
      startLaneIndicatorsModeText);
  startLaneIndicatorsModeButton->setOnClickListener([this]() {
    context.settings.startLaneIndicatorsEnabled =
        !context.settings.startLaneIndicatorsEnabled;
    persistSettings();
  });
  startLaneIndicatorControls->addView(startLaneIndicatorsModeButton);
  cardsColumn->addView(makeCard(
      metrics, i18n::message("settings.visual.start_lane_indicators.label"),
      i18n::message("settings.visual.show_lanes_used_by_first_playable_chord.message"),
      startLaneIndicatorControls, metrics.modeCardHeight, metrics.cardsWidth));

  auto *touchVisualizationControls = new View();
  touchVisualizationControls->setFlexDirection(FlexDirection::Column);
  touchVisualizationControls->setGap(metrics.compact ? 12.0f : 16.0f);
  touchVisualizationControls->setAlignItems(YGAlignFlexStart);
  touchVisualizationModeText =
      makeText("", metrics.bodyTextSize + 6, ui_theme::textPrimary(),
               TextView::CENTER, TextView::MIDDLE);
  touchVisualizationModeButton =
      makeControlButton(metrics.actionButtonWidth, metrics.actionButtonHeight,
                        touchVisualizationModeText);
  touchVisualizationModeButton->setOnClickListener([this]() {
    context.settings.touchVisualizationEnabled =
        !context.settings.touchVisualizationEnabled;
    persistSettings();
  });
  touchVisualizationControls->addView(touchVisualizationModeButton);
  cardsColumn->addView(makeCard(
      metrics, i18n::message("settings.visual.touch_points.label"), i18n::message("settings.visual.show_touch_positions_during_play.message"),
      touchVisualizationControls, metrics.modeCardHeight, metrics.cardsWidth));

  if (showLegacyBuiltInGameplayControls) {
  auto *judgementCounterControls = new View();
  judgementCounterControls->setFlexDirection(FlexDirection::Column);
  judgementCounterControls->setGap(metrics.compact ? 12.0f : 16.0f);
  judgementCounterControls->setAlignItems(YGAlignFlexStart);
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
    context.settings.presentation().judgementCounterEnabled =
        !context.settings.presentation().judgementCounterEnabled;
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
    context.settings.presentation().judgementCounterPosition =
        nextJudgementCounterPosition(context.settings.presentation().judgementCounterPosition);
    persistSettings();
  });
  judgementCounterModeControls->addView(judgementCounterPositionButton);
  judgementCounterControls->addView(judgementCounterModeControls);
  cardsColumn->addView(makeCard(
      metrics, i18n::message("settings.visual.judgement_counter.label"), i18n::message("settings.visual.show_live_judgement_totals.message"),
      judgementCounterControls, metrics.modeCardHeight, metrics.cardsWidth));

  auto *gaugeControls = new View();
  gaugeControls->setFlexDirection(FlexDirection::Column);
  gaugeControls->setGap(metrics.compact ? 12.0f : 16.0f);
  gaugeControls->setAlignItems(YGAlignFlexStart);
  gaugeControls->addView(makeWrappedText(
      i18n::message("settings.visual.gauge_position.help"),
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
    context.settings.presentation().gaugeBarPosition =
        nextGaugeBarPosition(context.settings.presentation().gaugeBarPosition);
    persistSettings();
  });
  gaugePositionControls->addView(gaugeBarPositionButton);
  gaugeControls->addView(gaugePositionControls);
  cardsColumn->addView(makeCard(
      metrics, i18n::message("settings.visual.gauge_bar.label"), i18n::message("settings.visual.choose_gauge_position.message"),
      gaugeControls, metrics.modeCardHeight, metrics.cardsWidth));

  }

  auto *bgaDisplayControls = new View();
  bgaDisplayControls->setFlexDirection(FlexDirection::Column);
  bgaDisplayControls->setGap(metrics.compact ? 12.0f : 16.0f);
  bgaDisplayControls->setAlignItems(YGAlignFlexStart);
  bgaDisplayControls->addView(makeWrappedText(
      i18n::message("settings.visual.fit_shows_all_fill_crops_stretch_distorts.message"),
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
      metrics, i18n::message("settings.visual.bga_aspect.label"), "",
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
  auto *minusBrightnessFive =
      makeStepButton(metrics, metrics.offsetButtonWidthLarge, "-5");
  minusBrightnessFive->setOnClickListener(
      [updateBgaBrightness]() { updateBgaBrightness(-5); });
  brightnessControls->addView(minusBrightnessFive);
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
  auto *plusBrightnessFive =
      makeStepButton(metrics, metrics.offsetButtonWidthLarge, "+5");
  plusBrightnessFive->setOnClickListener(
      [updateBgaBrightness]() { updateBgaBrightness(5); });
  brightnessControls->addView(plusBrightnessFive);
  auto *resetBrightness = makeResetButton(metrics);
  resetBrightness->setOnClickListener([this]() {
    context.settings.bgaBrightnessPercent =
        AppSettings::kDefaultBgaBrightnessPercent;
    persistSettings();
    syncBgaBrightnessInputText(true);
  });
  brightnessControls->addView(resetBrightness);
  cardsColumn->addView(makeCard(
      metrics, i18n::message("settings.visual.bga_brightness.label"), i18n::message("settings.visual.dim_bga_behind_lanes.message"),
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
      metrics, i18n::message("settings.visual.bga_blur.label"), i18n::message("settings.visual.soften_background_motion.message"),
      blurControls, metrics.offsetCardHeight, metrics.cardsWidth));
  return cardsColumn;
}

View *SettingsScene::buildLaneTab(const LayoutMetrics &metrics) {
  auto *cardsColumn = makeCardsColumn(metrics);
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  const bool showLegacyBuiltInGameplayControls =
      !gameplaySkinTraitsRuntimeAvailable();
#else
  constexpr bool showLegacyBuiltInGameplayControls = true;
#endif

  auto *previewControls = new View();
  previewControls->setFlexDirection(FlexDirection::Column);
  previewControls->setGap(metrics.compact ? 12.0f : 16.0f);
  previewControls->setAlignItems(YGAlignFlexStart);
  auto *previewButton = makeAccentButton(
      metrics.actionButtonWidth, metrics.actionButtonHeight,
      makeText(i18n::message("settings.lane.preview.label"), metrics.bodyTextSize + 4, ui_theme::textPrimary(),
               TextView::CENTER, TextView::MIDDLE),
      ui_theme::lime());
  previewButton->setOnClickListener([this]() { startLanePreview(); });
  previewControls->addView(previewButton);
  cardsColumn->addView(makeCard(
      metrics, i18n::message("settings.lane.gameplay_preview.label"), i18n::message("settings.lane.preview_current_lane_hud.message"),
      previewControls, metrics.modeCardHeight, metrics.cardsWidth));

  auto *visibleTimeControls = buildVisibleTimeControls(metrics, true, false);
  cardsColumn->addView(makeCard(
      metrics, i18n::message("settings.lane.visible_time.label"), i18n::message("settings.lane.set_how_long_notes_remain_visible.message"),
      visibleTimeControls, metrics.visibleTimeCardHeight, metrics.cardsWidth));

  auto *noteStartPanel = new View();
  noteStartPanel->setFlexDirection(FlexDirection::Column);
  noteStartPanel->setGap(metrics.compact ? 12.0f : 16.0f);
  noteStartPanel->setAlignItems(YGAlignFlexStart);

  auto *noteStartControls = new View();
  noteStartControls->setFlexDirection(FlexDirection::Row);
  noteStartControls->setFlexWrap(YGWrapWrap);
  noteStartControls->setGap(metrics.compact ? 8.0f : 12.0f);
  noteStartControls->setAlignItems(YGAlignFlexStart);
  auto updateNoteStartPosition = [this](int deltaPercent) {
    context.settings.presentation().noteStartPositionPercent = clampNoteStartPositionPercent(
        context.settings.presentation().noteStartPositionPercent + deltaPercent);
    persistSettings();
    syncNoteStartPositionInputText(true);
  };
  auto *minusNoteStartLarge =
      makeStepButton(metrics, metrics.offsetButtonWidthLarge, "-5%");
  minusNoteStartLarge->setOnClickListener(
      [updateNoteStartPosition]() { updateNoteStartPosition(-5); });
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
      makeStepButton(metrics, metrics.offsetButtonWidthLarge, "+5%");
  plusNoteStartLarge->setOnClickListener(
      [updateNoteStartPosition]() { updateNoteStartPosition(5); });
  noteStartControls->addView(plusNoteStartLarge);
  auto *resetNoteStart = makeResetButton(metrics);
  resetNoteStart->setOnClickListener([this]() {
    context.settings.presentation().noteStartPositionPercent =
        AppSettings::kDefaultNoteStartPositionPercent;
    persistSettings();
    syncNoteStartPositionInputText(true);
  });
  noteStartControls->addView(resetNoteStart);
  noteStartPanel->addView(noteStartControls);

  hispeedAutoAdjustModeText =
      makeText(i18n::message("settings.lane.hi_speed_auto_adjust_off.label"), metrics.bodyTextSize + 6,
               ui_theme::textPrimary(),
               TextView::CENTER, TextView::MIDDLE);
  constexpr int hispeedAutoAdjustHorizontalPadding = 32;
  const int hispeedAutoAdjustButtonWidth =
      std::max(metrics.actionButtonWidth,
               hispeedAutoAdjustModeText->textureWidth() +
                   hispeedAutoAdjustHorizontalPadding);
  hispeedAutoAdjustModeButton =
      makeControlButton(hispeedAutoAdjustButtonWidth,
                        metrics.actionButtonHeight,
                        hispeedAutoAdjustModeText);
  hispeedAutoAdjustModeButton->setOnClickListener([this]() {
    context.settings.hispeedAutoAdjust = !context.settings.hispeedAutoAdjust;
    persistSettings();
  });
  noteStartPanel->addView(hispeedAutoAdjustModeButton);
  cardsColumn->addView(makeCard(
      metrics, i18n::message("settings.lane.note_start_position.label"), i18n::message("settings.lane.set_where_notes_enter_lane.message"),
      noteStartPanel, metrics.offsetCardHeight, metrics.cardsWidth));

  if (showLegacyBuiltInGameplayControls) {
  cardsColumn->addView(makeCard(metrics,
      i18n::message("settings.skins.scratch_position.label"), "",
      buildScratchLanePositionControl(metrics), metrics.modeCardHeight, metrics.cardsWidth));
  auto *angleControls = new View();
  angleControls->setFlexDirection(FlexDirection::Row);
  angleControls->setFlexWrap(YGWrapWrap);
  angleControls->setGap(metrics.compact ? 8.0f : 12.0f);
  angleControls->setAlignItems(YGAlignFlexStart);
  auto updateLaneAngle = [this](float delta) {
    context.settings.presentation().laneAngleDegrees =
        clampLaneAngle(context.settings, context.settings.presentation().laneAngleDegrees + delta);
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
    context.settings.presentation().laneAngleDegrees = context.settings.geometryPolicy().angle.defaultValue;
    persistSettings();
    syncLaneAngleInputText(true);
  });
  angleControls->addView(resetAngle);
  cardsColumn->addView(makeCard(
      metrics, i18n::message("settings.lane.lane_angle.label"), i18n::message("settings.lane.tilt_lane_touch_plane.message"),
      angleControls, metrics.offsetCardHeight, metrics.cardsWidth));

  auto *lengthControls = new View();
  lengthControls->setFlexDirection(FlexDirection::Row);
  lengthControls->setFlexWrap(YGWrapWrap);
  lengthControls->setGap(metrics.compact ? 8.0f : 12.0f);
  lengthControls->setAlignItems(YGAlignFlexStart);
  auto updateLaneLength = [this](float delta) {
    context.settings.presentation().laneLength =
        clampLaneLength(context.settings, context.settings.presentation().laneLength + delta);
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
    context.settings.presentation().laneLength = context.settings.geometryPolicy().length.defaultValue;
    persistSettings();
    syncLaneLengthInputText(true);
  });
  lengthControls->addView(resetLength);
  cardsColumn->addView(makeCard(
      metrics, i18n::message("settings.lane.lane_length.label"), i18n::message("settings.lane.set_how_far_lane_reaches.message"),
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
                                                  clampPlayAreaWidth(context.settings, width));
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
        [this, applyWidth]() { applyWidth(context.settings.geometryPolicy().width.defaultValue); });
    row->addView(resetWidth);

    syncInput();
    return row;
  };

  for (int keyMode : {4, 5, 6, 7, 8, 10, 14}) {
    playAreaWidthControls->addView(makePlayAreaWidthRow(keyMode));
  }
  cardsColumn->addView(makeCard(
      metrics, i18n::message("settings.lane.play_area_width.label"), i18n::message("settings.lane.set_width_each_key_mode.message"),
      playAreaWidthControls, metrics.visibleTimeCardHeight,
      metrics.cardsWidth));

  auto *beamControls = new View();
  beamControls->setFlexDirection(FlexDirection::Row);
  beamControls->setFlexWrap(YGWrapWrap);
  beamControls->setGap(metrics.compact ? 8.0f : 12.0f);
  beamControls->setAlignItems(YGAlignFlexStart);
  auto updateLaneBeamLength = [this](int deltaPercent) {
    context.settings.presentation().laneBeamLengthPercent = clampLaneBeamLengthPercent(
        context.settings.presentation().laneBeamLengthPercent + deltaPercent);
    persistSettings();
    syncLaneBeamLengthInputText(true);
  };
  auto *minusBeamLarge =
      makeStepButton(metrics, metrics.offsetButtonWidthLarge, "-5%");
  minusBeamLarge->setOnClickListener(
      [updateLaneBeamLength]() { updateLaneBeamLength(-5); });
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
      makeStepButton(metrics, metrics.offsetButtonWidthLarge, "+5%");
  plusBeamLarge->setOnClickListener(
      [updateLaneBeamLength]() { updateLaneBeamLength(5); });
  beamControls->addView(plusBeamLarge);
  auto *resetBeam = makeResetButton(metrics);
  resetBeam->setOnClickListener([this]() {
    context.settings.presentation().laneBeamLengthPercent =
        AppSettings::kDefaultLaneBeamLengthPercent;
    persistSettings();
    syncLaneBeamLengthInputText(true);
  });
  beamControls->addView(resetBeam);
  cardsColumn->addView(makeCard(
      metrics, i18n::message("settings.lane.lane_beam_length.label"), i18n::message("settings.lane.set_press_feedback_height.message"),
      beamControls, metrics.offsetCardHeight, metrics.cardsWidth));

  }

  return cardsColumn;
}

View *SettingsScene::buildMiscTab(const LayoutMetrics &metrics) {
  auto *cardsColumn = makeCardsColumn(metrics);
  measureTemporaryArchiveCache();

  auto *languageControls = new View();
  languageControls->setFlexDirection(FlexDirection::Column);
  languageControls->setGap(metrics.compact ? 12.0F : 16.0F);
  languageControls->setAlignItems(YGAlignFlexStart);
  auto *languageStatus = makeWrappedText(
      i18n::message("settings.language.change_notice"),
      metrics.bodyTextSize, ui_theme::textSecondary());
  auto *languageDropdown = new DropdownView(
      {.onOptionSelectedResult =
           [this, languageStatus](const std::string &id) {
             auto &preference = context.applicationUiState.language;
             const auto previous = preference;
             preference = id;
             std::string error;
             const bool saved = context.saveApplicationUiState(&error);
             if (!saved) {
               preference = previous;
             } else {
               i18n::initializePlatformLanguage(id);
             }
             languageStatus->setLocalizedText(i18n::message(
                 saved ? "settings.language.change_notice"
                       : "settings.language.save_error"));
             rootLayout->applyYogaLayout();
             return saved;
           }},
      overlayPortal);
  languageDropdown->refresh(
      {.selectedId = context.applicationUiState.language,
       .options = {{.id = "system",
                    .label = i18n::message("settings.language.system.label")},
                   {.id = "en", .label = "English"},
                   {.id = "ko", .label = "한국어"},
                   {.id = "ja", .label = "日本語"}},
       .maxVisibleItems = 4});
  languageControls->addView(languageDropdown);
  languageControls->addView(languageStatus);
  cardsColumn->addView(makeCard(
      metrics, i18n::message("settings.language.title"), "", languageControls,
      metrics.modeCardHeight, metrics.cardsWidth));

  auto *themeControls = new View();
  themeControls->setFlexDirection(FlexDirection::Column);
  themeControls->setGap(metrics.compact ? 12.0f : 16.0f);
  themeControls->setAlignItems(YGAlignFlexStart);
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
      metrics, i18n::message("settings.misc.theme.label"), i18n::message("settings.misc.choose_ui_palette.message"),
      themeControls, metrics.modeCardHeight, metrics.cardsWidth));

  auto *archivePreviewControls = new View();
  archivePreviewControls->setFlexDirection(FlexDirection::Column);
  archivePreviewControls->setGap(metrics.compact ? 12.0f : 16.0f);
  archivePreviewControls->setAlignItems(YGAlignFlexStart);
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
      metrics, i18n::message("settings.misc.archive_preview.label"), i18n::message("settings.misc.preview_charts_inside_archives.message"),
      archivePreviewControls, metrics.modeCardHeight, metrics.cardsWidth));

  auto *findBmsArchiveControls = new View();
  findBmsArchiveControls->setFlexDirection(FlexDirection::Column);
  findBmsArchiveControls->setGap(metrics.compact ? 12.0f : 16.0f);
  findBmsArchiveControls->setAlignItems(YGAlignFlexStart);
  findBmsSkipUnarchivingModeText =
      makeText("", metrics.bodyTextSize + 6, ui_theme::textPrimary(),
               TextView::CENTER, TextView::MIDDLE);
  findBmsSkipUnarchivingModeButton =
      makeAccentButton(metrics.actionButtonWidth, metrics.actionButtonHeight,
                       findBmsSkipUnarchivingModeText, ui_theme::lime());
  findBmsSkipUnarchivingModeButton->setOnClickListener([this]() {
    context.settings.findBmsSkipUnarchivingForNonSolidArchives =
        !context.settings.findBmsSkipUnarchivingForNonSolidArchives;
    persistSettings();
  });
  findBmsArchiveControls->addView(findBmsSkipUnarchivingModeButton);
  cardsColumn->addView(makeCard(
      metrics, BmsSearchService::kSkipUnarchivingSettingLabel,
      i18n::message("settings.misc.archive_extraction.non_solid_help"),
      findBmsArchiveControls, metrics.modeCardHeight, metrics.cardsWidth));

  auto *cacheCleanupControls = new View();
  cacheCleanupControls->setFlexDirection(FlexDirection::Column);
  cacheCleanupControls->setGap(metrics.compact ? 12.0f : 16.0f);
  cacheCleanupControls->setAlignItems(YGAlignFlexStart);
  archiveCacheCleanupButtonText =
      makeText(archiveCacheMaintenance.cleanupRunning() ? i18n::message("settings.misc.cleaning.progress") : i18n::message("settings.misc.clean_up.label"),
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
      metrics, i18n::message("settings.misc.archive_cache.label"), i18n::message("settings.misc.remove_temporary_extracted_media.message"),
      cacheCleanupControls, metrics.modeCardHeight, metrics.cardsWidth));

  return cardsColumn;
}

View *SettingsScene::buildDifficultyTablesTab(const LayoutMetrics &metrics) {
  auto *cardsColumn = makeCardsColumn(metrics);
  loadDifficultyTables();

  const i18n::Text tableCardDescription = i18n::message("settings.difficulty_tables.add_bmstable_url.message");

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
  tableUrlInput->setClearable(true);
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
      makeText(i18n::message("settings.difficulty_tables.add_table.label"), metrics.bodyTextSize + 4, ui_theme::textPrimary(),
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
    tableList->addView(makeWrappedText(i18n::message("settings.difficulty_tables.no_difficulty_tables_installed.message"),
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
          makeText(i18n::message("settings.difficulty_tables.update.label"), metrics.bodyTextSize + 2, ui_theme::textPrimary(),
                   TextView::CENTER, TextView::MIDDLE));
      updateButton->setOnClickListener([this, tableId = table.id]() {
        updateDifficultyTableFromSource(tableId);
      });
      actions->addView(updateButton);

      const bool confirmingDelete = pendingDeleteDifficultyTableId == table.id;
      auto *deleteButton = makeAccentButton(
          smallActionWidth, metrics.actionButtonHeight,
          makeText(confirmingDelete ? i18n::message("settings.difficulty_tables.confirm.label") : i18n::message("settings.difficulty_tables.delete.label"),
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

  cardsColumn->addView(makeCard(metrics, i18n::message("settings.difficulty_tables.installed_difficulty_tables.label"),
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
      kFitContentWidth, metrics.actionButtonHeight,
      makeText(i18n::message("settings.bms_library.rebuild_library.label"), metrics.bodyTextSize + 2,
               ui_theme::textPrimary(), TextView::CENTER, TextView::MIDDLE),
      ui_theme::lime());
  refreshFoldersButton->setOnClickListener([this]() { refreshChartLibrary(); });
  folderActions->addView(refreshFoldersButton);

  bool showAddFolderButton = true;
  bool importFolderByCopy = false;
  i18n::Text addFolderButtonLabel = i18n::message("settings.bms_library.add_folder.label");
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  showAddFolderButton = true;
#elif TARGET_OS_ANDROID
  const bool androidFullFileAccessBuild =
      AndroidBuildHasManageExternalStorage();
  showAddFolderButton = true;
  importFolderByCopy = !androidFullFileAccessBuild;
  addFolderButtonLabel =
      androidFullFileAccessBuild ? i18n::message("settings.bms_library.add_folder.label") : i18n::message("settings.bms_library.import_folder.label");
#endif
  if (showAddFolderButton) {
    auto *addFolderButton = makeAccentButton(
        kFitContentWidth, metrics.actionButtonHeight,
        makeText(addFolderButtonLabel, metrics.bodyTextSize + 2,
                 ui_theme::textPrimary(), TextView::CENTER, TextView::MIDDLE),
        ui_theme::cyan());
    addFolderButton->setOnClickListener([this, importFolderByCopy,
                                         addFolderButtonLabel]() {
      if (context.requestAddChartFolderFromFiles) {
        context.requestAddChartFolderFromFiles();
        chartFolderStatusMessage = importFolderByCopy
                                       ? i18n::message("settings.bms_library.choose_folder_import.message")
                                       : i18n::message("settings.bms_library.choose_folder_add.message");
        chartFolderStatusColor = ui_theme::sdl(ui_theme::textSecondary());
      } else {
        chartFolderStatusMessage = i18n::message("settings.library.action.unavailable", {{"action", addFolderButtonLabel}});
        chartFolderStatusColor = {255, 177, 170, 255};
      }

      if (chartFolderStatusText != nullptr) {
        chartFolderStatusText->setLocalizedText(chartFolderStatusMessage);
        chartFolderStatusText->setColor(chartFolderStatusColor);
      }
    });
    folderActions->addView(addFolderButton);
  }

  folderList->addView(folderActions);

#if TARGET_OS_ANDROID
  if (importFolderByCopy) {
    auto *playNote = new View();
    playNote->setFlexDirection(FlexDirection::Column);
    playNote->setGap(metrics.compact ? 8.0f : 10.0f);
    playNote->setAlignSelf(YGAlignStretch);
    playNote->setPadding(Edge::All,
                         static_cast<float>(metrics.compact ? 14 : 16));
    playNote->setThemedBackgroundColor(ui_theme::panelSubtle);
    playNote->setCornerRadius(ui_theme::controlRadius());
    playNote->setThemedBorderColor(ui_theme::hairline);
    playNote->setBorderWidth(1);
    playNote->addView(makeWrappedText(
        i18n::message("settings.bms_library.folder_import.play_storage_notice"),
        metrics.bodyTextSize, ui_theme::textSecondary()));

    auto *repoButton = makeAccentButton(
        metrics.compact ? 150 : 170, metrics.actionButtonHeight,
        makeText(i18n::message("settings.bms_library.open_git_hub.label"), metrics.bodyTextSize + 2,
                 ui_theme::textPrimary(), TextView::CENTER, TextView::MIDDLE),
        ui_theme::cyan());
    repoButton->setOnClickListener([this]() {
      std::string errorMessage;
      if (!OpenURLInAndroidBrowser(kRepositoryUrl, errorMessage)) {
        chartFolderStatusMessage = i18n::message("settings.bms_library.could_not_open_git_hub_link.message");
        chartFolderStatusColor = {255, 177, 170, 255};
        if (chartFolderStatusText != nullptr) {
          chartFolderStatusText->setLocalizedText(chartFolderStatusMessage);
          chartFolderStatusText->setColor(chartFolderStatusColor);
        }
        if (!errorMessage.empty()) {
          SDL_Log("Failed to open repository URL: %s", errorMessage.c_str());
        }
      }
    });
    playNote->addView(repoButton);
    folderList->addView(playNote);
  }
#endif

  chartFolderStatusText = makeWrappedText(
      chartFolderStatusMessage, metrics.bodyTextSize,
      Color(chartFolderStatusColor.r, chartFolderStatusColor.g,
            chartFolderStatusColor.b, chartFolderStatusColor.a));
  folderList->addView(chartFolderStatusText);

  if (chartEntries.empty()) {
    folderList->addView(makeWrappedText(i18n::message("settings.bms_library.no_chart_folders_installed.message"),
                                        metrics.bodyTextSize,
                                        ui_theme::textSecondary()));
    folderList->addView(makeWrappedText(
        i18n::message("settings.bms_library.default_download_folder.help"),
        metrics.smallTextSize, ui_theme::textMuted()));
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
      if (entry.primaryStorageFolder) {
        actions->addView(makeWrappedText(i18n::message("settings.bms_library.download_folder.label"),
                                         metrics.smallTextSize,
                                         ui_theme::lime()));
      } else if (entry.primaryStorageEligible) {
        auto *downloadButton = makeAccentButton(
            metrics.compact ? 180 : 210, metrics.actionButtonHeight,
            makeText(i18n::message("settings.bms_library.use_downloads.label"), metrics.smallTextSize,
                     ui_theme::textPrimary(), TextView::CENTER,
                     TextView::MIDDLE),
            ui_theme::cyan());
        downloadButton->setOnClickListener([this, entryPathText]() {
          setFindBmsDownloadEntry(entryPathText);
        });
        actions->addView(downloadButton);
      } else if (ChartRepository::IsDefaultBmsFolderPath(
                     std::filesystem::path(entry.path))) {
        actions->addView(makeWrappedText(i18n::message("settings.bms_library.fallback_download_folder.label"),
                                         metrics.smallTextSize,
                                         ui_theme::textMuted()));
      } else {
        actions->addView(makeWrappedText(i18n::message("settings.bms_library.not_writable_by_find_bms.label"),
                                         metrics.smallTextSize,
                                         ui_theme::textMuted()));
      }

      if (entry.removable) {
        const bool confirmingDelete =
            pendingDeleteChartEntryPath == entryPathText;
        auto *deleteButton = makeAccentButton(
            folderActionWidth, metrics.actionButtonHeight,
            makeText(confirmingDelete ? i18n::message("settings.bms_library.confirm.label") : i18n::message("settings.bms_library.delete.label"),
                     metrics.bodyTextSize + 2, ui_theme::textPrimary(),
                     TextView::CENTER, TextView::MIDDLE),
            ui_theme::coral());
        deleteButton->setOnClickListener(
            [this, entryPathText]() { deleteChartEntry(entryPathText); });
        actions->addView(deleteButton);
      } else {
        actions->addView(makeWrappedText(i18n::message("settings.bms_library.built_in.label"), metrics.smallTextSize,
                                         ui_theme::textMuted()));
      }

#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
      const auto backupStatusIt =
          chartEntryICloudBackupExcluded.find(entryPathText);
      const bool backupExcluded =
          backupStatusIt != chartEntryICloudBackupExcluded.end() &&
          backupStatusIt->second;
      const int backupActionWidth = metrics.compact ? 224 : 260;
      auto *backupButton = makeAccentButton(
          backupActionWidth, metrics.actionButtonHeight,
          makeText(backupExcluded ? i18n::message("settings.bms_library.enable_i_cloud_backup.label")
                                  : i18n::message("settings.bms_library.disable_i_cloud_backup.label"),
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
      metrics, i18n::message("settings.bms_library.chart_folders.label"), i18n::message("settings.bms_library.manage_library_folders.message"),
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
      makeWrappedText(i18n::message("settings.difficulty_table_import_modal.importing_difficulty_tables.label"), metrics.sectionTitleSize,
                      ui_theme::textPrimary());
  importPanel->addView(difficultyTableImportTitleText);

  difficultyTableImportStatusText = makeWrappedText(
      i18n::message("settings.difficulty_table_import_modal.preparing_import.progress"), metrics.bodyTextSize, ui_theme::textSecondary());
  importPanel->addView(difficultyTableImportStatusText);

  difficultyTableImportTableText =
      makeWrappedText(i18n::message("settings.difficulty_table_import_modal.current_table_resolving_table_url.label"),
                      metrics.bodyTextSize, ui_theme::textPrimary());
  importPanel->addView(difficultyTableImportTableText);

  auto *progressRow = new View();
  progressRow->setFlexDirection(FlexDirection::Column);
  progressRow->setGap(metrics.compact ? 8.0f : 10.0f);
  difficultyTableImportProgressText =
      makeText(i18n::message("settings.difficulty_table_import_modal.import.initial_progress"), metrics.bodyTextSize, ui_theme::textMuted());
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
      makeText(i18n::message("settings.difficulty_table_import_modal.close.label"), metrics.bodyTextSize + 2, ui_theme::textPrimary(),
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
    overlayPortal = new OverlayPortal(0, 0, rendering::window_width,
                                      rendering::window_height);
    overlayPortal->setPositionType(YGPositionTypeAbsolute);
    overlayPortal->setPosition(Edge::Left, 0);
    overlayPortal->setPosition(Edge::Top, 0);
    overlayPortal->setZIndex(900);
    buildPreviewLayout(metrics);
    rootLayout->addView(overlayPortal);
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
      makeText(i18n::message("settings.navigation.settings.label"), metrics.titleSize, ui_theme::textPrimary()));
  headerText->addView(makeText(i18n::message(
      context.settings.activePresentationOrientation() == player_settings::PresentationOrientation::Portrait
          ? "settings.presentation.portrait.label" : "settings.presentation.landscape.label"),
      metrics.smallTextSize, ui_theme::textSecondary()));
  header->addView(headerText);

  auto *backLabel =
      makeText(i18n::message("settings.navigation.back.label"), metrics.bodyTextSize + 6, ui_theme::textPrimary(),
               TextView::CENTER, TextView::MIDDLE);
  auto *backButton = makeButton(
      metrics.backButtonWidth, metrics.backButtonHeight, backLabel,
      ui_theme::control(), ui_theme::controlHover(), ui_theme::controlPressed(),
      ui_theme::hairline(), ui_theme::cyan(), ui_theme::cyan());
  const auto profilePhase = profileController ? profileController->phase()
                                              : ProfileSettingsPhase::Idle;
  const bool profilePipelineBusy =
      profilePhase == ProfileSettingsPhase::PickingImport ||
      profilePhase == ProfileSettingsPhase::Importing ||
      profilePhase == ProfileSettingsPhase::PreparingExport ||
      profilePhase == ProfileSettingsPhase::PickingExport;
  backButton->setEnabled(!profilePipelineBusy);
  backButton->setOnClickListener([this]() {
    if (profileController != nullptr) {
      const auto phase = profileController->phase();
      if (phase == ProfileSettingsPhase::PickingImport ||
          phase == ProfileSettingsPhase::Importing ||
          phase == ProfileSettingsPhase::PreparingExport ||
          phase == ProfileSettingsPhase::PickingExport) {
        return;
      }
    }
    (void)returnToScene(*context.sceneManager, returnTarget_);
  });
  header->addView(backButton);
  rootLayout->addView(header);

  const bool portrait = rendering::window_height > rendering::window_width;
  const int tabColumnWidth = portrait ? (metrics.contentWidth - 16) / 3 : std::min(
      metrics.contentWidth,
      metrics.compact ? std::clamp(metrics.contentWidth / 4, 150, 190)
                      : std::clamp(metrics.contentWidth / 6, 220, 280));
  const int scrollRightPadding = metrics.compact ? 12 : 16;
  metrics.cardsWidth = std::max(0, metrics.contentWidth - scrollRightPadding -
      (portrait ? 0 : tabColumnWidth + metrics.bodyGap));
  metrics.useDualCardRow = !metrics.compact && metrics.cardsWidth >= 980;
  metrics.secondaryCardWidth =
      metrics.useDualCardRow
          ? std::max(0, (metrics.cardsWidth - metrics.secondaryGap) / 2)
          : metrics.cardsWidth;

  auto *content = new View();
  content->setFlexDirection(portrait ? FlexDirection::Column : FlexDirection::Row);
  content->setGap(static_cast<float>(metrics.bodyGap));
  content->setFlex(1.0f);
  content->setAlignItems(YGAlignStretch);

  auto *tabControls = new View();
  tabControls->setFlexDirection(portrait ? FlexDirection::Row : FlexDirection::Column);
  tabControls->setFlexWrap(portrait ? YGWrapWrap : YGWrapNoWrap);
  tabControls->setGap(metrics.compact ? 8.0f : 12.0f);
  tabControls->setWidth(static_cast<float>(portrait ? metrics.contentWidth : tabColumnWidth));
  tabControls->setFlexShrink(0.0f);
  auto makeTabButton = [&](SettingsTab tab, const i18n::Text &label,
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
      if (activeTab == SettingsTab::Display) {
        cancelDisplayPreviewForTabExit();
      }
      if (activeTab == SettingsTab::Input &&
          inputCaptureController != nullptr) {
        inputCaptureController->cancel();
        inputCaptureAction.reset();
        inputViewRebuildGate.reset();
        inputLastViewSignature.clear();
      }
      if (activeTab == SettingsTab::Profile && profileController != nullptr) {
        const auto phase = profileController->phase();
        if (phase == ProfileSettingsPhase::PickingImport ||
            phase == ProfileSettingsPhase::Importing ||
            phase == ProfileSettingsPhase::PreparingExport ||
            phase == ProfileSettingsPhase::PickingExport) {
          return;
        }
        profileInlineEditor.clear();
        profileController->cancelConfirmation();
      }
      activeTab = tab;
      if (activeTab == SettingsTab::GameplaySkins) {
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
        ensureGameplaySkinSettingsController();
#endif
      }
      lastLayoutWidth = -1;
    });
    return button;
  };
  profileTabButton =
      makeTabButton(SettingsTab::Profile, i18n::message("settings.navigation.profile.label"), &profileTabText);
  timingTabButton =
      makeTabButton(SettingsTab::Timing, i18n::message("settings.navigation.timing.label"), &timingTabText);
  visualTabButton =
      makeTabButton(SettingsTab::Visual, i18n::message("settings.navigation.visual.label"), &visualTabText);
  laneTabButton = makeTabButton(SettingsTab::Lane, i18n::message("settings.navigation.lane.label"), &laneTabText);
  inputTabButton = makeTabButton(SettingsTab::Input, i18n::message("settings.navigation.input.label"), &inputTabText);
  miscTabButton = makeTabButton(SettingsTab::Misc, i18n::message("settings.navigation.misc.label"), &miscTabText);
  audioTabButton = makeTabButton(SettingsTab::Audio, i18n::message("settings.navigation.audio.label"), &audioTabText);
  displayTabButton =
      makeTabButton(SettingsTab::Display, i18n::message("settings.navigation.display.label"), &displayTabText);
  difficultyTablesTabButton =
      makeTabButton(SettingsTab::DifficultyTables, i18n::message("settings.navigation.difficulty_tables.label"),
                    &difficultyTablesTabText);
  bmsLibraryTabButton =
      makeTabButton(SettingsTab::BmsLibrary, i18n::message("settings.navigation.bms_library.label"), &bmsLibraryTabText);
  gameplaySkinsTabButton = makeTabButton(SettingsTab::GameplaySkins,
                                         i18n::message("settings.navigation.skins.label"),
                                         &gameplaySkinsTabText);
  irTabButton = makeTabButton(SettingsTab::Ir, "IR", &irTabText);
  tabControls->addView(profileTabButton);
  tabControls->addView(inputTabButton);
  tabControls->addView(timingTabButton);
  tabControls->addView(laneTabButton);
  tabControls->addView(visualTabButton);
  tabControls->addView(gameplaySkinsTabButton);
  tabControls->addView(audioTabButton);
  tabControls->addView(displayTabButton);
  tabControls->addView(bmsLibraryTabButton);
  tabControls->addView(difficultyTablesTabButton);
  tabControls->addView(irTabButton);
  tabControls->addView(miscTabButton);
  auto *tabRail = new ScrollView();
  tabRail->setWidth(static_cast<float>(portrait ? metrics.contentWidth : tabColumnWidth));
  if (portrait) tabRail->setHeight(metrics.actionButtonHeight * 4.0F + 24.0F);
  tabRail->setFlexShrink(0.0f);
  tabRail->setContentView(tabControls);
  content->addView(tabRail);

  scrollView = new ScrollView();
  scrollView->setFlex(1.0f);
  scrollView->setContentPadding(Edge::Right,
                                static_cast<float>(scrollRightPadding));

  auto *scrollContent = new View();
  scrollContent->setFlexDirection(FlexDirection::Column);
  scrollContent->setGap(static_cast<float>(metrics.rootGap));

  overlayPortal = new OverlayPortal(0, 0, rendering::window_width,
                                    rendering::window_height);
  overlayPortal->setPositionType(YGPositionTypeAbsolute);
  overlayPortal->setPosition(Edge::Left, 0);
  overlayPortal->setPosition(Edge::Top, 0);
  overlayPortal->setZIndex(900);

  View *cardsColumn = nullptr;
  switch (activeTab) {
  case SettingsTab::Profile:
    cardsColumn = buildProfileTab(metrics);
    break;
  case SettingsTab::Timing:
    cardsColumn = buildTimingTab(metrics);
    break;
  case SettingsTab::Visual:
    cardsColumn = buildVisualTab(metrics);
    break;
  case SettingsTab::Lane:
    cardsColumn = buildLaneTab(metrics);
    break;
  case SettingsTab::Input:
    cardsColumn = buildInputTab(metrics);
    break;
  case SettingsTab::Misc:
    cardsColumn = buildMiscTab(metrics);
    break;
  case SettingsTab::Audio:
    cardsColumn = buildAudioTab(metrics);
    break;
  case SettingsTab::Display:
    cardsColumn = buildDisplayTab(metrics);
    break;
  case SettingsTab::DifficultyTables:
    cardsColumn = buildDifficultyTablesTab(metrics);
    break;
  case SettingsTab::BmsLibrary:
    cardsColumn = buildBmsLibraryTab(metrics);
    break;
  case SettingsTab::GameplaySkins:
    cardsColumn = buildGameplaySkinsTab(metrics);
    break;
  case SettingsTab::Ir:
    cardsColumn = buildIrTab(metrics);
    break;
  }
  if (cardsColumn != nullptr) {
    scrollContent->addView(cardsColumn);
  }

  scrollView->setContentView(scrollContent);
  content->addView(scrollView);
  rootLayout->addView(content);
  rootLayout->addView(overlayPortal);

  buildDifficultyTableImportModal(metrics);
  buildInputConflictOverlay(metrics);
  buildInputVirtualControllerEditorOverlay(metrics);
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  buildGameplaySkinSafetyOverlay(metrics);
#endif
  buildDisplayPreviewOverlay(metrics);

  rootLayout->applyYogaLayout();
  refreshDifficultyTableImportModal();
  refreshSettingsText();
}
