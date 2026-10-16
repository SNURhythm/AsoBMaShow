#include "../rendering/ResponsiveUi.h"
#include "../i18n/Localization.h"
#include "IrUploadsScene.h"

#include "../ir/IrProfileSettings.h"
#include "../ir/IrSubmissionService.h"
#include "../rendering/common.h"
#include "../targets.h"
#include "../view/Button.h"
#include "../view/IrUploadCandidateListView.h"
#include "../view/TextView.h"
#include "../view/AppTheme.h"
#include "../view/MainMenuTheme.h"
#include "SceneManager.h"
#include "SettingsScene.h"
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
#include "../iOSNatives.hpp"
#endif

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <iterator>
#include <span>
#include <stop_token>
#include <utility>
#include <vector>

namespace {

constexpr const char *kFontPath = app_theme::font;
constexpr float kScreenPadding = 18.0F;
constexpr float kHeaderHeight = 82.0F;

struct SafeAreaInsets {
  int top = 0;
  int left = 0;
  int bottom = 0;
  int right = 0;
};

SafeAreaInsets getSafeAreaInsetsUi() {
  SafeAreaInsets insets;
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  const IOSNormalizedSafeAreaInsets normalized =
      GetIOSSafeAreaInsetsNormalized();
  insets.top = static_cast<int>(normalized.top * rendering::window_height);
  insets.left = static_cast<int>(normalized.left * rendering::window_width);
  insets.bottom =
      static_cast<int>(normalized.bottom * rendering::window_height);
  insets.right = static_cast<int>(normalized.right * rendering::window_width);
#endif
  return insets;
}

Button *makeButton(const i18n::Text &label, int fontSize,
                   TextView **textOut = nullptr) {
  auto *button = new Button();
  button->setHeight(52);
  button->setCornerRadius(app_theme::controlRadius());
  button->setThemedBackgroundColors(app_theme::control, app_theme::controlHover,
                                    app_theme::controlPressed);
  button->setThemedBorderColors(app_theme::hairlineStrong,
                                app_theme::hairlineStrong,
                                app_theme::hairlineStrong);
  button->setStyledBorderWidth(1);

  auto *text = new TextView(fontSize >= 26 ? app_theme::boldFont : kFontPath, fontSize);
  text->setLocalizedText(label);
  text->setAlign(TextView::CENTER);
  text->setVAlign(TextView::MIDDLE);
  text->setOverflow(TextView::TextOverflow::Hidden);
  text->setThemedColor([] { return app_theme::textOn(app_theme::control()); });
  button->setContentView(text);
  if (textOut != nullptr) {
    *textOut = text;
  }
  return button;
}

TextView *makeText(const i18n::Text &text, int fontSize,
                   View::ThemeColorProvider color) {
  auto *view = new TextView(fontSize >= 26 ? app_theme::boldFont : kFontPath, fontSize);
  view->setLocalizedText(text);
  view->setThemedColor(std::move(color));
  view->setOverflow(TextView::TextOverflow::Hidden);
  return view;
}

} // namespace

void IrUploadsScene::configureTab(float topInset,
                                  std::function<void()> onClose,
                                  Scene *settingsReturnOwner) {
  embeddedTab = true;
  tabClose = std::move(onClose);
  tabSettingsReturnOwner = settingsReturnOwner;
  setTabTopInset(topInset);
}

void IrUploadsScene::setTabTopInset(float topInset) {
  const float inset = std::max(0.0F, topInset);
  if (tabTopInset == inset) {
    return;
  }
  tabTopInset = inset;
  updateLayoutInsets();
}

void IrUploadsScene::onPause() {
  // Keep preparation running while deferring hidden list and layout work.
  presentationActive = false;
  onInputQueueOverflow();
}

std::optional<std::string> IrUploadsScene::profileSwitchBlocker() {
  // Settings can ask while the retained menu receives no updates. Consume a
  // completed worker here so it cannot indefinitely block a profile switch.
  applyPreparationUpdates();
  if (!preparationTask.hasWorker()) return std::nullopt;
  auto status = controller.statusMessage().resolve();
  return status.empty() ? i18n::tr("menu.loading.progress") : status;
}

void IrUploadsScene::onResume() {
  presentationActive = true;
  refreshProfileIfNeeded();
  refreshLanguageIfNeeded();
  observeRemoteRevisions();
  if ((!candidatesLoaded || reloadRequested) && !controller.selectionLocked()) {
    reloadCandidates();
  } else {
    refreshUi();
  }
  updateLayoutInsets();
}

void IrUploadsScene::onProfileWillChange() {
  stopPreparation();
  preparationTask.reset();
  controller = {};
  displayedProfileId.clear();
  candidatesLoaded = false;
  loadError = {};
  loadDiagnostic = {};
  reloadRequested = false;
  if (candidateList != nullptr) {
    candidateList->setCandidates({}, {});
    candidateList->scrollOffset = 0;
  }
  refreshUi();
}

void IrUploadsScene::refreshProfileIfNeeded() {
  const auto &profileId = context.profileManager.activeProfile().id;
  if (displayedProfileId == profileId) {
    return;
  }
  onProfileWillChange();
  displayedProfileId = profileId;
  reloadRequested = true;
}

void IrUploadsScene::updateLayoutInsets() {
  if (rootLayout == nullptr) {
    return;
  }
  auto safe = getSafeAreaInsetsUi();
  lastSafeTop = safe.top;
  lastSafeLeft = safe.left;
  lastSafeBottom = safe.bottom;
  lastSafeRight = safe.right;
  if (embeddedTab) safe.top = 0;
  rootLayout->setSize(rendering::window_width, rendering::window_height);
  rootLayout->setPadding(Edge::Top, embeddedTab ? tabTopInset : 0);
  headerLayout->setDisplay(embeddedTab ? YGDisplayNone : YGDisplayFlex);
  headerLayout->setHeight(safe.top + kHeaderHeight)
      ->setPadding(Edge::Top, safe.top + 10)
      ->setPadding(Edge::Left, safe.left + kScreenPadding)
      ->setPadding(Edge::Right, safe.right + kScreenPadding);
  contentLayout->setPadding(Edge::Bottom, safe.bottom)
      ->setPadding(Edge::Left, safe.left)
      ->setPadding(Edge::Right, safe.right);
  const float width = rendering::window_width - safe.left - safe.right;
  const bool phoneStack = width < 900;
  contentLayout->setFlexDirection(phoneStack ? FlexDirection::Column : FlexDirection::Row);
  if (auto *rail = rootLayout->findViewByName("irProviderRail")) {
    rail->setWidth(phoneStack ? width : (width < 1100 ? width * 0.28F : 350));
    rail->setHeight(YGUndefined);
    rail->setFlexWrap(phoneStack ? YGWrapWrap : YGWrapNoWrap);
    rail->setGap(phoneStack ? 12 : 16);
    rail->setFlexDirection(phoneStack ? FlexDirection::Row : FlexDirection::Column);
    rail->setPadding(Edge::All, phoneStack ? 12 : 24);
  }
  if (auto *provider = rootLayout->findViewByName("irProviderIdentity")) {
    provider->setFlex(0);
    provider->setWidth(phoneStack ? std::max(0.0f, width - 196) : YGUndefined);
  }
  providerStatusText->setMinHeight(phoneStack ? 36 : 100);
  openIrSettingsButton->setWidth(phoneStack ? 160 : YGUndefined);
  if (!phoneStack) openIrSettingsButton->setWidthPercent(100);
  if (embeddedTab) {
    candidateCountText->setWidth(phoneStack ? std::max(0.0f, width - 196) : YGUndefined);
    refreshButton->setWidth(phoneStack ? 160 : YGUndefined);
    if (!phoneStack) {
      candidateCountText->setWidthPercent(100);
      refreshButton->setWidthPercent(100);
    }
  }
  const int actionHeight = rendering::compactMenuCanvasActive ? rendering::compactTouchTarget : 52;
  for (auto *button : {openIrSettingsButton, selectAllButton, clearButton, refreshButton})
    button->setHeight(actionHeight);
  if (auto *toolbar = rootLayout->findViewByName("irSelectionToolbar"))
    toolbar->setHeight(std::max(58, actionHeight));

  rootLayout->applyYogaLayout();
}

void IrUploadsScene::init() {
  presentationActive = true;
  displayedProfileId = context.profileManager.activeProfile().id;
  context.jukebox.stop();
  preparationTask.reset();
  lastLayoutWidth = rendering::window_width;
  lastLayoutHeight = rendering::window_height;
  observedAccountEvidenceRevision =
      context.irAccountEvidenceRevision.load(std::memory_order_acquire);
  observedAttemptStatusRevision =
      context.irSubmissionService != nullptr
          ? context.irSubmissionService
                ->status(ir::kTachiProviderId, std::string_view{})
                .revision
          : 0;
  buildView();
  reloadCandidates();
}

EventHandleResult IrUploadsScene::handleEvents(SDL_Event &event) {
  if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
    goBack();
    return {};
  }
  return Scene::handleEvents(event);
}

void IrUploadsScene::update(float) {
  refreshProfileIfNeeded();
  applyPreparationUpdates();
  // Completion still reaches the controller in the background. Resume observes
  // the latest revisions once, avoiding a history scan for each upload status.
  if (!presentationActive) {
    return;
  }
  observeRemoteRevisions();
  if (reloadRequested && !controller.selectionLocked()) {
    reloadRequested = false;
    reloadCandidates();
  }

  const auto safe = getSafeAreaInsetsUi();
  if (rootLayout != nullptr && (lastLayoutWidth != rendering::window_width ||
                                lastLayoutHeight != rendering::window_height ||
                                lastSafeTop != safe.top ||
                                lastSafeLeft != safe.left ||
                                lastSafeBottom != safe.bottom ||
                                lastSafeRight != safe.right)) {
    lastLayoutWidth = rendering::window_width;
    lastLayoutHeight = rendering::window_height;
    updateLayoutInsets();
  }
}

void IrUploadsScene::renderScene() {
  if (rootLayout != nullptr) {
    rootLayout->setSize(rendering::window_width, rendering::window_height);
  }
}

void IrUploadsScene::cleanupScene() {
  stopPreparation();
  rootLayout = nullptr;
  headerLayout = nullptr;
  contentLayout = nullptr;
  candidateCountText = nullptr;
  providerStatusText = nullptr;
  selectionCountText = nullptr;
  stateText = nullptr;
  progressText = nullptr;
  uploadButtonText = nullptr;
  refreshButton = nullptr;
  openIrSettingsButton = nullptr;
  selectAllButton = nullptr;
  clearButton = nullptr;
  uploadButton = nullptr;
  candidateList = nullptr;
  preparationTask.reset();
  controller = {};
  loadError = {};
  loadDiagnostic = {};
  providerCanSubmit = false;
  reloadRequested = false;
}

void IrUploadsScene::buildView() {
  auto safe = getSafeAreaInsetsUi();
  if (embeddedTab) safe.top = 0;
  rootLayout =
      new View(0, 0, rendering::window_width, rendering::window_height);
  rootLayout->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setPadding(Edge::Top, embeddedTab ? tabTopInset : 0)
      ->setThemedBackgroundColor(app_theme::mainMenuBackdrop);
  addView(rootLayout);

  auto *header = new View();
  headerLayout = header;
  header->setFlexShrink(0);
  header->setDisplay(embeddedTab ? YGDisplayNone : YGDisplayFlex);
  header->setHeight(safe.top + kHeaderHeight)
      ->setFlexDirection(FlexDirection::Row)
      ->setAlignItems(YGAlignCenter)
      ->setGap(14)
      ->setPadding(Edge::Top, safe.top + 10)
      ->setPadding(Edge::Left, safe.left + kScreenPadding)
      ->setPadding(Edge::Right, safe.right + kScreenPadding)
      ->setPadding(Edge::Bottom, 10)
      ->setThemedBackgroundColor(app_theme::panel)
      ->setThemedShadow(app_theme::shadow, app_theme::kHeaderShadow)
      ->setThemedBorderColor(app_theme::hairline)
      ->setBorderWidth(1);

  auto *backButton = makeButton(i18n::message("ir.upload.back.label"), 20);
  backButton->setWidth(118);
  backButton->setOnClickListener([this]() { goBack(); });
  backButton->setVisible(!embeddedTab);
  backButton->setDisplay(embeddedTab ? YGDisplayNone : YGDisplayFlex);
  header->addView(backButton);

  auto *title = makeText(i18n::message("ir.upload.ir_uploads.label"), 34, app_theme::textPrimary);
  title->setHeight(42);
  title->setFlex(1);
  header->addView(title);

  candidateCountText = makeText(i18n::message("ir.upload.score_count.empty"), 18, app_theme::textSecondary);
  candidateCountText->setWidth(150);
  candidateCountText->setHeight(32);
  candidateCountText->setAlign(TextView::RIGHT);
  candidateCountText->setVAlign(TextView::MIDDLE);
  if (!embeddedTab) header->addView(candidateCountText);

  refreshButton = makeButton(i18n::message("ir.upload.refresh.label"), 20);
  refreshButton->setWidth(132);
  refreshButton->setOnClickListener([this]() {
    if (!controller.selectionLocked()) {
      reloadCandidates();
    }
  });
  if (!embeddedTab) header->addView(refreshButton);
  rootLayout->addView(header);

  auto *content = new View();
  contentLayout = content;
  content->setFlex(1)
      ->setMinHeight(0)
      ->setFlexDirection(FlexDirection::Row)
      ->setAlignItems(YGAlignStretch)
      ->setPadding(Edge::Bottom, safe.bottom)
      ->setPadding(Edge::Left, safe.left)
      ->setPadding(Edge::Right, safe.right);

  auto *providerCard = new View();
  providerCard->setName("irProviderRail");
  providerCard->setWidth(350)
      ->setFlexShrink(0)
      ->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setGap(16)
      ->setPadding(Edge::All, 24);
  auto *railRule = new View();
  railRule->setPositionType(YGPositionTypeAbsolute)
      ->setPosition(Edge::Right, 0)->setPosition(Edge::Top, 0)
      ->setPosition(Edge::Bottom, 0)->setWidth(1)
      ->setThemedBackgroundColor(main_menu_theme::ruleStrong);
  providerCard->addView(railRule);
  auto *providerColumn = new View();
  providerColumn->setName("irProviderIdentity");
  providerColumn->setMinWidth(0)
      ->setFlexDirection(FlexDirection::Column)
      ->setGap(4);
  auto *providerTitle = makeText("Bokutachi", 32, app_theme::textPrimary);
  providerTitle->setHeight(46);
  providerColumn->addView(providerTitle);
  providerStatusText =
      makeText(i18n::message("ir.upload.checking_configuration.progress"), 17, app_theme::textSecondary);
  providerStatusText->setMinHeight(100);
  providerStatusText->setWrap(true);
  providerColumn->addView(providerStatusText);
  providerCard->addView(providerColumn);
  openIrSettingsButton = makeButton(i18n::message("ir.upload.open_ir_settings.label"), 18);
  openIrSettingsButton->setWidthPercent(100);
  openIrSettingsButton->setOnClickListener([this]() { openIrSettings(); });
  providerCard->addView(openIrSettingsButton);
  content->addView(providerCard);

  auto *workspace = new View();
  workspace->setFlex(1)->setMinWidth(0)->setMinHeight(0)
      ->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)->setGap(12)
      ->setPadding(Edge::All, 24);
  content->addView(workspace);

  auto *selectionToolbar = new View();
  selectionToolbar->setName("irSelectionToolbar");
  selectionToolbar->setHeight(58)
      ->setFlexShrink(0)
      ->setFlexDirection(FlexDirection::Row)
      ->setAlignItems(YGAlignCenter)
      ->setGap(10);
  selectAllButton = makeButton(i18n::message("ir.upload.select_all.label"), 18);
  selectAllButton->setWidth(146);
  selectAllButton->setOnClickListener([this]() {
    controller.selectAll();
    refreshUi();
  });
  selectionToolbar->addView(selectAllButton);
  clearButton = makeButton(i18n::message("ir.upload.clear.label"), 18);
  clearButton->setWidth(112);
  clearButton->setOnClickListener([this]() {
    controller.clearSelection();
    refreshUi();
  });
  selectionToolbar->addView(clearButton);
  selectionCountText = makeText(i18n::message("ir.upload.selection_count.empty"), 18, app_theme::textSecondary);
  selectionCountText->setHeight(32);
  selectionCountText->setVAlign(TextView::MIDDLE);
  selectionCountText->setFlex(1)->setMinWidth(0);
  selectionToolbar->addView(selectionCountText);
  if (embeddedTab) {
    candidateCountText->setAlign(TextView::LEFT);
    candidateCountText->setWidthPercent(100);
    providerCard->addView(candidateCountText);
    refreshButton->setWidthPercent(100);
    providerCard->addView(refreshButton);
  }
  workspace->addView(selectionToolbar);

  auto *listPanel = new View();
  listPanel->setFlex(1)
      ->setMinHeight(0)
      ->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setThemedBackgroundColor(main_menu_theme::surface);
  candidateList = new IrUploadCandidateListView();
  candidateList->setFlex(1);
  candidateList->clearBackgroundColor();
  candidateList->setBorderWidth(0);
  candidateList->onSelectionToggle = [this](std::string attemptId) {
    controller.toggle(attemptId);
    refreshUi();
  };
  candidateList->onSelected = [this](const ir::IrUploadCandidate &candidate,
                                     int) {
    controller.toggle(candidate.result.attemptId);
    refreshUi();
  };
  listPanel->addView(candidateList);
  stateText =
      makeText(i18n::message("ir.upload.no_scores_waiting_ir_upload.message"), 21, app_theme::textSecondary);
  stateText->setHeight(58);
  stateText->setWrap(true);
  stateText->setAlign(TextView::CENTER);
  stateText->setVAlign(TextView::MIDDLE);
  listPanel->addView(stateText);
  workspace->addView(listPanel);

  auto *footer = new View();
  footer->setHeight(88)
      ->setFlexShrink(0)
      ->setFlexDirection(FlexDirection::Row)
      ->setAlignItems(YGAlignCenter)
      ->setGap(14)
      ->setPadding(Edge::Left, 16)
      ->setThemedBackgroundColor(main_menu_theme::surface);
  progressText = makeText("", 18, app_theme::textSecondary);
  progressText->setFlex(1);
  progressText->setHeight(48);
  progressText->setWrap(true);
  progressText->setVAlign(TextView::MIDDLE);
  footer->addView(progressText);
  uploadButton = makeButton(i18n::message("ir.upload.upload_0_scores.label"), 20, &uploadButtonText);
  uploadButton->setWidth(280)->setHeight(88);
  uploadButton->setThemedBackgroundColors(main_menu_theme::action,
                                          main_menu_theme::actionHover,
                                          main_menu_theme::actionPressed);
  uploadButton->setStyledBorderWidth(0);
  uploadButtonText->setThemedColor(main_menu_theme::onAction);
  uploadButton->setOnClickListener([this]() { startUpload(); });
  footer->addView(uploadButton);
  workspace->addView(footer);

  rootLayout->addView(content);
  updateLayoutInsets();
}

std::string IrUploadsScene::serverOrigin() const {
  const auto provider =
      context.settings.irProviders.find(std::string(ir::kTachiProviderId));
  if (provider == context.settings.irProviders.end()) {
    return std::string(ir::kDefaultTachiServerOrigin);
  }
  const auto normalized =
      ir::normalizeServerOrigin(provider->second.serverOrigin);
  return normalized.value_or(std::string{});
}

void IrUploadsScene::reloadCandidates() {
  candidatesLoaded = false;
  // Consume changes before the read; changes arriving during it trigger a
  // subsequent refresh. Failed reads are retried on resume or explicit Refresh.
  observeRemoteRevisions();
  reloadRequested = false;
  const float scrollOffset =
      candidateList != nullptr ? candidateList->scrollOffset : 0.0F;
  loadError = {};
  loadDiagnostic = {};
  refreshProviderState();

  const std::string origin = serverOrigin();
  std::vector<ir::IrUploadCandidate> candidates;
  std::optional<int> beforeResultId;
  do {
    auto page = context.replayRepository.ListIrUploadCandidates(
        ir::kTachiProviderId, origin, beforeResultId);
    if (page.status != ir::IrUploadCandidateReadStatus::Loaded) {
      loadError = page.diagnostic.empty()
                      ? i18n::message("ir.upload.saved_results_failed_loaded.message")
                      : i18n::Text(ir::sanitizeDiagnostic(page.diagnostic));
      controller.applyCandidateRefresh(std::nullopt);
      refreshUi();
      return;
    }
    if (loadDiagnostic.empty() && !page.diagnostic.empty()) {
      loadDiagnostic = page.diagnostic;
    }
    const std::size_t available =
        ir::kMaximumIrUploadCandidateRows - candidates.size();
    const std::size_t accepted = std::min(available, page.candidates.size());
    candidates.insert(candidates.end(),
                      std::make_move_iterator(page.candidates.begin()),
                      std::make_move_iterator(page.candidates.begin() +
                                              static_cast<std::ptrdiff_t>(
                                                  accepted)));
    if (accepted != page.candidates.size() ||
        (candidates.size() == ir::kMaximumIrUploadCandidateRows &&
         page.nextBeforeModernChartResultId)) {
      if (loadDiagnostic.empty()) {
        loadDiagnostic = i18n::message("ir.upload.only_newest_saved_ir_candidates_shown.message");
      }
      break;
    }
    if (page.nextBeforeModernChartResultId && beforeResultId &&
        *page.nextBeforeModernChartResultId >= *beforeResultId) {
      loadError = i18n::message("ir.upload.pagination_failed.message");
      controller.applyCandidateRefresh(std::nullopt);
      refreshUi();
      return;
    }
    beforeResultId = page.nextBeforeModernChartResultId;
  } while (beforeResultId.has_value());

  controller.applyCandidateRefresh(std::move(candidates));
  candidatesLoaded = true;
  if (candidateList != nullptr) {
    candidateList->setCandidates(controller.candidates(),
                                 controller.selectedAttemptIds());
    candidateList->scrollOffset = scrollOffset;
  }
  refreshUi();
}

void IrUploadsScene::refreshProviderState() {
  const auto provider =
      context.settings.irProviders.find(std::string(ir::kTachiProviderId));
  const bool enabled = provider != context.settings.irProviders.end() &&
                       provider->second.enabled;
  const bool httpsOrigin =
      provider != context.settings.irProviders.end() &&
      ir::isHttpsServerOrigin(provider->second.serverOrigin);
  const bool hasCredential =
      !context
           .lookupActiveIrCredential(context.profileManager.activeProfile().id,
                                     ir::kTachiProviderId)
           .empty();
  const auto driver = context.irDrivers.find(ir::kTachiProviderId);
  const bool driverCanSubmit = driver != nullptr &&
                               !driver->capabilities().readOnly &&
                               driver->capabilities().scoreSubmission;
  const auto availability = ir_uploads::evaluateProviderAvailability({
      .enabled = enabled,
      .hasCredential = hasCredential,
      .httpsOrigin = httpsOrigin,
      .driverCanSubmit = driverCanSubmit,
      .submissionServiceAvailable = context.irSubmissionService != nullptr,
  });
  providerCanSubmit = availability.canSubmit;

  if (providerStatusText != nullptr) {
    providerStatusText->setLocalizedText(availability.statusText);
  }
}

void IrUploadsScene::refreshUi() {
  const std::size_t candidateCount = controller.candidates().size();
  const std::size_t selectedCount = controller.selectedCount();
  const bool locked = controller.selectionLocked();

  if (candidateCountText != nullptr) {
    candidateCountText->setLocalizedText(i18n::message(
        candidateCount == 1 ? "ir.upload.score_count.one" : "ir.upload.score_count.other",
        {{"count", std::to_string(candidateCount)}}));
  }
  if (selectionCountText != nullptr) {
    selectionCountText->setLocalizedText(i18n::message(
        "ir.upload.selection.count", {{"count", std::to_string(selectedCount)}}));
  }
  if (uploadButtonText != nullptr) {
    uploadButtonText->setLocalizedText(i18n::message(
        selectedCount == 1 ? "ir.upload.selection.action.one" : "ir.upload.selection.action.other",
        {{"count", std::to_string(selectedCount)}}));
  }
  if (progressText != nullptr) {
    const auto &status = controller.statusMessage();
    progressText->setLocalizedText(status.empty() ? loadDiagnostic : status);
  }
  if (refreshButton != nullptr) {
    refreshButton->setEnabled(!locked);
  }
  if (selectAllButton != nullptr) {
    selectAllButton->setEnabled(!locked && candidateCount > 0);
  }
  if (clearButton != nullptr) {
    clearButton->setEnabled(!locked && selectedCount > 0);
  }
  if (uploadButton != nullptr) {
    uploadButton->setEnabled(!locked && providerCanSubmit && selectedCount > 0);
  }
  if (candidateList != nullptr) {
    candidateList->setSelectedAttemptIds(controller.selectedAttemptIds());
    candidateList->setSelectionLocked(locked);
    candidateList->setVisible(loadError.empty() && candidateCount > 0);
  }
  if (stateText != nullptr) {
    const bool visible = !loadError.empty() || candidateCount == 0;
    stateText->setVisible(visible);
    stateText->setLocalizedText(!loadError.empty() ? loadError
                                          : i18n::message("ir.upload.no_scores_waiting_ir_upload.message"));
    stateText->setThemedColor(!loadError.empty() ? app_theme::coral
                                                 : app_theme::textSecondary);
  }
  if (rootLayout != nullptr) {
    rootLayout->applyYogaLayout();
  }
}

void IrUploadsScene::observeRemoteRevisions() {
  bool changed = false;
  const auto scoreRevision = context.scoreRepository.GetRevision();
  const auto profileRevision = context.profileCacheRevision.load();
  const auto origin = serverOrigin();
  if (scoreRevision != observedScoreRevision ||
      profileRevision != observedProfileCacheRevision ||
      origin != observedServerOrigin) {
    observedScoreRevision = scoreRevision;
    observedProfileCacheRevision = profileRevision;
    observedServerOrigin = origin;
    changed = true;
  }
  const std::uint64_t accountRevision =
      context.irAccountEvidenceRevision.load(std::memory_order_acquire);
  if (accountRevision != observedAccountEvidenceRevision) {
    observedAccountEvidenceRevision = accountRevision;
    changed = true;
  }
  if (context.irSubmissionService != nullptr) {
    const std::uint64_t attemptRevision =
        context.irSubmissionService
            ->status(ir::kTachiProviderId, std::string_view{})
            .revision;
    if (attemptRevision != observedAttemptStatusRevision) {
      observedAttemptStatusRevision = attemptRevision;
      changed = true;
    }
  }
  reloadRequested = reloadRequested || changed;
}

void IrUploadsScene::applyPreparationUpdates() {
  auto updates = preparationTask.takeUpdates();
  if (updates.progress) {
    controller.setPreparationProgress(updates.progress->first, updates.progress->second);
  }
  if (updates.completion) {
    controller.completePreparation(*updates.completion);
    reloadRequested = true;
  }
  if (presentationActive && (updates.progress || updates.completion)) {
    refreshUi();
  }
}

void IrUploadsScene::startUpload() {
  if (controller.selectionLocked()) {
    return;
  }
  refreshProviderState();
  if (!providerCanSubmit) {
    refreshUi();
    return;
  }
  auto candidates = controller.beginPreparation();
  if (candidates.empty()) {
    refreshUi();
    return;
  }
  refreshUi();

  ir_uploads::PreparationDependencies dependencies;
  dependencies.verify = [](const ir::IrUploadCandidate &candidate,
                           const std::stop_token &) {
    std::string diagnostic;
    auto submission = ir::submissionForIrUploadCandidate(candidate, diagnostic);
    return ir_uploads::VerificationOutcome{
        .submission = std::move(submission), .diagnostic = std::move(diagnostic)};
  };
  dependencies.enqueueBatch =
      [&application = context](std::span<const ir::IrSubmission> submissions) {
        return ir::executeIrSavedResultBatchUpload(
            ir::kTachiProviderId, submissions,
            {.buildDraft =
                 [&application](const ir::IrSubmission &submission) {
                   return application.irDrivers.buildDraft(ir::kTachiProviderId,
                                                            submission);
                 },
             .enqueueBatch =
                 [&application](std::span<const ir::IrOutboxDraft> drafts) {
                   return application.irSubmissionService->enqueueManualBatch(drafts);
                 }});
      };
  preparationTask.start(std::move(candidates), std::move(dependencies));
}

void IrUploadsScene::stopPreparation() {
  if (!preparationTask.hasWorker()) {
    return;
  }
  controller.markCancellationRequested();
  refreshUi();
  preparationTask.stopAndWait();
}

void IrUploadsScene::goBack() {
  if (embeddedTab) {
    if (tabClose) tabClose();
    return;
  }
  stopPreparation();
  if (context.sceneManager != nullptr) {
    (void)returnToScene(*context.sceneManager, returnTarget_);
  }
}

void IrUploadsScene::openIrSettings() {
  stopPreparation();
  if (context.sceneManager != nullptr) {
    returnTarget_.notifySettingsWillOpen();
    context.sceneManager->changeScene(
        std::make_unique<SettingsScene>(
            context, SettingsDestination::Ir,
            embeddedTab ? SceneReturnTarget::Retained(tabSettingsReturnOwner)
                        : returnTarget_),
        embeddedTab);
  }
}
