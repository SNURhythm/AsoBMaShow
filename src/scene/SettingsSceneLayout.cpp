#include "../view/KeybombPreviewView.h"
#include "../GameplayKeyMode.h"
#include "SettingsSceneShared.h"
#include "../i18n/Localization.h"
#include "../i18n/PlatformLocale.h"
#include "../BmsSearchService.h"
#include "../input/InputCaptureController.h"
#include "../view/BlockingOverlayView.h"
#include "../view/CheckboxButtonContent.h"
#include "../view/DropdownView.h"
#include "../view/IconText.h"
#include "../view/LaneCoverControlsView.h"
#include "../view/OverlayPortal.h"
#include "../view/ScrollView.h"
#include "play/BMSRenderer.h"
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
#include "GameplaySkinSettingsPresentation.h"
#endif
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
constexpr uint32_t kIconPlay = 0xf04b;
constexpr uint32_t kIconPause = 0xf04c;

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
  previewKeyModeDropdown = nullptr;
  for (auto *view : views) {
    delete view;
  }
  views.clear();
  rootLayout = nullptr;
  overlayPortal = nullptr;
  scrollView = nullptr;
  tabRailScrollView = nullptr;
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
  prepMetronomeMaxDurationInput = nullptr;
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
  hudTabButton = nullptr;
  backgroundTabButton = nullptr;
  laneTabButton = nullptr;
  inputTabButton = nullptr;
  generalTabButton = nullptr;
  audioTabButton = nullptr;
  displayTabButton = nullptr;
  difficultyTablesTabButton = nullptr;
  bmsLibraryTabButton = nullptr;
  gameplaySkinsTabButton = nullptr;
  irTabButton = nullptr;
  timingTabText = nullptr;
  hudTabText = nullptr;
  backgroundTabText = nullptr;
  laneTabText = nullptr;
  inputTabText = nullptr;
  generalTabText = nullptr;
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
  displayAntiAliasingDropdown = nullptr;
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
  gameplaySkinBusyOverlayTitleText = nullptr;
  gameplaySkinBusyOverlayStatusText = nullptr;
  gameplaySkinBusyOverlayStatusScroll = nullptr;
  gameplaySkinBusyOverlayActionText = nullptr;
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

  const bool preserveScroll = rootLayout != nullptr &&
                              activeTab == lastLaidOutTab &&
                              previewActive == lastLaidOutPreviewActive &&
                              (!previewActive || previewPanelPage == lastLaidOutPreviewPanelPage);
  if (!preserveScroll) {
    lastLayoutScrollOffset = 0.0F;
  } else if (scrollView != nullptr) {
    lastLayoutScrollOffset = scrollView->getScrollOffset();
  }
  // A folded preview has no scroll view; retain the last expanded offset.
  if (tabRailScrollView != nullptr) {
    lastTabRailScrollOffset = tabRailScrollView->getScrollOffset();
  }

  resetViewState();
  lastLayoutWidth = rendering::window_width;
  lastLayoutHeight = rendering::window_height;
  lastSafeTop = safe.top;
  lastSafeLeft = safe.left;
  lastSafeBottom = safe.bottom;
  lastSafeRight = safe.right;
  initView();
  lastLaidOutTab = activeTab;
  lastLaidOutPreviewActive = previewActive;
  lastLaidOutPreviewPanelPage = previewPanelPage;
  if (preserveScroll && scrollView != nullptr) {
    scrollView->setScrollOffset(lastLayoutScrollOffset);
  }
  if (tabRailScrollView != nullptr) {
    tabRailScrollView->setScrollOffset(lastTabRailScrollOffset);
  }
}

View *SettingsScene::buildVisibleTimeControls(const LayoutMetrics &metrics,
                                              bool includeDescription,
                                              bool compactAdjustments) {
  auto *visibleTimeControls = new View();
  visibleTimeControls->setFlexDirection(FlexDirection::Column);
  visibleTimeControls->setWidthPercent(100);
  visibleTimeControls->setGap(metrics.compact ? 12.0f : 16.0f);
  visibleTimeControls->setAlignItems(YGAlignFlexStart);
  if (includeDescription) {
    visibleTimeControls->addView(makeWrappedText(
        metrics.compact ? i18n::message("settings.visible_time_controls.note_duration.green_number_help")
                        : i18n::message("settings.visible_time_controls.green_number.unit_help"),
        metrics.bodyTextSize, app_theme::textSecondary()));
  }

  visibleTimeModeText =
      makeText("", metrics.bodyTextSize + 6, app_theme::textPrimary(),
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
    visibleTimeModeRow->setJustifyContent(YGJustifyFlexStart);
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
        metrics.smallTextSize, app_theme::textSecondary()));
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
                 app_theme::textSecondary(), TextView::LEFT, TextView::MIDDLE);
    fixedHispeedLabel->setMinWidth(0.0F);
    fixedHispeedLabel->setFlexShrink(1.0F);
    fixedHispeedChoices->addView(fixedHispeedLabel);
    for (const auto mode :
         {AppSettings::HiSpeedFixMode::Off, AppSettings::HiSpeedFixMode::Start,
          AppSettings::HiSpeedFixMode::Max, AppSettings::HiSpeedFixMode::Main,
          AppSettings::HiSpeedFixMode::Min}) {
      auto *choiceLabel = makeText(formatVisibleTimeBpmStrategyLabel(mode),
                                   metrics.smallTextSize, app_theme::textPrimary(),
                                   TextView::CENTER, TextView::MIDDLE);
      const int width =
          std::max(metrics.compact ? 84 : 96, choiceLabel->textureWidth() + 28);
      auto *choice =
          context.settings.hispeedFixMode == mode
              ? makeAccentButton(width, metrics.actionButtonHeight,
                                 choiceLabel,
                                 app_theme::cyan())
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
  if (compactAdjustments) {
    visibleTimeInput = makeTextInput(metrics, metrics.compact ? 116 : 136);
    visibleTimeInput->onEditingFinished(
        [this](const std::string &) { commitVisibleTimeInput(); });
    auto *reset = makeResetButton(metrics);
    reset->setOnClickListener([this]() {
      context.settings.setVisibleTimeGreenNumber(400);
      persistSettings();
      syncVisibleTimeInputText(true);
    });
    visibleTimeControls->addView(makeSteppedValueRow(metrics, visibleTimeInput, "10",
        [this](int direction) {
          context.settings.visibleTimeDurationMilliseconds = adjustVisibleTimeDurationMilliseconds(
              context.settings.visibleTimeDurationMilliseconds,
              context.settings.visibleTimeUseMilliseconds, direction * 10);
          persistSettings();
          syncVisibleTimeInputText(true);
        }, reset));
    return visibleTimeControls;
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

void SettingsScene::refreshPreviewKeyModeDropdown() {
  if (!previewKeyModeDropdown) return;
  std::vector<DropdownView::Option> options;
  for (const auto mode : kPreviewKeyModes)
    options.push_back({.id = std::to_string(mode), .label = gameplay::keyModeLabel(mode)});
  previewKeyModeDropdown->refresh({
      .label = i18n::message("settings.input.mode.label"),
      .selectedId = std::to_string(previewKeyMode),
      .options = std::move(options), .open = previewKeyModeDropdownOpen,
      .maxVisibleItems = 9});
}

void SettingsScene::buildPreviewLayout(const LayoutMetrics &metrics) {
  rootLayout->setFlexDirection(FlexDirection::Row);
  rootLayout->setJustifyContent(previewPanelOnLeft ? YGJustifyFlexStart : YGJustifyFlexEnd);
  rootLayout->setAlignItems(YGAlignFlexStart);

  bool previewHasSelectedSkin = false;
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  const skin::GameplaySkinEntryRow *previewSkinRow = nullptr;
  bool previewSkinActionsEnabled = false;
  if (gameplaySkinTraitsRuntimeAvailable()) {
    const auto &snapshot = gameplaySkinSettingsController->snapshot();
    previewSkinRow = skin::gameplaySkinSettingsEntryForKeyMode(snapshot, previewKeyMode);
    previewHasSelectedSkin = previewSkinRow != nullptr;
    previewSkinActionsEnabled =
        skin::gameplaySkinSettingsActionAvailability(snapshot).ordinaryActions;
    gameplaySkinControlsBuiltDisabled = !previewSkinActionsEnabled;
  }
#endif

  const int foldButtonSize = metrics.compact ? 54 : 58;
  const int previewPanelPageCount = previewHasSelectedSkin ? 3 : 4;
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
  previewPanel->setThemedBackgroundColor(app_theme::panel);
  previewPanel->setCornerRadius(app_theme::panelRadius());
  previewPanel->setThemedBorderColor(app_theme::hairline);
  previewPanel->setBorderWidth(1);

  auto makeFoldButton = [this, foldButtonSize](const i18n::Text &label) {
    auto *button =
        makeControlButton(foldButtonSize, foldButtonSize,
                          makeText(label, 18, app_theme::textPrimary(),
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
      makeHeading(i18n::message("settings.preview_layout.preview.label"), metrics.sectionTitleSize));

  auto *headerActions = new View();
  headerActions->setFlexDirection(FlexDirection::Row);
  headerActions->setAlignItems(YGAlignCenter);
  headerActions->setGap(metrics.compact ? 8.0F : 10.0F);
  auto *movePanel = makeControlButton(metrics.compact ? 108 : 120, foldButtonSize,
      makeText(i18n::message(previewPanelOnLeft
          ? "settings.preview_layout.move_right.label" : "settings.preview_layout.move_left.label"),
          metrics.smallTextSize, app_theme::textPrimary(), TextView::CENTER, TextView::MIDDLE));
  movePanel->setOnClickListener([this]() {
    previewPanelOnLeft = !previewPanelOnLeft;
    lastLayoutWidth = -1;
  });
  headerActions->addView(movePanel);
  headerActions->addView(makeFoldButton(i18n::message("settings.preview_layout.hide.label")));
  previewHeader->addView(headerActions);
  previewPanel->addView(previewHeader);
  auto *previewSelectionRow = new View();
  previewSelectionRow->setFlexDirection(FlexDirection::Row);
  previewSelectionRow->setAlignItems(YGAlignCenter);
  const float selectionGap = metrics.compact ? 8.0F : 10.0F;
  const float selectionWidth = static_cast<float>(panelWidth - metrics.cardPadding * 2);
  previewSelectionRow->setGap(selectionGap);
  previewKeyModeDropdown = new DropdownView({
      .onOpenChanged = [this](bool open) {
        previewKeyModeDropdownOpen = open;
        refreshPreviewKeyModeDropdown();
      },
      .onOptionSelected = [this](const std::string &id) {
        const auto mode = std::stoi(id);
        previewKeyModeDropdownOpen = false;
        if (previewKeyMode != mode) {
          previewKeyMode = mode;
          previewRendererDirty = true;
          lastLayoutWidth = -1;
        }
        refreshPreviewKeyModeDropdown();
      }}, overlayPortal);
  previewKeyModeDropdown->setTriggerWidth(selectionWidth);
  refreshPreviewKeyModeDropdown();
  previewSelectionRow->addView(previewKeyModeDropdown);
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  if (gameplaySkinTraitsRuntimeAvailable()) {
    if (const auto target = skin::gameplaySkinTargetForKeyMode(previewKeyMode)) {
      const auto selection = skin::gameplaySkinSelectionForTarget(
          gameplaySkinSettingsController->snapshot(), *target);
      const float keyModeWidth = std::min(220.0F, selectionWidth * 0.4F);
      previewKeyModeDropdown->setTriggerWidth(keyModeWidth);
      const float dropdownWidth = selectionWidth - keyModeWidth - selectionGap;
      auto *skinDropdown = buildGameplaySkinSelectionDropdown(
          metrics, target->skinType, selection, previewSkinActionsEnabled, dropdownWidth);
      skinDropdown->setTriggerWidth(dropdownWidth);
      previewSelectionRow->addView(skinDropdown);
    }
  }
#endif
  previewPanel->addView(previewSelectionRow);
  auto *playbackControls = new View();
  playbackControls->setFlexDirection(FlexDirection::Row);
  playbackControls->setGap(selectionGap);
  const int pauseButtonSize = metrics.actionButtonHeight;
  const int playbackToggleWidth =
      static_cast<int>((selectionWidth - pauseButtonSize - selectionGap * 2) / 2);
  const auto addPlaybackToggle = [&](const char *labelKey, bool selected, bool enabled,
                                      std::function<void()> action) {
    auto *label = makeText(i18n::message(labelKey, {{"state", i18n::tr(selected
        ? "settings.skins.judgement_hud.on.label" : "settings.skins.judgement_hud.off.label")}}),
        metrics.smallTextSize, app_theme::textPrimary(), TextView::CENTER, TextView::MIDDLE);
    auto *button = selected
        ? makeAccentButton(playbackToggleWidth, metrics.actionButtonHeight, label, app_theme::cyan())
        : makeControlButton(playbackToggleWidth, metrics.actionButtonHeight, label);
    button->setEnabled(enabled);
    button->setOnClickListener([this, action = std::move(action)]() {
      action();
      resetPreviewSimulation();
      lastLayoutWidth = -1;
    });
    playbackControls->addView(button);
  };
  addPlaybackToggle("settings.preview_layout.autoplay.label", previewAutoPlay, true,
                     [this]() { previewAutoPlay = !previewAutoPlay; });
  addPlaybackToggle("settings.preview_layout.random_timing.label", previewRandomTiming,
                     previewAutoPlay, [this]() { previewRandomTiming = !previewRandomTiming; });
  auto *pauseIcon = new TextView(ui_icons::kFontAwesomeSolidPath, metrics.smallTextSize);
  pauseIcon->setText(ui_icons::textForCodepoint(previewPaused ? kIconPlay : kIconPause));
  pauseIcon->setAlign(TextView::CENTER);
  pauseIcon->setVAlign(TextView::MIDDLE);
  pauseIcon->setThemedColor(app_theme::textPrimary);
  auto *pauseButton = previewPaused
      ? makeAccentButton(pauseButtonSize, pauseButtonSize, pauseIcon, app_theme::cyan())
      : makeControlButton(pauseButtonSize, pauseButtonSize, pauseIcon);
  pauseButton->setOnClickListener([this]() {
    previewPaused = !previewPaused;
    lastLayoutWidth = -1;
  });
  playbackControls->addView(pauseButton);
  previewPanel->addView(playbackControls);
  if (previewAutoPlay && previewRandomTiming)
    previewPanel->addView(makeWrappedText(
        i18n::message("settings.preview_layout.random_timing.description"),
        metrics.smallTextSize, app_theme::textSecondary()));
  previewPanel->addView(makeWrappedText(
      previewError.empty()
          ? i18n::message(previewPresentation && previewPresentation->activeMode() == PresentationMode::Skin
                ? "settings.preview_layout.skin_selected.label" : "settings.preview_layout.skin_builtin.label")
          : i18n::message("settings.preview_layout.skin_error.message", {{"error", previewError}}),
      metrics.smallTextSize, app_theme::textSecondary()));

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
        makeText(label, metrics.bodyTextSize + 2, app_theme::textPrimary(),
                 TextView::CENTER, TextView::MIDDLE);
    auto *button =
        previewPanelPage == page
            ? makeAccentButton(previewTabWidth, metrics.actionButtonHeight,
                               labelText, app_theme::cyan())
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
  previewTabs->addView(makePreviewTab(1, i18n::message(previewHasSelectedSkin
      ? "settings.skins.skin.label" : "settings.preview_layout.lane.label")));
  previewTabs->addView(makePreviewTab(2, "HUD"));
  if (!previewHasSelectedSkin)
    previewTabs->addView(makePreviewTab(3, i18n::message("settings.notes.title")));
  previewPanel->addView(previewTabs);

  auto *previewScroll = new ScrollView();
  scrollView = previewScroll;
  previewScroll->setFlex(1.0f);
  previewScroll->setFlexShrink(1.0f);
  previewScroll->setWidthPercent(100.0f);
  previewScroll->setContentPadding(Edge::Right,
                                   metrics.compact ? 10.0f : 12.0f);

  auto *previewControls = new View();
  previewControls->setFlexDirection(FlexDirection::Column);
  previewControls->setGap(metrics.compact ? 12.0f : 16.0f);
  previewControls->setAlignItems(YGAlignFlexStart);
  previewScroll->setContentView(previewControls);
  previewPanel->addView(previewScroll);

  if (previewPanelPage == 0) {
    previewControls->addView(makeWrappedText(
        i18n::message("settings.preview_layout.visible_time.label"),
        metrics.bodyTextSize, app_theme::textPrimary()));
    previewControls->addView(buildVisibleTimeControls(metrics, false, true));

    previewControls->addView(makeWrappedText(
        i18n::message("settings.preview_layout.note_start.label"),
        metrics.bodyTextSize, app_theme::textPrimary()));
    constexpr std::array coverLabels{"SUDDEN+", "HIDDEN+", "LIFT"};
    for (std::size_t index = 0; index < coverLabels.size(); ++index) {
      const auto read = [this, index]() {
        const auto state = context.settings.presentation().laneCoverState();
        return static_cast<int>(std::lround(index == 0 ? state.laneCoverPercent * 10
            : index == 1 ? state.hiddenRatio * 1000 : state.liftRatio * 1000));
      };
      const auto apply = [this, index](int amount) {
        auto state = context.settings.presentation().laneCoverState();
        amount = std::clamp(amount, 0, 1000);
        if (index == 0) state.laneCoverPercent = static_cast<float>(amount) / 10;
        else if (index == 1) state.hiddenRatio = static_cast<float>(amount) / 1000;
        else state.liftRatio = static_cast<float>(amount) / 1000;
        context.settings.presentation().setLaneCoverState(state);
        persistSettings();
      };
      auto *content = new CheckboxButtonContent(coverLabels[index], metrics.bodyTextSize, 20);
      content->setThemedColor(app_theme::textPrimary);
      const auto state = context.settings.presentation().laneCoverState();
      const bool enabled = index == 0 ? state.laneCoverEnabled
          : index == 1 ? state.hiddenEnabled : state.liftEnabled;
      content->setChecked(enabled);
      auto *toggle = new Button(0, 0, metrics.actionButtonWidth, metrics.actionButtonHeight);
      toggle->setCornerRadius(app_theme::controlRadius());
      toggle->setThemedBackgroundColors(app_theme::control, app_theme::controlHover, app_theme::controlPressed);
      toggle->setContentView(content);
      toggle->setSelected(enabled);
      toggle->setOnClickListener([this, index, toggle, content]() {
        auto value = context.settings.presentation().laneCoverState();
        bool &enabled = index == 0 ? value.laneCoverEnabled
            : index == 1 ? value.hiddenEnabled : value.liftEnabled;
        enabled = !enabled;
        content->setChecked(enabled);
        toggle->setSelected(enabled);
        context.settings.presentation().setLaneCoverState(value);
        persistSettings();
      });
      previewControls->addView(toggle);
      auto *input = makeTextInput(metrics, metrics.compact ? 116 : 136);
      input->setEditingText(std::to_string(read()));
      input->onEditingFinished([input, read, apply](const std::string &text) {
        apply(sanitizeOffsetComponent(text, read()));
        input->setEditingText(std::to_string(read()));
      });
      auto *reset = makeResetButton(metrics);
      reset->setOnClickListener([input, read, apply, index]() {
        apply(index == 0 ? 0 : 100);
        input->setEditingText(std::to_string(read()));
      });
      previewControls->addView(makeSteppedValueRow(metrics, input, "10",
          [input, read, apply](int direction) {
            apply(read() + direction * 10);
            input->setEditingText(std::to_string(read()));
          }, reset));
    }
    previewControls->addView(makeWrappedText(
        i18n::message("settings.lane.covers.shortcuts"),
        metrics.smallTextSize, app_theme::textSecondary()));
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  } else if (previewPanelPage == 1 && previewSkinRow != nullptr) {
    previewControls->addView(makeWrappedText(
        previewSkinRow->metadata.displayName.empty()
            ? previewSkinRow->entry.packageRelativePath
            : previewSkinRow->metadata.displayName,
        metrics.bodyTextSize, app_theme::cyan()));
    appendGameplaySkinCatalogSettings(previewControls, metrics,
                                       *previewSkinRow, previewSkinActionsEnabled);
    appendGameplaySkinViewportSettings(previewControls, metrics,
                                        *previewSkinRow, previewSkinActionsEnabled);
#endif
  } else if (previewPanelPage == 1) {
    if (previewKeyMode == 5 || previewKeyMode == 7 ||
        previewKeyMode == -5 || previewKeyMode == -7) {
      previewControls->addView(buildScratchLanePositionControl(metrics));
    }
    appendBuiltInLaneGeometryControls(previewControls, metrics, previewKeyMode);
    appendBuiltInLaneOpacityControls(previewControls, metrics, previewKeyMode);
    appendBuiltInJudgeLineControls(previewControls, metrics, previewKeyMode);
    appendBuiltInMeasureLineControls(previewControls, metrics, previewKeyMode);
    appendBuiltInBeamLengthControls(previewControls, metrics);

    previewControls->addView(buildKeybombControls(metrics, previewKeyMode, true));
  } else if (previewPanelPage == 3 && !previewHasSelectedSkin) {
    appendBuiltInNoteControls(previewControls, metrics, previewKeyMode);
  } else {
    appendSelectedSkinHudSettings(previewControls, metrics, !previewHasSelectedSkin);
  }

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  if (gameplaySkinTraitsRuntimeAvailable()) {
    gameplaySkinUiMessageText = makeWrappedText(gameplaySkinUiMessage,
        metrics.smallTextSize, app_theme::textSecondary());
    previewControls->addView(gameplaySkinUiMessageText);
  }
#endif

  auto *restartButton = makeButton(
      metrics.actionButtonWidth, metrics.actionButtonHeight,
      makeText(i18n::message("settings.preview_layout.restart.label"), metrics.bodyTextSize + 4, app_theme::textPrimary(),
               TextView::CENTER, TextView::MIDDLE),
      app_theme::control(), app_theme::controlHover(), app_theme::controlPressed(),
      app_theme::hairline(), app_theme::cyan(), app_theme::cyan());
  restartButton->setOnClickListener([this]() { resetPreviewSimulation(); });

  auto *doneButton = makeButton(
      metrics.actionButtonWidth, metrics.actionButtonHeight,
      makeText(i18n::message("settings.preview_layout.done.label"), metrics.bodyTextSize + 4, app_theme::textPrimary(),
               TextView::CENTER, TextView::MIDDLE),
      app_theme::control(), app_theme::controlHover(), app_theme::controlPressed(),
      app_theme::hairline(), app_theme::cyan(), app_theme::cyan());
  doneButton->setOnClickListener([this]() { stopLanePreview(); });

  auto *previewActions = new View();
  previewActions->setFlexDirection(FlexDirection::Row);
  previewActions->setFlexWrap(YGWrapNoWrap);
  for (auto *button : {restartButton, doneButton}) {
    button->setWidth(0)->setMinWidth(0)->setFlex(1);
  }
  previewActions->setGap(metrics.compact ? 12.0f : 10.0f);
  previewActions->setAlignItems(YGAlignCenter);
  previewActions->setWidthPercent(100.0f);
  previewActions->setJustifyContent(YGJustifyFlexStart);
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
  row->setFlexDirection(FlexDirection::Column);
  row->setAlignItems(YGAlignFlexStart);
  row->setJustifyContent(YGJustifyFlexStart);
  row->setGap(12);
  auto *label = makeWrappedText(i18n::message("settings.skins.scratch_position.label"),
                                metrics.smallTextSize, app_theme::textSecondary());
  label->setFlexShrink(1)->setMinWidth(0);
  row->addView(label);
  auto *value = makeText("", metrics.bodyTextSize, app_theme::textPrimary(),
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
                                                            bool /*previewStyle*/) {
  auto *body = new View();
  body->setFlexDirection(FlexDirection::Column);
  body->setWidthPercent(100);
  body->setGap(metrics.compact ? 12.0f : 16.0f);
  body->setAlignItems(YGAlignFlexStart);
  const auto appendPosition = [this, body, &metrics](const i18n::Text &label,
      float AppSettings::PresentationSettings::*member) {
    body->addView(makeWrappedText(label, metrics.smallTextSize, app_theme::textSecondary()));
    auto *input = makeTextInput(metrics, metrics.compact ? 116 : 136);
    const auto refresh = [this, input, member]() {
      input->setEditingText(std::to_string(judgementTextYToPercent(context.settings.presentation().*member)));
    };
    refresh();
    input->onEditingFinished([this, member, refresh](const std::string &text) {
      auto &value = context.settings.presentation().*member;
      value = judgementTextPercentToY(sanitizeOffsetComponent(text, judgementTextYToPercent(value)));
      refresh();
      persistSettings();
    });
    auto *reset = makeResetButton(metrics);
    reset->setOnClickListener([this, member, refresh]() {
      const AppSettings::PresentationSettings defaults(context.settings.activePresentationOrientation());
      context.settings.presentation().*member = defaults.*member;
      refresh();
      persistSettings();
    });
    body->addView(makeSteppedValueRow(metrics, input, "5%",
        [this, member, refresh](int direction) {
          auto &value = context.settings.presentation().*member;
          value = judgementTextPercentToY(judgementTextYToPercent(value) + direction * 5);
          refresh();
          persistSettings();
        }, reset));
  };
  appendPosition(i18n::message("settings.preview_layout.judge_text_y.label"),
                 &AppSettings::PresentationSettings::judgementTextY);
  if (context.settings.presentation().judgementComboSeparated) {
    appendPosition(i18n::message("settings.feedback.combo_y.label"),
                   &AppSettings::PresentationSettings::comboTextY);
  }
  appendPosition(i18n::message("settings.feedback.timing_y.label"),
                 &AppSettings::PresentationSettings::judgementTimingY);
  appendPosition(i18n::message("settings.feedback.pacemaker_y.label"),
                 &AppSettings::PresentationSettings::pacemakerDiffY);
  return body;
}

View *SettingsScene::buildKeybombControls(const LayoutMetrics &metrics, int keyMode, bool previewStyle) {
  if (keyMode == -5 && context.settings.presentation().skin.follow5K1S) keyMode = 5;
  if (keyMode == -7 && context.settings.presentation().skin.follow7K1S) keyMode = 7;
  auto *body = new View();
  body->setWidthPercent(100);
  body->setFlexDirection(FlexDirection::Column);
  body->setGap(8.0F);
  body->setAlignItems(YGAlignFlexStart);
  if (previewStyle)
    body->addView(makeWrappedText(i18n::message("settings.keybomb.title"),
                                  metrics.bodyTextSize, app_theme::textPrimary()));
  auto *selector = new DropdownView({
      .onOptionSelected = [this](const std::string &id) {
        keybombEditingJudgement = static_cast<Judgement>(std::stoi(id));
        lastLayoutWidth = -1;
      }}, overlayPortal);
  std::vector<DropdownView::Option> options;
  for (const auto &option : player_settings::kJudgementTextVisibilityOptions) {
    if (option.judgement == Poor) continue;
    options.push_back({.id = std::to_string(option.judgement), .label = option.label});
  }
  selector->setWidthPercent(100);
  selector->refresh({.label = i18n::message("settings.keybomb.judgement"),
      .selectedId = std::to_string(keybombEditingJudgement), .options = std::move(options)});
  body->addView(selector);
  const auto judgement = keybombEditingJudgement;
  body->addView(makeWrappedText(i18n::message("settings.keybomb.preview"),
                                metrics.smallTextSize, app_theme::textSecondary()));
  auto *sample = new KeybombPreviewView(judgement, [this, keyMode, judgement]() {
    auto styles = context.settings.keybombsForKeyMode(keyMode);
    if (appearanceColorPreview) {
      PlayfieldPresentationConfig draft;
      draft.keybombs = styles;
      appearanceColorPreview(draft);
      styles = draft.keybombs;
    }
    return styles[judgement];
  });
  sample->setWidthPercent(100);
  sample->setHeight(previewStyle ? 160 : 220);
  sample->setFlexShrink(0);
  body->addView(sample);
  auto *visibleText = makeText("", metrics.bodyTextSize, app_theme::textPrimary(),
                                TextView::CENTER, TextView::MIDDLE);
  auto *visibleButton = makeControlButton(metrics.actionButtonWidth,
                                           metrics.actionButtonHeight, visibleText);
  const auto refreshVisible = [this, keyMode, judgement, visibleText, visibleButton]() {
    const bool visible = context.settings.keybombsForKeyMode(keyMode)[judgement].visible;
    visibleText->setLocalizedText(i18n::message(visible ? "settings.skins.visible.label"
                                                      : "settings.skins.hidden.label"));
    styleVisibilityButton(visibleButton, visibleText, visible);
  };
  refreshVisible();
  visibleButton->setOnClickListener([this, keyMode, judgement, refreshVisible]() {
    auto &visible = context.settings.presentation().keybombs[keyMode][judgement].visible;
    visible = !visible;
    refreshVisible();
    persistSettings();
  });
  body->addView(visibleButton);
  const auto appendPercent = [&](const i18n::Text &label, int keybomb::Style::*member,
                                  int minimum, int maximum) {
    body->addView(makeWrappedText(label, metrics.smallTextSize, app_theme::textSecondary()));
    auto *input = makeTextInput(metrics, metrics.compact ? 116 : 136);
    const auto refresh = [this, keyMode, judgement, member, input]() {
      input->setEditingText(std::to_string(context.settings.keybombsForKeyMode(keyMode)[judgement].*member));
    };
    refresh();
    input->onEditingFinished([this, keyMode, judgement, member, minimum, maximum, refresh](const std::string &text) {
      auto &value = context.settings.presentation().keybombs[keyMode][judgement].*member;
      value = std::clamp(sanitizeOffsetComponent(text, value), minimum, maximum);
      refresh();
      persistSettings();
    });
    auto *reset = makeResetButton(metrics);
    reset->setOnClickListener([this, keyMode, judgement, member, refresh]() {
      context.settings.presentation().keybombs[keyMode][judgement].*member = keybomb::Style{}.*member;
      refresh();
      persistSettings();
    });
    body->addView(makeSteppedValueRow(metrics, input, "5%",
        [this, keyMode, judgement, member, minimum, maximum, refresh](int direction) {
          auto &value = context.settings.presentation().keybombs[keyMode][judgement].*member;
          value = std::clamp(value + direction * 5, minimum, maximum);
          refresh();
          persistSettings();
        }, reset));
  };
  appendKeybombColorControls(body, metrics, keyMode, judgement);
  appendPercent(i18n::message("settings.keybomb.size"), &keybomb::Style::sizePercent, 25, 300);
  appendPercent(i18n::message("settings.lane_background.opacity"), &keybomb::Style::opacityPercent, 0, 100);
  return body;
}

View *SettingsScene::buildJudgementFeedbackStyleControls(const LayoutMetrics &metrics,
                                                         bool previewStyle) {
  auto *body = new View();
  body->setFlexDirection(FlexDirection::Column);
  body->setWidthPercent(100);
  body->setGap(previewStyle ? (metrics.compact ? 12.0f : 16.0f)
                            : (metrics.compact ? 8.0f : 12.0f));
  body->setAlignItems(YGAlignFlexStart);
  const auto appendStyle = [this, body, &metrics](const i18n::Text &label,
      int AppSettings::PresentationSettings::*sizeMember,
      bool AppSettings::PresentationSettings::*boldMember) {
    body->addView(makeWrappedText(label, metrics.smallTextSize, app_theme::textSecondary()));
    auto *input = makeTextInput(metrics, metrics.compact ? 116 : 136);
    const auto refreshSize = [this, input, sizeMember]() {
      input->setEditingText(std::to_string(context.settings.presentation().*sizeMember));
    };
    refreshSize();
    input->onEditingFinished([this, sizeMember, refreshSize](const std::string &text) {
      auto &value = context.settings.presentation().*sizeMember;
      value = std::clamp(sanitizeOffsetComponent(text, value), AppSettings::kMinJudgementFeedbackSizePercent,
                         AppSettings::kMaxJudgementFeedbackSizePercent);
      refreshSize();
      persistSettings();
    });
    auto *weightText = makeText("", metrics.bodyTextSize, app_theme::textPrimary(),
                                TextView::CENTER, TextView::MIDDLE);
    const auto refreshWeight = [this, weightText, boldMember]() {
      weightText->setLocalizedText(i18n::message(context.settings.presentation().*boldMember
          ? "settings.skins.feedback.bold.label" : "settings.skins.feedback.regular.label"));
    };
    refreshWeight();
    auto *weight = makeControlButton(metrics.compact ? 132 : 156,
                                     metrics.actionButtonHeight, weightText);
    weight->setOnClickListener([this, boldMember, refreshWeight]() {
      auto &bold = context.settings.presentation().*boldMember;
      bold = !bold;
      refreshWeight();
      persistSettings();
    });
    auto *reset = makeResetButton(metrics);
    reset->setOnClickListener([this, sizeMember, boldMember, refreshSize, refreshWeight]() {
      const AppSettings::PresentationSettings defaults;
      context.settings.presentation().*sizeMember = defaults.*sizeMember;
      context.settings.presentation().*boldMember = defaults.*boldMember;
      refreshSize();
      refreshWeight();
      persistSettings();
    });
    body->addView(makeSteppedValueRow(metrics, input, "5%",
        [this, sizeMember, refreshSize](int direction) {
          auto &value = context.settings.presentation().*sizeMember;
          value = std::clamp(value + direction * 5, AppSettings::kMinJudgementFeedbackSizePercent,
                             AppSettings::kMaxJudgementFeedbackSizePercent);
          refreshSize();
          persistSettings();
        }, reset));
    body->addView(weight);
  };
  auto *separationRow = new View();
  separationRow->setWidthPercent(100);
  separationRow->setFlexDirection(FlexDirection::Column);
  separationRow->setAlignItems(YGAlignFlexStart);
  separationRow->setJustifyContent(YGJustifyFlexStart);
  separationRow->setGap(8.0F);
  auto *separationLabel = makeWrappedText(
      i18n::message("settings.skins.feedback.combo_layout.label"),
      metrics.bodyTextSize, app_theme::textPrimary());
  separationLabel->setFlexShrink(1)->setMinWidth(0);
  separationRow->addView(separationLabel);
  auto *separationText = makeText(i18n::message(
      context.settings.presentation().judgementComboSeparated
          ? "settings.skins.feedback.combo_layout.separated.label"
          : "settings.skins.feedback.combo_layout.combined.label"),
      metrics.bodyTextSize, app_theme::textPrimary(), TextView::CENTER, TextView::MIDDLE);
  auto *separationToggle = makeControlButton(metrics.compact ? 132 : 156,
                                              metrics.actionButtonHeight, separationText);
  separationToggle->setOnClickListener([this]() {
    auto &separated = context.settings.presentation().judgementComboSeparated;
    separated = !separated;
    persistSettings();
    lastLayoutWidth = -1;
  });
  separationRow->addView(separationToggle);
  body->addView(separationRow);
  auto *visibilityLabel = makeWrappedText(
      i18n::message("settings.skins.feedback.judgement_visibility.label"),
      metrics.smallTextSize, app_theme::textSecondary());
  body->addView(visibilityLabel);
  auto *visibilityControls = new View();
  visibilityControls->setWidthPercent(100);
  visibilityControls->setFlexDirection(FlexDirection::Row);
  visibilityControls->setFlexWrap(YGWrapWrap);
  visibilityControls->setGap(8.0F);
  visibilityControls->setJustifyContent(YGJustifyFlexStart);
  const auto appendVisibility = [this, visibilityControls, &metrics](
      const i18n::Text &label, bool player_settings::JudgementTextVisibility::*member) {
    auto *text = makeText("", metrics.bodyTextSize, app_theme::textPrimary(),
                          TextView::CENTER, TextView::MIDDLE);
    auto *toggle = makeControlButton(metrics.compact ? 180 : 200,
                                     metrics.actionButtonHeight, text);
    const auto refresh = [this, text, toggle, label, member]() {
      const bool visible = context.settings.presentation().judgementTextVisibility.*member;
      text->setLocalizedText(i18n::message("settings.skins.feedback.judgement_visibility.value",
          {{"judgement", label},
           {"state", i18n::message(visible ? "settings.skins.visible.label"
                                         : "settings.skins.hidden.label")}}));
      styleVisibilityButton(toggle, text, visible);
    };
    refresh();
    toggle->setOnClickListener([this, member, refresh]() {
      auto &visible = context.settings.presentation().judgementTextVisibility.*member;
      visible = !visible;
      refresh();
      persistSettings();
    });
    visibilityControls->addView(toggle);
  };
  for (const auto &option : player_settings::kJudgementTextVisibilityOptions) {
    appendVisibility(option.label, option.member);
  }
  appendVisibility(i18n::message("settings.skins.feedback.combo.label"),
                   &player_settings::JudgementTextVisibility::combo);
  body->addView(visibilityControls);
  appendStyle(i18n::message("settings.skins.feedback.judgement_size.percent_label"),
              &AppSettings::PresentationSettings::judgementTextSizePercent,
              &AppSettings::PresentationSettings::judgementTextBold);
  if (context.settings.presentation().judgementComboSeparated) {
    appendStyle(i18n::message("settings.skins.feedback.combo_size.percent_label"),
                &AppSettings::PresentationSettings::comboTextSizePercent,
                &AppSettings::PresentationSettings::comboTextBold);
  }
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
  offsetValue->setThemedBackgroundColor(app_theme::control);
  offsetValue->setCornerRadius(app_theme::controlRadius());
  offsetValue->setThemedBorderColor(app_theme::hairline);
  offsetValue->setBorderWidth(1);
  offsetInput = new TextInputBox(kFontPath, metrics.bodyTextSize + 6);
  offsetInput->setText("");
  offsetInput->setSize(metrics.offsetValueWidth, metrics.actionButtonHeight);
  offsetInput->setBackgroundColor(Color(0, 0, 0, 0));
  offsetInput->setBorderWidth(0);
  offsetInput->setAlign(TextView::CENTER);
  offsetInput->setVAlign(TextView::MIDDLE);
  offsetInput->setThemedColor(app_theme::textPrimary);
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
  visualOffsetValue->setThemedBackgroundColor(app_theme::control);
  visualOffsetValue->setCornerRadius(app_theme::controlRadius());
  visualOffsetValue->setThemedBorderColor(app_theme::hairline);
  visualOffsetValue->setBorderWidth(1);
  visualOffsetInput = new TextInputBox(kFontPath, metrics.bodyTextSize + 6);
  visualOffsetInput->setText("");
  visualOffsetInput->setSize(metrics.offsetValueWidth,
                             metrics.actionButtonHeight);
  visualOffsetInput->setBackgroundColor(Color(0, 0, 0, 0));
  visualOffsetInput->setBorderWidth(0);
  visualOffsetInput->setAlign(TextView::CENTER);
  visualOffsetInput->setVAlign(TextView::MIDDLE);
  visualOffsetInput->setThemedColor(app_theme::textPrimary);
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

  return cardsColumn;
}

View *SettingsScene::buildHudTab(const LayoutMetrics &metrics) {
  auto *cardsColumn = makeCardsColumn(metrics);
  cardsColumn->addView(makeWrappedText(i18n::message("settings.hud.scope_help"),
      metrics.smallTextSize, app_theme::textSecondary()));
  auto *judgementFeedbackControls = new View();
  judgementFeedbackControls->setFlexDirection(FlexDirection::Column);
  judgementFeedbackControls->setGap(metrics.compact ? 12.0f : 16.0f);
  judgementFeedbackControls->setAlignItems(YGAlignFlexStart);
  judgementFeedbackControls->addView(buildJudgementFeedbackStyleControls(metrics));
  judgementFeedbackControls->addView(buildJudgementFeedbackPositionControls(metrics));

  auto *timingCriteriaControls = new View();
  timingCriteriaControls->setFlexDirection(FlexDirection::Row);
  timingCriteriaControls->setFlexWrap(YGWrapWrap);
  timingCriteriaControls->setGap(metrics.compact ? 8.0f : 12.0f);
  timingCriteriaControls->setAlignItems(YGAlignFlexStart);

  auto *timingFastSlowGroup = new View();
  timingFastSlowGroup->setFlexDirection(FlexDirection::Column);
  timingFastSlowGroup->setGap(metrics.compact ? 6.0f : 8.0f);
  timingFastSlowGroup->addView(
      makeText("FAST/SLOW", metrics.bodyTextSize, app_theme::textSecondary()));
  judgementTimingFastSlowCriteriaText =
      makeText("", metrics.bodyTextSize + 4, app_theme::textPrimary(),
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
      i18n::message("settings.timing.milliseconds.label"), metrics.bodyTextSize, app_theme::textSecondary()));
  judgementTimingMillisecondsCriteriaText =
      makeText("", metrics.bodyTextSize + 4, app_theme::textPrimary(),
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
      makeText("", metrics.bodyTextSize + 6, app_theme::textPrimary(),
               TextView::CENTER, TextView::MIDDLE);
  judgementIndicatorModeButton =
      makeAccentButton(metrics.actionButtonWidth, metrics.actionButtonHeight,
                       judgementIndicatorModeText, app_theme::lime());
  judgementIndicatorModeButton->setOnClickListener([this]() {
    context.settings.presentation().judgementIndicatorEnabled =
        !context.settings.presentation().judgementIndicatorEnabled;
    persistSettings();
  });
  judgementIndicatorModeControls->addView(judgementIndicatorModeButton);

  judgementIndicatorRenderModeText =
      makeText("", metrics.bodyTextSize + 6, app_theme::textPrimary(),
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
      makeText(i18n::message("settings.timing.y_position.label"), metrics.bodyTextSize, app_theme::textSecondary()));
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
      makeText(i18n::message("settings.timing.width.label"), metrics.bodyTextSize, app_theme::textSecondary()));
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
      i18n::message("settings.timing.range_ms.label"), metrics.bodyTextSize, app_theme::textSecondary()));
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
      makeText("", metrics.bodyTextSize + 6, app_theme::textPrimary(),
               TextView::CENTER, TextView::MIDDLE);
  judgementCounterModeButton =
      makeAccentButton(metrics.actionButtonWidth, metrics.actionButtonHeight,
                       judgementCounterModeText, app_theme::lime());
  judgementCounterModeButton->setOnClickListener([this]() {
    context.settings.presentation().judgementCounterEnabled =
        !context.settings.presentation().judgementCounterEnabled;
    persistSettings();
  });
  judgementCounterModeControls->addView(judgementCounterModeButton);
  judgementCounterPositionText =
      makeText("", metrics.bodyTextSize + 6, app_theme::textPrimary(),
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
      metrics.bodyTextSize, app_theme::textSecondary()));
  auto *gaugePositionControls = new View();
  gaugePositionControls->setFlexDirection(FlexDirection::Row);
  gaugePositionControls->setFlexWrap(YGWrapWrap);
  gaugePositionControls->setGap(metrics.compact ? 8.0f : 10.0f);
  gaugePositionControls->setAlignItems(YGAlignFlexStart);
  gaugeBarPositionText =
      makeText("", metrics.bodyTextSize + 6, app_theme::textPrimary(),
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

  return cardsColumn;
}

View *SettingsScene::buildBackgroundTab(const LayoutMetrics &metrics) {
  auto *cardsColumn = makeCardsColumn(metrics);
  auto *bgaControls = new View();
  bgaControls->setFlexDirection(FlexDirection::Column);
  bgaControls->setGap(metrics.compact ? 12.0f : 16.0f);
  bgaControls->setAlignItems(YGAlignFlexStart);
  bgaModeText = makeText("", metrics.bodyTextSize + 6, app_theme::textPrimary(),
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

  auto *bgaDisplayControls = new View();
  bgaDisplayControls->setFlexDirection(FlexDirection::Column);
  bgaDisplayControls->setGap(metrics.compact ? 12.0f : 16.0f);
  bgaDisplayControls->setAlignItems(YGAlignFlexStart);
  bgaDisplayControls->addView(makeWrappedText(
      i18n::message("settings.visual.fit_shows_all_fill_crops_stretch_distorts.message"),
      metrics.bodyTextSize, app_theme::textSecondary()));
  bgaDisplayModeText =
      makeText("", metrics.bodyTextSize + 6, app_theme::textPrimary(),
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

  auto *previewControls = new View();
  previewControls->setFlexDirection(FlexDirection::Column);
  previewControls->setGap(metrics.compact ? 12.0f : 16.0f);
  previewControls->setAlignItems(YGAlignFlexStart);
  auto *previewButton = makeAccentButton(
      metrics.actionButtonWidth, metrics.actionButtonHeight,
      makeText(i18n::message("settings.lane.preview.label"), metrics.bodyTextSize + 4, app_theme::textPrimary(),
               TextView::CENTER, TextView::MIDDLE),
      app_theme::lime());
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

  auto *covers = new LaneCoverControlsView([this](const lane_cover::State &value) {
    context.settings.presentation().setLaneCoverState(value);
    persistSettings();
  });
  covers->setWidthPercent(100);
  covers->refresh(context.settings.presentation().laneCoverState());
  noteStartPanel->addView(covers);

  hispeedAutoAdjustModeText =
      makeText(i18n::message("settings.lane.hi_speed_auto_adjust_off.label"), metrics.bodyTextSize + 6,
               app_theme::textPrimary(),
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
      metrics, i18n::message("settings.lane.covers.title"), "",
      noteStartPanel, metrics.offsetCardHeight, metrics.cardsWidth));

  auto *invisibleNoteControls = new View();
  invisibleNoteControls->setFlexDirection(FlexDirection::Column);
  invisibleNoteControls->setGap(metrics.compact ? 12.0f : 16.0f);
  invisibleNoteControls->setAlignItems(YGAlignFlexStart);
  showInvisibleNotesModeText =
      makeText("", metrics.bodyTextSize + 6, app_theme::textPrimary(),
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
      makeText("", metrics.bodyTextSize + 6, app_theme::textPrimary(),
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

  auto *startLaneIndicatorControls = new View();
  startLaneIndicatorControls->setFlexDirection(FlexDirection::Column);
  startLaneIndicatorControls->setGap(metrics.compact ? 12.0f : 16.0f);
  startLaneIndicatorControls->setAlignItems(YGAlignFlexStart);
  startLaneIndicatorsModeText =
      makeText("", metrics.bodyTextSize + 6, app_theme::textPrimary(),
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

  return cardsColumn;
}

View *SettingsScene::buildGeneralTab(const LayoutMetrics &metrics) {
  auto *cardsColumn = makeCardsColumn(metrics);

  auto *languageControls = new View();
  languageControls->setFlexDirection(FlexDirection::Column);
  languageControls->setGap(metrics.compact ? 12.0F : 16.0F);
  languageControls->setAlignItems(YGAlignFlexStart);
  auto *languageStatus = makeWrappedText(
      i18n::message("settings.language.change_notice"),
      metrics.bodyTextSize, app_theme::textSecondary());
  auto *languageDropdown = new DropdownView(
      {.onOptionSelectedResult =
           [this, languageStatus](const std::string &id) {
             auto &preference = context.applicationUiState.language;
             std::string previous;
             {
               std::lock_guard lock(context.applicationUiStateMutex);
               previous = preference;
               preference = id;
             }
             std::string error;
             const bool saved = context.saveApplicationUiState(&error);
             if (!saved) {
               std::lock_guard lock(context.applicationUiStateMutex);
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
                   {.id = "ja", .label = "日本語"},
                   {.id = "zh-Hans", .label = "简体中文"},
                   {.id = "zh-Hant", .label = "繁體中文"}},
       .maxVisibleItems = 6});
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
      makeText("", metrics.bodyTextSize + 6, app_theme::textPrimary(),
               TextView::CENTER, TextView::MIDDLE);
  uiThemeModeButton =
      makeAccentButton(metrics.actionButtonWidth, metrics.actionButtonHeight,
                       uiThemeModeText, app_theme::cyan());
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
      makeText(i18n::message("settings.difficulty_tables.add_table.label"), metrics.bodyTextSize + 4, app_theme::textPrimary(),
               TextView::CENTER, TextView::MIDDLE),
      app_theme::lime());
  addButton->setOnClickListener([this]() { addDifficultyTableFromUrl(); });
  urlRow->addView(addButton);
  addControls->addView(urlRow);

  difficultyTableStatusText = makeWrappedText(
      difficultyTableStatusMessage, metrics.bodyTextSize,
      Color(difficultyTableStatusColor.r, difficultyTableStatusColor.g,
            difficultyTableStatusColor.b, difficultyTableStatusColor.a));
  addControls->addView(difficultyTableStatusText);

  cardsColumn->addView(makeCard(metrics,
      i18n::message("settings.difficulty_tables.install.label"), tableCardDescription,
      addControls, metrics.modeCardHeight, metrics.cardsWidth));

  auto *tableList = new View();
  tableList->setFlexDirection(FlexDirection::Column);
  tableList->setGap(metrics.compact ? 10.0f : 12.0f);
  tableList->setAlignSelf(YGAlignStretch);

  if (difficultyTables.empty()) {
    tableList->addView(makeWrappedText(i18n::message("settings.difficulty_tables.no_difficulty_tables_installed.message"),
                                       metrics.bodyTextSize,
                                       app_theme::textSecondary()));
  } else {
    for (const auto &table : difficultyTables) {
      auto *row = new View();
      row->setFlexDirection(FlexDirection::Column);
      row->setGap(metrics.compact ? 8.0f : 10.0f);
      row->setPadding(Edge::All, static_cast<float>(metrics.compact ? 14 : 16));
      row->setThemedBackgroundColor(app_theme::panelSubtle);
      row->setCornerRadius(app_theme::controlRadius());
      row->setThemedBorderColor(app_theme::hairline);
      row->setBorderWidth(1);

      auto *titleRow = new View();
      titleRow->setFlexDirection(FlexDirection::Row);
      titleRow->setFlexWrap(YGWrapWrap);
      titleRow->setGap(metrics.compact ? 8.0f : 12.0f);
      titleRow->setAlignItems(YGAlignCenter);
      titleRow->addView(makeWrappedText(table.name, metrics.bodyTextSize + 6,
                                        app_theme::textPrimary()));
      titleRow->addView(
          makeText(table.symbol, metrics.bodyTextSize, app_theme::cyan()));
      titleRow->addView(makeText(formatTableCount(table.chartCount),
                                 metrics.bodyTextSize,
                                 app_theme::textSecondary()));
      row->addView(titleRow);

      row->addView(makeWrappedText(formatTableSource(table.sourceUrl),
                                   metrics.smallTextSize,
                                   app_theme::textMuted()));

      auto *actions = new View();
      actions->setFlexDirection(FlexDirection::Row);
      actions->setFlexWrap(YGWrapWrap);
      actions->setGap(metrics.compact ? 8.0f : 10.0f);

      const int smallActionWidth = metrics.compact ? 136 : 156;
      auto *updateButton = makeControlButton(
          smallActionWidth, metrics.actionButtonHeight,
          makeText(i18n::message("settings.difficulty_tables.update.label"), metrics.bodyTextSize + 2, app_theme::textPrimary(),
                   TextView::CENTER, TextView::MIDDLE));
      updateButton->setOnClickListener([this, tableId = table.id]() {
        updateDifficultyTableFromSource(tableId);
      });
      actions->addView(updateButton);

      const bool confirmingDelete = pendingDeleteDifficultyTableId == table.id;
      auto *deleteButton = makeAccentButton(
          smallActionWidth, metrics.actionButtonHeight,
          makeText(confirmingDelete ? i18n::message("settings.difficulty_tables.confirm.label") : i18n::message("settings.difficulty_tables.delete.label"),
                   metrics.bodyTextSize + 2, app_theme::textPrimary(),
                   TextView::CENTER, TextView::MIDDLE),
          app_theme::coral());
      deleteButton->setOnClickListener(
          [this, tableId = table.id]() { deleteDifficultyTable(tableId); });
      actions->addView(deleteButton);

      row->addView(actions);
      tableList->addView(row);
    }
  }

  cardsColumn->addView(makeCard(metrics,
      i18n::message("settings.difficulty_tables.installed_difficulty_tables.label"), "",
      tableList, metrics.modeCardHeight, metrics.cardsWidth));
  return cardsColumn;
}

View *SettingsScene::buildBmsLibraryTab(const LayoutMetrics &metrics) {
  auto *cardsColumn = makeCardsColumn(metrics);
  loadChartEntries();
  measureTemporaryArchiveCache();
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
               app_theme::textPrimary(), TextView::CENTER, TextView::MIDDLE),
      app_theme::lime());
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
                 app_theme::textPrimary(), TextView::CENTER, TextView::MIDDLE),
        app_theme::cyan());
    addFolderButton->setOnClickListener([this, importFolderByCopy,
                                         addFolderButtonLabel]() {
      if (context.requestAddChartFolderFromFiles) {
        context.requestAddChartFolderFromFiles();
        chartFolderStatusMessage = importFolderByCopy
                                       ? i18n::message("settings.bms_library.choose_folder_import.message")
                                       : i18n::message("settings.bms_library.choose_folder_add.message");
        chartFolderStatusColor = app_theme::sdl(app_theme::textSecondary());
      } else {
        chartFolderStatusMessage = i18n::message("settings.library.action.unavailable", {{"action", addFolderButtonLabel}});
        chartFolderStatusColor = app_theme::sdl(app_theme::coral());
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
    playNote->setThemedBackgroundColor(app_theme::panelSubtle);
    playNote->setCornerRadius(app_theme::controlRadius());
    playNote->setThemedBorderColor(app_theme::hairline);
    playNote->setBorderWidth(1);
    playNote->addView(makeWrappedText(
        i18n::message("settings.bms_library.folder_import.play_storage_notice"),
        metrics.bodyTextSize, app_theme::textSecondary()));

    auto *repoButton = makeAccentButton(
        metrics.compact ? 150 : 170, metrics.actionButtonHeight,
        makeText(i18n::message("settings.bms_library.open_git_hub.label"), metrics.bodyTextSize + 2,
                 app_theme::textPrimary(), TextView::CENTER, TextView::MIDDLE),
        app_theme::cyan());
    repoButton->setOnClickListener([this]() {
      std::string errorMessage;
      if (!OpenURLInAndroidBrowser(kRepositoryUrl, errorMessage)) {
        chartFolderStatusMessage = i18n::message("settings.bms_library.could_not_open_git_hub_link.message");
        chartFolderStatusColor = app_theme::sdl(app_theme::coral());
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
                                        app_theme::textSecondary()));
    folderList->addView(makeWrappedText(
        i18n::message("settings.bms_library.default_download_folder.help"),
        metrics.smallTextSize, app_theme::textMuted()));
  } else {
    for (const auto &entry : chartEntries) {
      const std::string entryPathText = formatChartEntryPath(entry);

      auto *row = new View();
      row->setFlexDirection(FlexDirection::Column);
      row->setGap(metrics.compact ? 8.0f : 10.0f);
      row->setPadding(Edge::All, static_cast<float>(metrics.compact ? 14 : 16));
      row->setThemedBackgroundColor(app_theme::panelSubtle);
      row->setCornerRadius(app_theme::controlRadius());
      row->setThemedBorderColor(app_theme::hairline);
      row->setBorderWidth(1);

      row->addView(makeWrappedText(formatChartEntryName(entry),
                                   metrics.bodyTextSize + 6,
                                   app_theme::textPrimary()));
      row->addView(makeWrappedText(formatChartEntrySource(entry),
                                   metrics.smallTextSize,
                                   app_theme::textMuted()));

      auto *actions = new View();
      actions->setFlexDirection(FlexDirection::Row);
      actions->setFlexWrap(YGWrapWrap);
      actions->setGap(metrics.compact ? 8.0f : 10.0f);

      const int folderActionWidth = metrics.compact ? 136 : 156;
      if (entry.primaryStorageFolder) {
        actions->addView(makeWrappedText(i18n::message("settings.bms_library.download_folder.label"),
                                         metrics.smallTextSize,
                                         app_theme::lime()));
      } else if (entry.primaryStorageEligible) {
        auto *downloadButton = makeAccentButton(
            metrics.compact ? 180 : 210, metrics.actionButtonHeight,
            makeText(i18n::message("settings.bms_library.use_downloads.label"), metrics.smallTextSize,
                     app_theme::textPrimary(), TextView::CENTER,
                     TextView::MIDDLE),
            app_theme::cyan());
        downloadButton->setOnClickListener([this, entryPathText]() {
          setFindBmsDownloadEntry(entryPathText);
        });
        actions->addView(downloadButton);
      } else if (ChartRepository::IsDefaultBmsFolderPath(
                     std::filesystem::path(entry.path))) {
        actions->addView(makeWrappedText(i18n::message("settings.bms_library.fallback_download_folder.label"),
                                         metrics.smallTextSize,
                                         app_theme::textMuted()));
      } else {
        actions->addView(makeWrappedText(i18n::message("settings.bms_library.not_writable_by_find_bms.label"),
                                         metrics.smallTextSize,
                                         app_theme::textMuted()));
      }

      if (entry.removable) {
        const bool confirmingDelete =
            pendingDeleteChartEntryPath == entryPathText;
        auto *deleteButton = makeAccentButton(
            folderActionWidth, metrics.actionButtonHeight,
            makeText(confirmingDelete ? i18n::message("settings.bms_library.confirm.label") : i18n::message("settings.bms_library.delete.label"),
                     metrics.bodyTextSize + 2, app_theme::textPrimary(),
                     TextView::CENTER, TextView::MIDDLE),
            app_theme::coral());
        deleteButton->setOnClickListener(
            [this, entryPathText]() { deleteChartEntry(entryPathText); });
        actions->addView(deleteButton);
      } else {
        actions->addView(makeWrappedText(i18n::message("settings.bms_library.built_in.label"), metrics.smallTextSize,
                                         app_theme::textMuted()));
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
                   metrics.smallTextSize, app_theme::textPrimary(),
                   TextView::CENTER, TextView::MIDDLE),
          backupExcluded ? app_theme::cyan() : app_theme::lime());
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
  auto *archivePreviewControls = new View();
  archivePreviewControls->setFlexDirection(FlexDirection::Column);
  archivePreviewControls->setGap(metrics.compact ? 12.0f : 16.0f);
  archivePreviewControls->setAlignItems(YGAlignFlexStart);
  archiveChartPreviewModeText =
      makeText("", metrics.bodyTextSize + 6, app_theme::textPrimary(),
               TextView::CENTER, TextView::MIDDLE);
  archiveChartPreviewModeButton =
      makeAccentButton(metrics.actionButtonWidth, metrics.actionButtonHeight,
                       archiveChartPreviewModeText, app_theme::lime());
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
      makeText("", metrics.bodyTextSize + 6, app_theme::textPrimary(),
               TextView::CENTER, TextView::MIDDLE);
  findBmsSkipUnarchivingModeButton =
      makeAccentButton(metrics.actionButtonWidth, metrics.actionButtonHeight,
                       findBmsSkipUnarchivingModeText, app_theme::lime());
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
               metrics.bodyTextSize + 4, app_theme::textPrimary(),
               TextView::CENTER, TextView::MIDDLE);
  archiveCacheCleanupButton =
      makeAccentButton(metrics.actionButtonWidth, metrics.actionButtonHeight,
                       archiveCacheCleanupButtonText, app_theme::coral());
  archiveCacheCleanupButton->setOnClickListener(
      [this]() { cleanupTemporaryArchiveCache(); });
  cacheCleanupControls->addView(archiveCacheCleanupButton);
  archiveCacheCleanupStatusText =
      makeWrappedText(archiveCacheCleanupStatusMessage, metrics.bodyTextSize,
                      app_theme::textSecondary());
  archiveCacheCleanupStatusText->setColor(archiveCacheCleanupStatusColor);
  cacheCleanupControls->addView(archiveCacheCleanupStatusText);
  cardsColumn->addView(makeCard(
      metrics, i18n::message("settings.misc.archive_cache.label"), i18n::message("settings.misc.remove_temporary_extracted_media.message"),
      cacheCleanupControls, metrics.modeCardHeight, metrics.cardsWidth));

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
  difficultyTableImportModalRoot->setThemedBackgroundColor(app_theme::scrim);

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
      ->setThemedBackgroundColor(app_theme::panelStrong)
      ->setCornerRadius(app_theme::panelRadius())
      ->setThemedBorderColor(app_theme::hairline)
      ->setBorderWidth(1);

  difficultyTableImportTitleText =
      makeHeading(i18n::message("settings.difficulty_table_import_modal.importing_difficulty_tables.label"), metrics.sectionTitleSize);
  importPanel->addView(difficultyTableImportTitleText);

  difficultyTableImportStatusText = makeWrappedText(
      i18n::message("settings.difficulty_table_import_modal.preparing_import.progress"), metrics.bodyTextSize, app_theme::textSecondary());
  importPanel->addView(difficultyTableImportStatusText);

  difficultyTableImportTableText =
      makeWrappedText(i18n::message("settings.difficulty_table_import_modal.current_table_resolving_table_url.label"),
                      metrics.bodyTextSize, app_theme::textPrimary());
  importPanel->addView(difficultyTableImportTableText);

  auto *progressRow = new View();
  progressRow->setFlexDirection(FlexDirection::Column);
  progressRow->setGap(metrics.compact ? 8.0f : 10.0f);
  difficultyTableImportProgressText =
      makeText(i18n::message("settings.difficulty_table_import_modal.import.initial_progress"), metrics.bodyTextSize, app_theme::textMuted());
  progressRow->addView(difficultyTableImportProgressText);

  auto *progressTrack = new View();
  progressTrack->setHeight(static_cast<float>(metrics.compact ? 16 : 18));
  progressTrack->setAlignSelf(YGAlignStretch);
  progressTrack->setFlexDirection(FlexDirection::Row);
  progressTrack->setThemedBackgroundColor(app_theme::control);
  progressTrack->setCornerRadius(app_theme::controlRadius());
  progressTrack->setThemedBorderColor(app_theme::hairline);
  progressTrack->setBorderWidth(1);
  difficultyTableImportProgressFill = new View();
  difficultyTableImportProgressFill->setWidthPercent(0.0f);
  difficultyTableImportProgressFill->setHeight(
      static_cast<float>(metrics.compact ? 16 : 18));
  difficultyTableImportProgressFill->setThemedBackgroundColor(
      app_theme::progressFill);
  progressTrack->addView(difficultyTableImportProgressFill);
  progressRow->addView(progressTrack);
  importPanel->addView(progressRow);

  auto *modalActions = new View();
  modalActions->setFlexDirection(FlexDirection::Row);
  modalActions->setJustifyContent(YGJustifyFlexEnd);
  difficultyTableImportCloseButton = makeControlButton(
      160, 60,
      makeText(i18n::message("settings.difficulty_table_import_modal.close.label"), metrics.bodyTextSize + 2, app_theme::textPrimary(),
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

  // The settings workspace shares the selector's compact band and ruled regions.
  // Preview keeps its own metrics and viewport above.
  metrics.flatSections = true;
  metrics.horizontalPadding = metrics.compact ? 18 : 28;
  metrics.contentWidth = std::max(0, rendering::window_width - metrics.safe.left -
      metrics.safe.right - metrics.horizontalPadding * 2);
  metrics.titleSize = metrics.compact ? 28 : 34;
  metrics.sectionTitleSize = metrics.compact ? 24 : 28;
  metrics.cardPadding = metrics.compact ? 14 : 18;
  metrics.cardGap = metrics.compact ? 12 : 16;
  metrics.secondaryGap = 0;
  metrics.bodyGap = metrics.compact ? 18 : 28;
  metrics.actionButtonHeight = rendering::compactMenuCanvasActive ? rendering::compactTouchTarget : (metrics.compact ? 48 : 54);
  metrics.backButtonHeight = metrics.actionButtonHeight;
  metrics.backButtonWidth = metrics.compact ? 110 : 132;
  rootLayout->setPadding(Edge::All, 0);
  rootLayout->setPadding(Edge::Top, 0)->setPadding(Edge::Bottom, 0)
      ->setPadding(Edge::Left, 0)->setPadding(Edge::Right, 0);
  rootLayout->setGap(0);
  rootLayout->setThemedBackgroundColor(app_theme::backdrop);

  auto *header = new View();
  header->setName("settingsTopBar");
  header->setFlexDirection(FlexDirection::Row);
  header->setAlignItems(YGAlignCenter);
  header->setJustifyContent(YGJustifySpaceBetween);
  header->setPadding(Edge::Top, metrics.safe.top + 10);
  header->setPadding(Edge::Bottom, 10);
  header->setPadding(Edge::Left, metrics.safe.left + metrics.horizontalPadding);
  header->setPadding(Edge::Right, metrics.safe.right + metrics.horizontalPadding);
  header->setFlexShrink(0);

  auto *headerText = new View();
  headerText->setFlexDirection(FlexDirection::Column);
  headerText->setGap(static_cast<float>(metrics.headerGap));
  headerText->addView(
      makeHeading(i18n::message("settings.navigation.settings.label"), metrics.titleSize));
  header->addView(headerText);

  auto *backLabel =
      makeText(i18n::message("settings.navigation.back.label"), metrics.bodyTextSize + 6, app_theme::textPrimary(),
               TextView::CENTER, TextView::MIDDLE);
  auto *backButton = makeButton(
      metrics.backButtonWidth, metrics.backButtonHeight, backLabel,
      app_theme::control(), app_theme::controlHover(), app_theme::controlPressed(),
      app_theme::hairline(), app_theme::cyan(), app_theme::cyan());
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
  const int tabColumnWidth = portrait ? metrics.contentWidth : std::min(
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
  content->setMinHeight(0);
  content->setPadding(Edge::Top, 16);
  content->setPadding(Edge::Left, metrics.safe.left + metrics.horizontalPadding);
  content->setPadding(Edge::Right, metrics.safe.right + metrics.horizontalPadding);
  content->setPadding(Edge::Bottom, metrics.safe.bottom);
  content->setAlignItems(YGAlignStretch);

  auto *tabControls = new View();
  tabControls->setFlexDirection(FlexDirection::Column);
  tabControls->setGap(0);
  tabControls->setWidth(static_cast<float>(portrait ? metrics.contentWidth : tabColumnWidth));
  tabControls->setFlexShrink(0.0f);
  auto selectTab = [this](SettingsTab tab) {
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
  };
  const int tabHeight = metrics.compact ? metrics.actionButtonHeight : 72;
  std::vector<DropdownView::Option> navigationOptions;
  auto makeTabButton = [&](SettingsTab tab, const i18n::Text &label,
                           TextView **labelOut) {
    navigationOptions.push_back({.id = std::to_string(static_cast<int>(tab)), .label = label});
    auto *labelText =
        makeText(label, metrics.bodyTextSize, app_theme::textPrimary(),
                 TextView::LEFT, TextView::MIDDLE, TextView::FontWeight::Bold);
    if (labelOut != nullptr) {
      *labelOut = labelText;
    }
    auto *button =
        makeButton(kFitContentWidth, tabHeight, labelText,
                   app_theme::control(), app_theme::controlHover(),
                   app_theme::controlPressed(), app_theme::hairline(),
                   app_theme::accentBorder(), app_theme::accentBorderStrong());
    labelText->setAlign(TextView::LEFT);
    labelText->setWidthPercent(100)->setMinWidth(0);
    labelText->setAutoFitText(true);
    button->setWidth(tabColumnWidth);
    button->setPadding(Edge::Left, 14);
    button->setPadding(Edge::Right, 14);
    button->setOnClickListener([selectTab, tab]() { selectTab(tab); });
    return button;
  };
  profileTabButton =
      makeTabButton(SettingsTab::Profile, i18n::message("settings.navigation.profile.label"), &profileTabText);
  timingTabButton =
      makeTabButton(SettingsTab::Timing, i18n::message("settings.navigation.timing.label"), &timingTabText);
  hudTabButton = makeTabButton(SettingsTab::Hud, "HUD", &hudTabText);
  backgroundTabButton =
      makeTabButton(SettingsTab::Background, i18n::message("settings.navigation.background.label"), &backgroundTabText);
  laneTabButton = makeTabButton(SettingsTab::Lane, i18n::message("settings.navigation.lane.label"), &laneTabText);
  inputTabButton = makeTabButton(SettingsTab::Input, i18n::message("settings.navigation.input.label"), &inputTabText);
  generalTabButton = makeTabButton(SettingsTab::General, i18n::message("settings.navigation.general.label"), &generalTabText);
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
  tabControls->addView(hudTabButton);
  tabControls->addView(backgroundTabButton);
  tabControls->addView(gameplaySkinsTabButton);
  tabControls->addView(audioTabButton);
  tabControls->addView(displayTabButton);
  tabControls->addView(bmsLibraryTabButton);
  tabControls->addView(difficultyTablesTabButton);
  tabControls->addView(irTabButton);
  tabControls->addView(generalTabButton);
  auto *tabRail = new ScrollView();
  tabRailScrollView = tabRail;
  tabRail->setWidth(static_cast<float>(portrait ? metrics.contentWidth : tabColumnWidth));
  if (portrait) tabRail->setDisplay(YGDisplayNone);
  tabRail->setFlexShrink(0.0f);
  tabRail->setContentView(tabControls);
  content->addView(tabRail);

  scrollView = new ScrollView();
  scrollView->setFlex(1.0f);
  scrollView->setMinHeight(0)->setMinWidth(0);
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

  if (portrait) {
    auto *navigation = new DropdownView(
        {.onOptionSelected = [selectTab](const std::string &id) {
          selectTab(static_cast<SettingsTab>(std::stoi(id)));
        }}, overlayPortal);
    navigation->setName("settingsSectionNavigation");
    navigation->setWidthPercent(100)->setHeight(metrics.actionButtonHeight);
    navigation->setFlexShrink(0);
    navigation->refresh({.selectedId = std::to_string(static_cast<int>(activeTab)),
                         .options = std::move(navigationOptions),
                         .enabled = !profilePipelineBusy, .maxVisibleItems = 8});
    content->addView(navigation);
  }

  View *cardsColumn = nullptr;
  switch (activeTab) {
  case SettingsTab::Profile:
    cardsColumn = buildProfileTab(metrics);
    break;
  case SettingsTab::Timing:
    cardsColumn = buildTimingTab(metrics);
    break;
  case SettingsTab::Hud:
    cardsColumn = buildHudTab(metrics);
    break;
  case SettingsTab::Background:
    cardsColumn = buildBackgroundTab(metrics);
    break;
  case SettingsTab::Lane:
    cardsColumn = buildLaneTab(metrics);
    break;
  case SettingsTab::Input:
    cardsColumn = buildInputTab(metrics);
    break;
  case SettingsTab::General:
    cardsColumn = buildGeneralTab(metrics);
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
    refineSettingsSectionSpacing(cardsColumn, metrics);
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

namespace settings_scene {

void styleGameplaySkinChoiceButton(Button *button, bool selected) {
  button->setSelected(selected);
  if (auto *label = dynamic_cast<TextView *>(button->getContentView())) {
    label->setThemedColor(selected ? app_theme::onAction : app_theme::textPrimary);
  }
  if (!selected) {
    button->setThemedBackgroundColors(app_theme::control, app_theme::controlHover,
                                      app_theme::controlPressed);
    button->setThemedBorderColors(app_theme::hairline, app_theme::accentBorder,
                                  app_theme::accentBorderStrong);
    return;
  }

  button->setThemedBackgroundColors(app_theme::action, app_theme::actionHover,
                                    app_theme::actionPressed);
  button->setThemedBorderColors(app_theme::action, app_theme::actionHover,
                                app_theme::actionPressed);
}

View *makeGameplaySkinChoiceRow(
    const LayoutMetrics &metrics, const i18n::Text &label, bool enabled,
    std::vector<GameplaySkinChoiceButton> choices) {
  auto *row = new View();
  row->setFlexDirection(FlexDirection::Column);
  row->setWidthPercent(100);
  row->setJustifyContent(YGJustifyFlexStart);
  row->setAlignItems(YGAlignFlexStart);
  row->setGap(metrics.compact ? 8.0f : 10.0f);

  auto *labelView = makeText(label, metrics.smallTextSize,
                             app_theme::textSecondary(), TextView::LEFT,
                             TextView::MIDDLE);
  labelView->setMinWidth(0.0f);
  labelView->setFlexShrink(1.0f);
  row->addView(labelView);

  auto *buttons = new View();
  buttons->setFlexDirection(FlexDirection::Row);
  buttons->setFlexWrap(YGWrapWrap);
  buttons->setWidthPercent(100);
  buttons->setJustifyContent(YGJustifyFlexStart);
  buttons->setGap(metrics.compact ? 6.0f : 8.0f);
  auto choiceButtons = std::make_shared<std::vector<Button *>>();
  for (auto &choice : choices) {
    auto *choiceLabel = makeText(choice.label, metrics.smallTextSize,
                                 app_theme::textPrimary(), TextView::CENTER,
                                 TextView::MIDDLE);
    constexpr int horizontalContentPadding = 28;
    const int minimumWidth = metrics.compact ? 84 : 96;
    const int width = std::max(
        minimumWidth,
        std::max(0, choiceLabel->textureWidth()) + horizontalContentPadding);
    auto *button = choice.selected
                       ? makeAccentButton(width, metrics.actionButtonHeight,
                                          choiceLabel, app_theme::cyan())
                       : makeControlButton(width, metrics.actionButtonHeight,
                                           choiceLabel);
    button->setSelected(choice.selected);
    button->setEnabled(enabled);
    if (enabled) {
      button->setOnClickListener(
          [button, choiceButtons, action = std::move(choice.action),
           tryAction = std::move(choice.tryAction)]() mutable {
            const bool accepted = tryAction ? tryAction() : (action(), true);
            if (!accepted) {
              return;
            }
            for (auto *candidate : *choiceButtons) {
              styleGameplaySkinChoiceButton(candidate, candidate == button);
            }
          });
    }
    choiceButtons->push_back(button);
    buttons->addView(button);
  }
  row->addView(buttons);
  return row;
}

int sanitizeOffsetComponent(std::string_view text, int fallback) {
  int value = 0;
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), value);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size()
             ? value
             : fallback;
}

} // namespace settings_scene

void SettingsScene::appendSelectedSkinHudSettings(
    View *body, const LayoutMetrics &metrics, bool includeBuiltInOnlySettings) {
  const auto appendHeading = [body, &metrics](const i18n::Text &label) {
    body->addView(
        makeWrappedText(label, metrics.bodyTextSize, app_theme::cyan()));
  };
  body->setAlignItems(YGAlignFlexStart);
  const auto appendNumeric =
      [this, body, &metrics](const i18n::Text &label, std::function<std::string()> read,
                             const std::string &defaultValue,
                             std::function<void(const std::string &)> apply) {
        body->addView(makeWrappedText(label, metrics.smallTextSize, app_theme::textSecondary()));
        auto *input = makeTextInput(metrics, metrics.compact ? 116 : 136);
        input->setEditingText(read());
        input->onEditingFinished([this, input, read, apply](const std::string &text) {
          apply(text);
          input->setEditingText(read());
          lastLayoutWidth = -1;
        });
        auto *reset = makeResetButton(metrics);
        reset->setOnClickListener([this, input, read, apply, defaultValue]() {
          apply(defaultValue);
          input->setEditingText(read());
          lastLayoutWidth = -1;
        });
        body->addView(makeSteppedValueRow(metrics, input, "5",
            [this, input, read, apply](int direction) {
              apply(std::to_string(sanitizeOffsetComponent(read(), 0) + direction * 5));
              input->setEditingText(read());
              lastLayoutWidth = -1;
            }, reset));
      };
  const auto appendToggle = [this, body,
                             &metrics](const i18n::Text &label, bool value,
                                       std::function<void(bool)> set) {
    body->addView(makeGameplaySkinChoiceRow(
        metrics, label, true,
        {{.label = i18n::message("settings.skins.judgement_hud.off.label"),
          .selected = !value,
          .action =
              [this, set]() mutable {
                set(false);
                lastLayoutWidth = -1;
              }},
         {.label = i18n::message("settings.skins.judgement_hud.on.label"), .selected = value, .action = [this, set]() mutable {
            set(true);
            lastLayoutWidth = -1;
          }}}));
  };
  const auto appendChoices =
      [this, body, &metrics](const i18n::Text &label,
                             std::vector<GameplaySkinChoiceButton> choices) {
        body->addView(makeGameplaySkinChoiceRow(metrics, label, true,
                                                std::move(choices)));
      };

  if (includeBuiltInOnlySettings) {
  appendHeading(i18n::message("settings.skins.judgement_feedback.label"));
  body->addView(buildJudgementFeedbackStyleControls(metrics, true));
  body->addView(buildJudgementFeedbackPositionControls(metrics, true));
  const auto timingChoices =
      [this](AppSettings::JudgementTimingDisplayCriteria value, auto assign) {
        std::vector<GameplaySkinChoiceButton> choices;
        for (const auto criteria :
             {AppSettings::JudgementTimingDisplayCriteria::PGreatOrBelow,
              AppSettings::JudgementTimingDisplayCriteria::GreatOrBelow,
              AppSettings::JudgementTimingDisplayCriteria::GoodOrBelow,
              AppSettings::JudgementTimingDisplayCriteria::BadOrBelow,
              AppSettings::JudgementTimingDisplayCriteria::Off}) {
          choices.push_back(
              {.label = formatJudgementTimingDisplayCriteriaLabel(criteria),
               .selected = value == criteria,
               .action = [this, criteria, assign]() mutable {
                 assign(criteria);
                 persistSettings();
                 lastLayoutWidth = -1;
               }});
        }
        return choices;
      };
  appendChoices(
      "FAST/SLOW",
      timingChoices(
          context.settings.presentation().judgementTimingFastSlowCriteria,
          [this](AppSettings::JudgementTimingDisplayCriteria criteria) {
            context.settings.presentation().judgementTimingFastSlowCriteria = criteria;
          }));
  appendChoices(
      "+/- ms",
      timingChoices(
          context.settings.presentation().judgementTimingMillisecondsCriteria,
          [this](AppSettings::JudgementTimingDisplayCriteria criteria) {
            context.settings.presentation().judgementTimingMillisecondsCriteria = criteria;
          }));

  }

  appendHeading(i18n::message("settings.skins.application_judgement_hud.label"));
  appendToggle(i18n::message("settings.skins.judgement_indicator.label"),
               context.settings.presentation().judgementIndicatorEnabled,
               [this](bool enabled) {
                 context.settings.presentation().judgementIndicatorEnabled = enabled;
                 persistSettings();
               });
  appendNumeric(
      i18n::message("settings.skins.indicator_y.percent_label"),
      [this] { return std::to_string(
          judgementIndicatorYToPercent(context.settings.presentation().judgementIndicatorY)); },
      std::to_string(judgementIndicatorYToPercent(AppSettings::kDefaultJudgementIndicatorY)),
      [this](const std::string &text) {
        context.settings.presentation().judgementIndicatorY = judgementIndicatorPercentToY(
            std::clamp(sanitizeOffsetComponent(
                           text, judgementIndicatorYToPercent(
                                     context.settings.presentation().judgementIndicatorY)),
                       0, 100));
        persistSettings();
      });
  appendNumeric(i18n::message("settings.skins.indicator_width.percent_label"),
                [this] { return std::to_string(judgementIndicatorWidthScaleToPercent(
                    context.settings.presentation().judgementIndicatorWidthScale)); },
                std::to_string(judgementIndicatorWidthScaleToPercent(
                    AppSettings::kDefaultJudgementIndicatorWidthScale)),
                [this](const std::string &text) {
                  const int current = judgementIndicatorWidthScaleToPercent(
                      context.settings.presentation().judgementIndicatorWidthScale);
                  context.settings.presentation().judgementIndicatorWidthScale =
                      judgementIndicatorWidthPercentToScale(std::clamp(
                          sanitizeOffsetComponent(text, current), 50, 200));
                  persistSettings();
                });
  appendNumeric(
      i18n::message("settings.skins.indicator_range_ms.label"),
      [this] { return std::to_string(context.settings.presentation().judgementIndicatorRangeMilliseconds); },
      std::to_string(AppSettings::kDefaultJudgementIndicatorRangeMilliseconds),
      [this](const std::string &text) {
        context.settings.presentation().judgementIndicatorRangeMilliseconds =
            clampJudgementIndicatorRangeMilliseconds(sanitizeOffsetComponent(
                text, context.settings.presentation().judgementIndicatorRangeMilliseconds));
        persistSettings();
      });

  if (includeBuiltInOnlySettings) {
    appendChoices(
        i18n::message("settings.skins.indicator_layout.label"),
        {{.label = i18n::message("settings.skins.hud.position.world.label"),
          .selected = context.settings.presentation().judgementIndicatorRenderMode ==
                      AppSettings::JudgementIndicatorRenderMode::World3D,
          .action =
              [this]() {
                context.settings.presentation().judgementIndicatorRenderMode =
                    AppSettings::JudgementIndicatorRenderMode::World3D;
                persistSettings();
                lastLayoutWidth = -1;
              }},
         {.label = i18n::message("settings.skins.hud.position.overlay.label"),
          .selected = context.settings.presentation().judgementIndicatorRenderMode ==
                      AppSettings::JudgementIndicatorRenderMode::Hud2D,
          .action = [this]() {
            context.settings.presentation().judgementIndicatorRenderMode =
                AppSettings::JudgementIndicatorRenderMode::Hud2D;
            persistSettings();
            lastLayoutWidth = -1;
          }}});
  }

  appendToggle(i18n::message("settings.skins.judgement_counter.label"), context.settings.presentation().judgementCounterEnabled,
               [this](bool enabled) {
                 context.settings.presentation().judgementCounterEnabled = enabled;
                 persistSettings();
               });
  appendChoices(i18n::message("settings.skins.counter_position.label"),
                {{.label = i18n::message("settings.skins.top.label"),
                  .selected = context.settings.presentation().judgementCounterPosition ==
                              AppSettings::JudgementCounterPosition::Top,
                  .action =
                      [this]() {
                        context.settings.presentation().judgementCounterPosition =
                            AppSettings::JudgementCounterPosition::Top;
                        persistSettings();
                        lastLayoutWidth = -1;
                      }},
                 {.label = i18n::message("settings.skins.left.label"),
                  .selected = context.settings.presentation().judgementCounterPosition ==
                              AppSettings::JudgementCounterPosition::Left,
                  .action =
                      [this]() {
                        context.settings.presentation().judgementCounterPosition =
                            AppSettings::JudgementCounterPosition::Left;
                        persistSettings();
                        lastLayoutWidth = -1;
                      }},
                 {.label = i18n::message("settings.skins.right.label"),
                  .selected = context.settings.presentation().judgementCounterPosition ==
                              AppSettings::JudgementCounterPosition::Right,
                  .action = [this]() {
                    context.settings.presentation().judgementCounterPosition =
                        AppSettings::JudgementCounterPosition::Right;
                    persistSettings();
                    lastLayoutWidth = -1;
                  }}});

  if (!includeBuiltInOnlySettings) {
    return;
  }

  appendHeading(i18n::message("settings.skins.gauge.label"));
  appendChoices(i18n::message("settings.skins.gauge_position.label"),
                {{.label = i18n::message("settings.skins.world.label"),
                  .selected = context.settings.presentation().gaugeBarPosition ==
                              AppSettings::GaugeBarPosition::World,
                  .action =
                      [this]() {
                        context.settings.presentation().gaugeBarPosition =
                            AppSettings::GaugeBarPosition::World;
                        persistSettings();
                        lastLayoutWidth = -1;
                      }},
                 {.label = i18n::message("settings.skins.left_hud.label"),
                  .selected = context.settings.presentation().gaugeBarPosition ==
                              AppSettings::GaugeBarPosition::Left,
                  .action =
                      [this]() {
                        context.settings.presentation().gaugeBarPosition =
                            AppSettings::GaugeBarPosition::Left;
                        persistSettings();
                        lastLayoutWidth = -1;
                      }},
                 {.label = i18n::message("settings.skins.right_hud.label"),
                  .selected = context.settings.presentation().gaugeBarPosition ==
                              AppSettings::GaugeBarPosition::Right,
                  .action = [this]() {
                    context.settings.presentation().gaugeBarPosition =
                        AppSettings::GaugeBarPosition::Right;
                    persistSettings();
                    lastLayoutWidth = -1;
                  }}});
}
