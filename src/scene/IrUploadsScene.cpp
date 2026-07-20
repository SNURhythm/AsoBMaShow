#include "IrUploadsScene.h"

#include "../ResultRecallBuilder.h"
#include "../ir/IrCredentialStore.h"
#include "../ir/IrProfileSettings.h"
#include "../ir/IrSubmissionService.h"
#include "../rendering/common.h"
#include "../targets.h"
#include "../view/Button.h"
#include "../view/IrUploadCandidateListView.h"
#include "../view/TextView.h"
#include "../view/UiTheme.h"
#include "SceneManager.h"
#include "SettingsScene.h"
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
#include "../iOSNatives.hpp"
#endif

#include <SDL2/SDL.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <span>
#include <stop_token>
#include <utility>
#include <vector>

namespace {

constexpr const char *kFontPath = "assets/fonts/notosanscjkjp.ttf";
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

Button *makeButton(const std::string &label, int fontSize,
                   TextView **textOut = nullptr) {
  auto *button = new Button();
  button->setHeight(52);
  button->setCornerRadius(ui_theme::controlRadius());
  button->setThemedBackgroundColors(ui_theme::control, ui_theme::controlHover,
                                    ui_theme::controlPressed);
  button->setThemedBorderColors(ui_theme::hairlineStrong,
                                ui_theme::hairlineStrong,
                                ui_theme::hairlineStrong);
  button->setStyledBorderWidth(1);

  auto *text = new TextView(kFontPath, fontSize);
  text->setText(label);
  text->setAlign(TextView::CENTER);
  text->setVAlign(TextView::MIDDLE);
  text->setOverflow(TextView::TextOverflow::Hidden);
  text->setThemedColor([] { return ui_theme::textOn(ui_theme::control()); });
  button->setContentView(text);
  if (textOut != nullptr) {
    *textOut = text;
  }
  return button;
}

TextView *makeText(const std::string &text, int fontSize,
                   View::ThemeColorProvider color) {
  auto *view = new TextView(kFontPath, fontSize);
  view->setText(text);
  view->setThemedColor(std::move(color));
  view->setOverflow(TextView::TextOverflow::Hidden);
  return view;
}

} // namespace

void IrUploadsScene::init() {
  context.jukebox.stop();
  mailbox = std::make_shared<IrUploadsSceneMailbox>();
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
  if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) {
    goBack();
    return {};
  }
  return Scene::handleEvents(event);
}

void IrUploadsScene::update(float) {
  applyMailbox();
  observeRemoteRevisions();
  if (reloadRequested && !controller.selectionLocked()) {
    reloadRequested = false;
    reloadCandidates();
  }

  if (rootLayout != nullptr && (lastLayoutWidth != rendering::window_width ||
                                lastLayoutHeight != rendering::window_height)) {
    lastLayoutWidth = rendering::window_width;
    lastLayoutHeight = rendering::window_height;
    rootLayout->setSize(rendering::window_width, rendering::window_height);
    rootLayout->applyYogaLayout();
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
  mailbox.reset();
  controller = {};
  loadError.clear();
  loadDiagnostic.clear();
  providerCanSubmit = false;
  reloadRequested = false;
}

void IrUploadsScene::buildView() {
  const SafeAreaInsets safe = getSafeAreaInsetsUi();
  rootLayout =
      new View(0, 0, rendering::window_width, rendering::window_height);
  rootLayout->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setThemedBackgroundColor(ui_theme::mainMenuBackdrop);
  addView(rootLayout);

  auto *header = new View();
  header->setHeight(safe.top + kHeaderHeight)
      ->setFlexDirection(FlexDirection::Row)
      ->setAlignItems(YGAlignCenter)
      ->setGap(14)
      ->setPadding(Edge::Top, safe.top + 10)
      ->setPadding(Edge::Left, safe.left + kScreenPadding)
      ->setPadding(Edge::Right, safe.right + kScreenPadding)
      ->setPadding(Edge::Bottom, 10)
      ->setThemedBackgroundColor(ui_theme::panelStrong)
      ->setThemedShadow(ui_theme::shadow, ui_theme::kHeaderShadow)
      ->setThemedBorderColor(ui_theme::hairline)
      ->setBorderWidth(1);

  auto *backButton = makeButton("Back", 20);
  backButton->setWidth(118);
  backButton->setOnClickListener([this]() { goBack(); });
  header->addView(backButton);

  auto *title = makeText("IR Uploads", 34, ui_theme::textPrimary);
  title->setHeight(42);
  title->setFlex(1);
  header->addView(title);

  candidateCountText = makeText("0 scores", 18, ui_theme::textSecondary);
  candidateCountText->setWidth(150);
  candidateCountText->setHeight(32);
  candidateCountText->setAlign(TextView::RIGHT);
  candidateCountText->setVAlign(TextView::MIDDLE);
  header->addView(candidateCountText);

  refreshButton = makeButton("Refresh", 20);
  refreshButton->setWidth(132);
  refreshButton->setOnClickListener([this]() {
    if (!controller.selectionLocked()) {
      reloadCandidates();
    }
  });
  header->addView(refreshButton);
  rootLayout->addView(header);

  auto *content = new View();
  content->setFlex(1)
      ->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setGap(12)
      ->setPadding(Edge::Top, 16)
      ->setPadding(Edge::Bottom, safe.bottom + 16)
      ->setPadding(Edge::Left, safe.left + kScreenPadding)
      ->setPadding(Edge::Right, safe.right + kScreenPadding);

  auto *providerCard = new View();
  providerCard->setHeight(94)
      ->setFlexShrink(0)
      ->setFlexDirection(FlexDirection::Row)
      ->setAlignItems(YGAlignCenter)
      ->setGap(16)
      ->setPadding(Edge::All, 16)
      ->setThemedBackgroundColor(ui_theme::panelStrong)
      ->setThemedBorderColor(ui_theme::hairline)
      ->setBorderWidth(1)
      ->setCornerRadius(ui_theme::panelRadius());
  auto *providerColumn = new View();
  providerColumn->setFlex(1)
      ->setMinWidth(0)
      ->setFlexDirection(FlexDirection::Column)
      ->setGap(4);
  auto *providerTitle = makeText("Bokutachi", 24, ui_theme::textPrimary);
  providerTitle->setHeight(32);
  providerColumn->addView(providerTitle);
  providerStatusText =
      makeText("Checking configuration...", 17, ui_theme::textSecondary);
  providerStatusText->setHeight(26);
  providerColumn->addView(providerStatusText);
  providerCard->addView(providerColumn);
  openIrSettingsButton = makeButton("Open IR Settings", 18);
  openIrSettingsButton->setWidth(204);
  openIrSettingsButton->setOnClickListener([this]() { openIrSettings(); });
  providerCard->addView(openIrSettingsButton);
  content->addView(providerCard);

  auto *selectionToolbar = new View();
  selectionToolbar->setHeight(58)
      ->setFlexShrink(0)
      ->setFlexDirection(FlexDirection::Row)
      ->setAlignItems(YGAlignCenter)
      ->setGap(10);
  selectAllButton = makeButton("Select All", 18);
  selectAllButton->setWidth(146);
  selectAllButton->setOnClickListener([this]() {
    controller.selectAll();
    refreshUi();
  });
  selectionToolbar->addView(selectAllButton);
  clearButton = makeButton("Clear", 18);
  clearButton->setWidth(112);
  clearButton->setOnClickListener([this]() {
    controller.clearSelection();
    refreshUi();
  });
  selectionToolbar->addView(clearButton);
  selectionCountText = makeText("0 selected", 18, ui_theme::textSecondary);
  selectionCountText->setHeight(32);
  selectionCountText->setVAlign(TextView::MIDDLE);
  selectionToolbar->addView(selectionCountText);
  content->addView(selectionToolbar);

  auto *listPanel = new View();
  listPanel->setFlex(1)
      ->setMinHeight(0)
      ->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setPadding(Edge::All, 10)
      ->setThemedBackgroundColor(ui_theme::mainMenuPanel)
      ->setThemedBorderColor(ui_theme::hairline)
      ->setBorderWidth(1)
      ->setCornerRadius(ui_theme::panelRadius());
  candidateList = new IrUploadCandidateListView();
  candidateList->setFlex(1);
  candidateList->clearBackgroundColor();
  candidateList->setBorderWidth(0);
  candidateList->onSelectionToggle = [this](int replayId) {
    controller.toggle(replayId);
    refreshUi();
  };
  candidateList->onSelected = [this](const ir::IrUploadCandidate &candidate,
                                     int) {
    controller.toggle(candidate.replayId());
    refreshUi();
  };
  listPanel->addView(candidateList);
  stateText =
      makeText("No scores waiting for IR upload.", 21, ui_theme::textSecondary);
  stateText->setHeight(58);
  stateText->setWrap(true);
  stateText->setAlign(TextView::CENTER);
  stateText->setVAlign(TextView::MIDDLE);
  listPanel->addView(stateText);
  content->addView(listPanel);

  auto *footer = new View();
  footer->setHeight(70)
      ->setFlexShrink(0)
      ->setFlexDirection(FlexDirection::Row)
      ->setAlignItems(YGAlignCenter)
      ->setGap(14)
      ->setPadding(Edge::Top, 8)
      ->setThemedBackgroundColor(ui_theme::mainMenuBackdrop);
  progressText = makeText("", 18, ui_theme::textSecondary);
  progressText->setFlex(1);
  progressText->setHeight(48);
  progressText->setWrap(true);
  progressText->setVAlign(TextView::MIDDLE);
  footer->addView(progressText);
  uploadButton = makeButton("Upload 0 Scores", 20, &uploadButtonText);
  uploadButton->setWidth(238);
  uploadButton->setOnClickListener([this]() { startUpload(); });
  footer->addView(uploadButton);
  content->addView(footer);

  rootLayout->addView(content);
  rootLayout->applyYogaLayout();
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
  const float scrollOffset =
      candidateList != nullptr ? candidateList->scrollOffset : 0.0F;
  loadError.clear();
  loadDiagnostic.clear();
  refreshProviderState();

  const std::string origin = serverOrigin();
  const auto replayRead = context.replayRepository.ListIrUploadCandidateReplays(
      ir::kTachiProviderId, origin);
  if (replayRead.status != IrUploadReplayReadStatus::Loaded) {
    loadError = replayRead.diagnostic.empty()
                    ? "Saved results could not be loaded."
                    : ir::sanitizeDiagnostic(replayRead.diagnostic);
    controller.applyCandidateRefresh(std::nullopt);
    refreshUi();
    return;
  }

  std::vector<std::filesystem::path> paths;
  paths.reserve(replayRead.replays.size());
  for (const ReplaySummary &replay : replayRead.replays) {
    if (replay.chartMeta.has_value()) {
      paths.push_back(replay.chartMeta->BmsPath);
    }
  }

  auto chartSession = context.chartRepository.OpenSession();
  if (!chartSession.has_value()) {
    loadError = "The chart library could not be opened.";
    controller.applyCandidateRefresh(std::nullopt);
    refreshUi();
    return;
  }
  const auto chartRead = chartSession->SelectChartMetaByPaths(paths);
  if (chartRead.status != ChartMetaPathBatchReadStatus::Loaded) {
    loadError = chartRead.diagnostic.empty()
                    ? "Chart details could not be loaded."
                    : ir::sanitizeDiagnostic(chartRead.diagnostic);
    controller.applyCandidateRefresh(std::nullopt);
    refreshUi();
    return;
  }

  auto projection =
      ir::projectIrUploadCandidates(replayRead.replays, chartRead.records);
  loadDiagnostic = projection.diagnostic;
  if (loadDiagnostic.empty()) {
    loadDiagnostic = replayRead.diagnostic;
  }
  controller.applyCandidateRefresh(std::move(projection.candidates));
  if (candidateList != nullptr) {
    candidateList->setCandidates(controller.candidates(),
                                 controller.selectedReplayIds());
    candidateList->scrollOffset = scrollOffset;
  }
  refreshUi();
}

void IrUploadsScene::refreshProviderState() {
  const auto provider =
      context.settings.irProviders.find(std::string(ir::kTachiProviderId));
  const bool enabled = provider != context.settings.irProviders.end() &&
                       provider->second.enabled;
  const auto credentials = ir::IrCredentialStore::load(
      context.profileManager.activePaths().irCredentialsJson);
  const auto key =
      credentials.credentials.apiKeys.find(std::string(ir::kTachiProviderId));
  const bool hasCredential =
      credentials.status == ir::IrCredentialLoadStatus::Loaded &&
      key != credentials.credentials.apiKeys.end() && !key->second.empty();
  const auto driver = context.irDrivers.find(ir::kTachiProviderId);
  const bool driverCanSubmit = driver != nullptr &&
                               !driver->capabilities().readOnly &&
                               driver->capabilities().scoreSubmission;
  providerCanSubmit = enabled && hasCredential && driverCanSubmit &&
                      context.irSubmissionService != nullptr;

  if (providerStatusText == nullptr) {
    return;
  }
  if (!enabled) {
    providerStatusText->setText(
        "Bokutachi is disabled. Enable it before uploading.");
  } else if (!hasCredential) {
    providerStatusText->setText(
        "A Bokutachi API key is required before uploading.");
  } else if (!driverCanSubmit || context.irSubmissionService == nullptr) {
    providerStatusText->setText("Bokutachi score submission is unavailable.");
  } else {
    providerStatusText->setText(
        "Ready to queue verified scores for batch delivery.");
  }
}

void IrUploadsScene::refreshUi() {
  const std::size_t candidateCount = controller.candidates().size();
  const std::size_t selectedCount = controller.selectedCount();
  const bool locked = controller.selectionLocked();

  if (candidateCountText != nullptr) {
    candidateCountText->setText(std::to_string(candidateCount) +
                                (candidateCount == 1 ? " score" : " scores"));
  }
  if (selectionCountText != nullptr) {
    selectionCountText->setText(std::to_string(selectedCount) + " selected");
  }
  if (uploadButtonText != nullptr) {
    uploadButtonText->setText("Upload " + std::to_string(selectedCount) +
                              (selectedCount == 1 ? " Score" : " Scores"));
  }
  if (progressText != nullptr) {
    const std::string &status = controller.statusText();
    progressText->setText(status.empty() ? loadDiagnostic : status);
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
    candidateList->setSelectedReplayIds(controller.selectedReplayIds());
    candidateList->setSelectionLocked(locked);
    candidateList->setVisible(loadError.empty() && candidateCount > 0);
  }
  if (stateText != nullptr) {
    const bool visible = !loadError.empty() || candidateCount == 0;
    stateText->setVisible(visible);
    stateText->setText(!loadError.empty() ? loadError
                                          : "No scores waiting for IR upload.");
    stateText->setThemedColor(!loadError.empty() ? ui_theme::coral
                                                 : ui_theme::textSecondary);
  }
  if (rootLayout != nullptr) {
    rootLayout->applyYogaLayout();
  }
}

void IrUploadsScene::observeRemoteRevisions() {
  bool changed = false;
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

void IrUploadsScene::applyMailbox() {
  if (mailbox == nullptr) {
    return;
  }
  std::optional<std::pair<std::size_t, std::size_t>> progress;
  std::optional<ir_uploads::PreparationOutcome> completion;
  {
    std::lock_guard lock(mailbox->mutex);
    progress = mailbox->progress;
    mailbox->progress.reset();
    completion = std::move(mailbox->completion);
    mailbox->completion.reset();
  }
  if (progress.has_value()) {
    controller.setPreparationProgress(progress->first, progress->second);
  }
  if (completion.has_value()) {
    if (preparationThread.joinable()) {
      preparationThread.join();
    }
    enqueueGate.reset();
    controller.completePreparation(*completion);
    reloadRequested = true;
  }
  if (progress.has_value() || completion.has_value()) {
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
  if (preparationThread.joinable()) {
    preparationThread.join();
  }
  if (mailbox == nullptr) {
    mailbox = std::make_shared<IrUploadsSceneMailbox>();
  }
  const auto workerMailbox = mailbox;
  enqueueGate = std::make_shared<ir_uploads::DurableEnqueueGate>();
  const auto workerEnqueueGate = enqueueGate;
  refreshUi();

  preparationThread = std::jthread([this, workerMailbox, workerEnqueueGate,
                                    candidates = std::move(candidates)](
                                       const std::stop_token &stopToken) {
    ir_uploads::PreparationDependencies dependencies;
    dependencies.verify = [this](const ir::IrUploadCandidate &candidate,
                                 const std::stop_token &stopToken) {
      auto stored = context.replayRepository.LoadReplayResult(
          candidate.replayId(), candidate.chart.meta);
      std::atomic_bool cancelled{stopToken.stop_requested()};
#if !defined(__ANDROID__) || defined(__cpp_lib_jthread)
      std::stop_callback stopCallback(stopToken,
                                      [&cancelled] { cancelled = true; });
#endif
      if (!stored) {
        return ir_uploads::VerificationOutcome{
            .diagnostic = "Saved replay data could not be loaded."};
      }
      auto recalled =
          result_recall::BuildChartResult(std::move(*stored), cancelled);
      if (!recalled.value) {
        return ir_uploads::VerificationOutcome{
            .diagnostic = recalled.diagnostic.empty()
                              ? "This saved result could not be reconstructed."
                              : ir::sanitizeDiagnostic(recalled.diagnostic)};
      }
      if (!recalled.value->historicalIr ||
          !recalled.value->historicalIr->submission) {
        return ir_uploads::VerificationOutcome{
            .diagnostic = "This saved result has no verifiable IR proof."};
      }
      return ir_uploads::VerificationOutcome{
          .submission = *recalled.value->historicalIr->submission};
    };
    dependencies.enqueueBatch =
        [this](std::span<const ir::IrSubmission> submissions) {
          return ir::executeIrSavedResultBatchUpload(
              ir::kTachiProviderId, submissions,
              {.buildDraft =
                   [this](const ir::IrSubmission &submission) {
                     return context.irDrivers.buildDraft(ir::kTachiProviderId,
                                                         submission);
                   },
               .enqueueBatch =
                   [this](std::span<const ir::IrOutboxDraft> drafts) {
                     return context.irSubmissionService->enqueueManualBatch(
                         drafts);
                   }});
        };
    dependencies.progress = [workerMailbox](std::size_t completed,
                                            std::size_t total) {
      std::lock_guard lock(workerMailbox->mutex);
      workerMailbox->progress = {completed, total};
    };
    auto outcome = ir_uploads::prepareSelectedCandidates(
        candidates, stopToken, dependencies, workerEnqueueGate);
    std::lock_guard lock(workerMailbox->mutex);
    workerMailbox->completion = std::move(outcome);
  });
}

void IrUploadsScene::stopPreparation() {
  if (!preparationThread.joinable()) {
    return;
  }
  controller.markCancellationRequested();
  refreshUi();
  if (enqueueGate != nullptr) {
    enqueueGate->requestCancellation();
  }
  preparationThread.request_stop();
  preparationThread.join();
  enqueueGate.reset();
}

void IrUploadsScene::goBack() {
  stopPreparation();
  if (context.sceneManager != nullptr) {
    context.sceneManager->changeScene("MainMenu", false);
  }
}

void IrUploadsScene::openIrSettings() {
  stopPreparation();
  if (context.sceneManager != nullptr) {
    context.sceneManager->changeScene(
        std::make_unique<SettingsScene>(context, SettingsDestination::Ir),
        false);
  }
}
