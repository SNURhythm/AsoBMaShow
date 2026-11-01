#include "FindBmsModal.h"
#include "FindBmsDialogPolicy.h"
#include "../downloads/DownloadService.h"
#include "FindBmsProgressPresentation.h"
#include "../PlatformOpen.h"
#include "../Utils.h"
#include "../library/ChartLibraryPlatform.h"
#include "../rendering/common.h"
#include "../rendering/ResponsiveUi.h"
#include "../rendering/UiSafeArea.h"
#include "../view/BlockingOverlayView.h"
#include "../view/Button.h"
#include "../view/ModalViewHelpers.h"
#include "../view/TextView.h"
#include "../view/AppTheme.h"

#include <algorithm>
#include <array>
#include <utility>

namespace {
using modal_view::makeModalButton;
using modal_view::modalPanelBorder;
using modal_view::styleThemedActionButton;

constexpr size_t kFindBmsMaxLogLines = 120;

bool ensureDirectoryExistsLogged(const std::filesystem::path &path,
                                 const char *description) {
  std::error_code error;
  if (Utils::EnsureDirectoryExists(path, error)) {
    return true;
  }

  SDL_Log("Failed to create %s %s: %s", description,
          fspath_to_utf8(path).c_str(), error.message().c_str());
  return false;
}

std::string findBmsManualSourceUrl(const BmsSearchResult &result) {
  if (!result.fallbackUrl.empty()) {
    return result.fallbackUrl;
  }
  if ((result.status == BmsSearchResult::Status::DownloadFailed ||
       result.status == BmsSearchResult::Status::HashMismatch) &&
      !result.downloadUrl.empty()) {
    return result.downloadUrl;
  }
  return result.patternUrl;
}

std::string findBmsTitleSearchQuery(const ChartMetaRecord &record) {
  std::string query = record.meta.Title;
  if (!query.empty() && !record.meta.Artist.empty()) {
    query += " " + record.meta.Artist;
  }
  if (query.empty()) {
    query = !record.meta.MD5.empty() ? record.meta.MD5 : record.meta.SHA256;
  }
  return query;
}

bool messageStartsWith(const std::string &message, const std::string &prefix) {
  return message.rfind(prefix, 0) == 0;
}


double progressRatio(const BmsSearchDownloadProgress &progress) {
  if (progress.totalBytes == 0) {
    return 0.0;
  }
  return std::clamp(static_cast<double>(progress.downloadedBytes) /
                        static_cast<double>(progress.totalBytes),
                    0.0, 1.0);
}

std::string
findBmsProgressEventDisplayText(const BmsSearchDownloadProgress &progress,
                                bool includeBytes) {
  if (progress.storageProgress) return findBmsStorageProgressMessage(*progress.storageProgress).resolve();
  return findBmsProgressDisplayText(progress.message, progress.downloadedBytes,
                                    progress.totalBytes, includeBytes);
}

bool shouldReplaceFindBmsLogLine(const std::string &previous,
                                 const std::string &next) {
  for (const char *prefix : {"Downloading archive", "Extracting "}) {
    if (messageStartsWith(previous, prefix) &&
        messageStartsWith(next, prefix)) {
      return true;
    }
  }
  return false;
}

double findBmsProgressFractionFor(const BmsSearchDownloadProgress &progress,
                                  double previous) {
  if (progress.storageProgress) return std::min(0.995, std::max(previous, 0.98));
  const std::string &message = progress.message;
  if (message == "Preparing lookup") {
    return std::max(previous, 0.02);
  }
  if (message == "Opening BMS Search pattern page") {
    return std::max(previous, 0.04);
  }
  if (message == "Opening BMS Search details page") {
    return std::max(previous, 0.07);
  }
  if (messageStartsWith(message, "Searching ") &&
      message.find(" package source") != std::string::npos) {
    return std::max(previous, 0.08);
  }
  if (messageStartsWith(message, "Preparing ") &&
      message.find(" package download") != std::string::npos) {
    return std::max(previous, 0.09);
  }
  if (message == "Downloading archive") {
    const double ratio = progressRatio(progress);
    if (progress.totalBytes > 0) {
      return std::max(previous, 0.10 + ratio * 0.80);
    }
    return std::min(0.90, std::max(previous + 0.003, 0.10));
  }
  if (message == "Download complete") {
    const double ratio = progressRatio(progress);
    return std::max(previous,
                    progress.totalBytes > 0 ? 0.10 + ratio * 0.80 : 0.90);
  }
  if (message == "Confirming Google Drive download") {
    return 0.10;
  }
  if (message == "Extracting archive") {
    return std::max(previous, 0.92);
  }
  if (messageStartsWith(message, "Extracting ")) {
    const double ratio = progressRatio(progress);
    if (progress.totalBytes > 0) {
      return std::max(previous, 0.92 + ratio * 0.06);
    }
    return std::min(0.98, std::max(previous + 0.005, 0.93));
  }
  return std::max(previous, 0.05);
}

} // namespace

std::filesystem::path findBmsDownloadRoot(ChartRepository::Session *session) {
  const auto fallback = ChartRepository::DefaultBmsFolderPath();
  if (session == nullptr) {
    ensureDirectoryExistsLogged(fallback, "BMS download root");
    return fallback;
  }

  const auto selected = session->SelectPrimaryStorageEntry();
  if (!selected.has_value()) {
    ensureDirectoryExistsLogged(fallback, "BMS download root");
    return fallback;
  }

  return chart_library_platform::resolveFolderEntryPath(*selected);
}

std::unique_ptr<FindBmsModal> FindBmsModal::Create(
    View *parent, FindBmsModalCallbacks callbacks) {
  if (parent == nullptr) return nullptr;
  auto modal = std::unique_ptr<FindBmsModal>(
      new FindBmsModal(std::move(callbacks)));
  modal->build(parent);
  return modal;
}

FindBmsModal::FindBmsModal(FindBmsModalCallbacks callbacks)
    : callbacks_(std::move(callbacks)) {
#if TARGET_OS_IPHONE || TARGET_OS_ANDROID
  callbacks_.readDownload = [](const std::string &id) -> std::optional<downloads::Job> {
    // jobs() includes transient import progress as well as the durable state.
    for (auto &job : downloads::service()->jobs()) if (job.id == id) return std::move(job);
    return {};
  };
  callbacks_.cancelDownload = [](const std::string &id) { return downloads::service()->cancel(id); };
  callbacks_.retryDownload = [](const std::string &id) { return downloads::service()->retry(id); };
  callbacks_.decideDownload = [](const std::string &id, BmsSearchPendingArtifactDecision decision) {
    return downloads::service()->decide(id, decision);
  };
#endif
}

FindBmsModal::~FindBmsModal() {
  cancelAndWait();
  for (auto *button : {findBmsCloseButton, findBmsBackgroundButton, findBmsConfirmButton, findBmsRetryButton,
                       findBmsKeepFilesButton,
                       findBmsDeleteFilesButton, findBmsOpenButton,
                       findBmsGoogleButton, findBmsRefreshButton}) {
    if (button != nullptr) button->setOnClickListener(nullptr);
  }
}

bool FindBmsModal::inProgress() const { return findBmsTask.running() || queuedDownloadRunning_; }

bool FindBmsModal::isVisible() const {
  return findBmsModalRoot != nullptr && findBmsModalRoot->getVisible();
}

View *FindBmsModal::root() const { return findBmsModalRoot; }

void FindBmsModal::resize(int width, int height) {
  if (findBmsModalRoot == nullptr) return;
  View::LayoutBatchScope layoutBatch;
  const auto safe = rendering::uiSafeAreaInsets();
  findBmsModalRoot->setSize(width, height);
  findBmsModalRoot->setPadding(Edge::Top, safe.top)->setPadding(Edge::Bottom, safe.bottom);
  findBmsModalRoot->setPadding(Edge::Left, safe.left)->setPadding(Edge::Right, safe.right);
  const float panelWidth = std::max(0.0f, std::min(760.0f, float(width - safe.left - safe.right - 24)));
  const float panelHeight = std::max(0.0f, std::min(560.0f, float(height - safe.top - safe.bottom - 24)));
  const float contentWidth = std::max(0.0f, panelWidth - 46);
  findBmsModalRoot->findViewByName("findBmsPanel")->setWidth(panelWidth)->setHeight(panelHeight);
  findBmsDetailText->setMinHeight(0);
  findBmsProgressTrack->setWidth(contentWidth);
  auto *footer = findBmsModalRoot->findViewByName("findBmsFooter");
  footer->setFlexWrap(YGWrapWrap)->setHeight(YGUndefined)->setFlexShrink(0);
  const float actionHeight = rendering::compactMenuCanvasActive ? rendering::compactTouchTarget : 58;
  const std::array<float, 9> widths{130, 240, 240, 150, 150, 150, 180, 150, 150};
  size_t index = 0;
  for (auto *button : {findBmsCloseButton, findBmsBackgroundButton, findBmsConfirmButton, findBmsRetryButton,
                       findBmsKeepFilesButton, findBmsDeleteFilesButton,
                       findBmsOpenButton, findBmsGoogleButton, findBmsRefreshButton}) {
    button->setWidth(std::min(widths[index++], contentWidth));
    button->setHeight(actionHeight)->setFlexShrink(0);
    button->setDisplay(button->getVisible() ? YGDisplayFlex : YGDisplayNone);
  }
}

void FindBmsModal::cancelAndWait() {
  findBmsAwaitingConfirmation = false;
  findBmsTask.stopAndWait();
  if (findBmsModalRoot != nullptr) findBmsModalRoot->setVisible(false);
}

bool FindBmsModal::handleEvents(SDL_Event &event) {
  if (!isVisible()) return true;
  if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
    if (event.key.repeat == 0) cancelOrClose();
    return false;
  }
  (void)findBmsModalRoot->handleEvents(event);
  return false;
}

void FindBmsModal::cancelOrClose() {
  const bool wasRunning = inProgress();
  update();
  const bool running = wasRunning || inProgress();
  if (!findBmsDialogPolicy(running, findBmsResult).showCloseOrCancel) {
    return;
  }
  if (running) {
    if (!findBmsResult.queuedDownloadId.empty() && callbacks_.cancelDownload) {
      try { callbacks_.cancelDownload(findBmsResult.queuedDownloadId); } catch (...) {}
      updateQueuedDownload(true);
    } else findBmsTask.requestCancel();
    refresh();
    return;
  }
  hide();
}

void FindBmsModal::build(View *parent) {
  if (parent == nullptr) {
    return;
  }

  constexpr float kModalPanelWidth = 760.0f;
  constexpr float kModalPanelPadding = 22.0f;
  constexpr float kModalContentWidth =
      kModalPanelWidth - kModalPanelPadding * 2.0f;

  findBmsModalRoot = new BlockingOverlayView(0, 0, rendering::window_width,
                                             rendering::window_height);
  findBmsModalRoot->setPositionType(YGPositionTypeAbsolute);
  findBmsModalRoot->setPosition(Edge::Left, 0);
  findBmsModalRoot->setPosition(Edge::Top, 0);
  findBmsModalRoot->setZIndex(1000);
  findBmsModalRoot->setVisible(false);
  findBmsModalRoot->setFlexDirection(FlexDirection::Column);
  findBmsModalRoot->setAlignItems(YGAlignCenter);
  findBmsModalRoot->setJustifyContent(YGJustifyCenter);
  findBmsModalRoot->setThemedBackgroundColor(app_theme::scrim);

  auto *panel = new View();
  panel->setName("findBmsPanel");
  panel->setWidth(kModalPanelWidth)
      ->setHeight(560)
      ->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setGap(14)
      ->setPadding(Edge::All, kModalPanelPadding)
      ->setThemedBackgroundColor(app_theme::panelStrong)
      ->setCornerRadius(app_theme::panelRadius())
      ->setThemedShadow(app_theme::shadow, app_theme::kModalShadow)
      ->setThemedBorderColor(modalPanelBorder)
      ->setBorderWidth(1);

  findBmsModalTitleText = new TextView(app_theme::boldFont, 30);
  findBmsModalTitleText->setLocalizedText(i18n::message("library.find_bms.find_bms.label"));
  findBmsModalTitleText->setThemedColor(app_theme::textPrimary);
  findBmsModalTitleText->setHeight(42);
  panel->addView(findBmsModalTitleText);

  findBmsStatusText = new TextView(app_theme::font, 22);
  findBmsStatusText->setLocalizedText(i18n::message("library.find_bms.preparing_lookup.label"));
  findBmsStatusText->setThemedColor(app_theme::textPrimary);
  findBmsStatusText->setWrap(true);
  findBmsStatusText->setOverflow(TextView::TextOverflow::Hidden);
  findBmsStatusText->setHeight(58);
  panel->addView(findBmsStatusText);

  findBmsDetailText = new TextView(app_theme::font, 18);
  findBmsDetailText->setText("");
  findBmsDetailText->setThemedColor(app_theme::textSecondary);
  findBmsDetailText->setWrap(true);
  findBmsDetailText->setOverflow(TextView::TextOverflow::Hidden);
  findBmsDetailText->setFlex(1);
  panel->addView(findBmsDetailText);

  findBmsProgressTrack = new View();
  findBmsProgressTrack->setWidth(kModalContentWidth)
      ->setHeight(24)
      ->setThemedBackgroundColor(app_theme::progressTrack)
      ->setCornerRadius(app_theme::controlRadius())
      ->setThemedBorderColor(app_theme::hairline)
      ->setBorderWidth(1);
  findBmsProgressFill = new View();
  findBmsProgressFill->setWidth(0)->setHeight(20)->setBackgroundColor(
      app_theme::progressFill());
  findBmsProgressTrack->addView(findBmsProgressFill);
  panel->addView(findBmsProgressTrack);

  auto *footer = new View();
  footer->setName("findBmsFooter");
  footer->setFlexDirection(FlexDirection::Row);
  footer->setJustifyContent(YGJustifyFlexEnd);
  footer->setAlignItems(YGAlignStretch);
  footer->setGap(12);
  footer->setHeight(58);

  findBmsCloseButton = makeModalButton(i18n::message("library.find_bms.cancel.label"), 20, &findBmsCloseButtonText);
  findBmsBackgroundButton = makeModalButton(
      i18n::message("library.find_bms.background.label"), 18, &findBmsBackgroundButtonText);
  findBmsConfirmButton = makeModalButton(
      i18n::message("library.find_bms.search_and_download.label"), 20,
      &findBmsConfirmButtonText);
  findBmsRetryButton = makeModalButton(i18n::message("library.find_bms.retry.label"), 20, &findBmsRetryButtonText);
  findBmsKeepFilesButton =
      makeModalButton(i18n::message("library.find_bms.keep_files.label"), 18, &findBmsKeepFilesButtonText);
  findBmsDeleteFilesButton =
      makeModalButton(i18n::message("library.find_bms.delete_files.label"), 18, &findBmsDeleteFilesButtonText);
  findBmsOpenButton = makeModalButton(i18n::message("library.find_bms.source.label"), 18, &findBmsOpenButtonText);
  findBmsGoogleButton = makeModalButton(i18n::message("library.find_bms.search.label"), 18, &findBmsGoogleButtonText);
  findBmsRefreshButton =
      makeModalButton(i18n::message("library.find_bms.refresh.label"), 18, &findBmsRefreshButtonText);

  findBmsCloseButton->setWidth(130);
  findBmsBackgroundButton->setWidth(240);
  findBmsConfirmButton->setWidth(240);
  findBmsRetryButton->setWidth(150);
  findBmsKeepFilesButton->setWidth(150);
  findBmsDeleteFilesButton->setWidth(150);
  findBmsOpenButton->setWidth(180);
  findBmsGoogleButton->setWidth(150);
  findBmsRefreshButton->setWidth(150);
  findBmsCloseButton->setOnClickListener([this]() { cancelOrClose(); });
  findBmsBackgroundButton->setOnClickListener([this]() { hide(); });
  findBmsConfirmButton->setOnClickListener([this]() { startLookup(); });
  findBmsRetryButton->setOnClickListener([this]() {
    if (!findBmsResult.queuedDownloadId.empty() && callbacks_.retryDownload) {
      try { callbacks_.retryDownload(findBmsResult.queuedDownloadId); } catch (...) {}
      updateQueuedDownload(true);
    } else findBmsTask.retryDownload();
    refresh();
  });
  findBmsKeepFilesButton->setOnClickListener([this]() {
    startPendingArtifactResolution(
        BmsSearchPendingArtifactDecision::Keep);
  });
  findBmsDeleteFilesButton->setOnClickListener([this]() {
    startPendingArtifactResolution(
        BmsSearchPendingArtifactDecision::Delete);
  });
  findBmsOpenButton->setOnClickListener([this]() {
    const std::string url = findBmsManualSourceUrl(findBmsResult);
    openResultUrl(url);
  });
  findBmsGoogleButton->setOnClickListener([this]() {
    openResultUrl(BmsSearchService::searchUrlForText(
        findBmsTitleSearchQuery(findBmsModalChart)));
  });
  findBmsRefreshButton->setOnClickListener([this]() {
    if (!findBmsResult.queuedDownloadId.empty() && callbacks_.openDownloads) {
      // The shared queue owns pending decisions, so it is safe to leave this view.
      findBmsModalRoot->setVisible(false);
      callbacks_.openDownloads(findBmsResult.queuedDownloadId);
      return;
    }
    if (callbacks_.refreshLibrary) callbacks_.refreshLibrary();
    hide();
  });

  footer->addView(findBmsCloseButton);
  footer->addView(findBmsBackgroundButton);
  footer->addView(findBmsConfirmButton);
  footer->addView(findBmsRetryButton);
  footer->addView(findBmsKeepFilesButton);
  footer->addView(findBmsDeleteFilesButton);
  footer->addView(findBmsOpenButton);
  footer->addView(findBmsGoogleButton);
  footer->addView(findBmsRefreshButton);
  panel->addView(footer);

  findBmsModalRoot->addView(panel);
  parent->addView(findBmsModalRoot);
  refresh();
}

void FindBmsModal::show(const ChartMetaRecord &record, bool requireConfirmation) {
#if TARGET_OS_IPHONE
  requireConfirmation = true;
#endif
  if (findBmsModalRoot == nullptr) return;
  if (inProgress() || findBmsResult.pendingArtifact) {
    // A retained scene may hide the dialog while a keep/delete decision is
    // pending. Reopen that decision without replacing its staged download.
    findBmsModalRoot->setVisible(true);
    refresh();
    return;
  }

  findBmsModalChart = record;
  findBmsResult = {};
  queuedDownloadRunning_ = false; queuedDownloadRetry_ = false;
  queuedDownloadSnapshot_.clear(); queuedDownloadUpdateAt_ = {};
  findBmsPendingDecision.reset();
  findBmsAwaitingConfirmation = true;
  findBmsProgressMessage.clear();
  findBmsProgressCurrent = 0;
  findBmsProgressTotal = 0;
  findBmsProgressFraction = 0.0;
  findBmsProgressLog.clear();
  findBmsModalRoot->setSize(rendering::window_width, rendering::window_height);
  findBmsModalRoot->setVisible(true);
  if (requireConfirmation) {
    refresh();
  } else {
    startLookup();
  }
}

void FindBmsModal::startLookup() {
  if (callbacks_.admit && !callbacks_.admit()) return;
  if (!findBmsAwaitingConfirmation || inProgress() ||
      findBmsResult.pendingArtifact) {
    return;
  }
  findBmsAwaitingConfirmation = false;
  const ChartMetaRecord record = findBmsModalChart;
  if (!record.meta.SHA256.empty()) {
    findBmsResult.patternUrl =
        BmsSearchService::patternUrlForSha256(record.meta.SHA256);
    findBmsResult.fallbackUrl = findBmsResult.patternUrl;
  } else {
    findBmsResult.fallbackUrl =
        BmsSearchService::searchUrlForText(findBmsTitleSearchQuery(record));
  }
  findBmsProgressMessage = "Preparing lookup";
  findBmsProgressCurrent = 0;
  findBmsProgressTotal = 0;
  findBmsProgressFraction = 0.02;
  findBmsProgressLog.clear();
  findBmsProgressLog.push_back(i18n::tr("library.find_bms.preparing_lookup.label"));

  const std::filesystem::path downloadRoot =
      callbacks_.downloadRoot ? callbacks_.downloadRoot() : std::filesystem::path{};
  BmsSearchDownloadOptions downloadOptions =
      callbacks_.downloadOptions ? callbacks_.downloadOptions()
                                 : BmsSearchDownloadOptions{};
  findBmsCapturedOptions = downloadOptions;
  findBmsStorageProgress.reset();
  downloadOptions.requestRetry = findBmsTask.retryCallback();
  if (callbacks_.downloadStarted) callbacks_.downloadStarted();
  findBmsTask.start([record, downloadRoot, downloadOptions](
                        std::atomic_bool &cancelled,
                        BmsSearchDownloadProgressCallback progress) {
    BmsSearchService service;
    return service.findAndDownload(
        record.meta.SHA256, record.meta.MD5, downloadRoot, cancelled,
        std::move(progress), record.meta.Title, record.meta.Artist,
        downloadOptions);
  });
  refresh();
}

void FindBmsModal::startPendingArtifactResolution(
    BmsSearchPendingArtifactDecision decision) {
  if (callbacks_.admit && !callbacks_.admit()) return;
  if (inProgress() || !findBmsResult.pendingArtifact) {
    return;
  }
  if (!findBmsResult.queuedDownloadId.empty() && callbacks_.decideDownload) {
    try { callbacks_.decideDownload(findBmsResult.queuedDownloadId, decision); } catch (...) {}
    updateQueuedDownload(true);
    refresh();
    return;
  }
  BmsSearchResult result = findBmsResult;
  findBmsPendingDecision = decision;
  findBmsProgressMessage =
      decision == BmsSearchPendingArtifactDecision::Keep ? i18n::tr("library.find_bms.keeping_files.label")
                                                        : i18n::tr("library.find_bms.deleting_files.label");
  findBmsProgressCurrent = 0;
  findBmsProgressTotal = 0;
  findBmsProgressFraction = 0.95;
  findBmsProgressLog.push_back(findBmsProgressMessage);
  findBmsTask.start([result = std::move(result), decision](
                        std::atomic_bool &, BmsSearchDownloadProgressCallback) mutable {
    BmsSearchService service;
    return service.resolvePendingArtifact(std::move(result), decision);
  });
  refresh();
}

void FindBmsModal::hide() {
  if (findBmsModalRoot == nullptr) return;
  const bool wasVisible = findBmsModalRoot->getVisible();
  // Consume any pending result without changing the user's choice to hide it.
  findBmsModalRoot->setVisible(false);
  const bool wasRunning = inProgress();
  update();
  const bool running = wasRunning || inProgress();
  if (!findBmsDialogPolicy(running, findBmsResult).canDismiss) {
    findBmsModalRoot->setVisible(wasVisible);
    return;
  }
  findBmsAwaitingConfirmation = false;
  findBmsModalRoot->setVisible(false);
}

void FindBmsModal::refresh() {
  if (findBmsModalRoot == nullptr) {
    return;
  }

  const bool running = inProgress();
  const auto retryRequest = findBmsTask.retryRequest();
  const bool canRetry = retryRequest.has_value() || queuedDownloadRetry_;
  const auto policy =
      findBmsDialogPolicy(inProgress(), findBmsResult);
  if (findBmsModalTitleText != nullptr) {
    findBmsModalTitleText->setLocalizedText(i18n::message("library.find_bms.find_bms.label"));
  }

  i18n::Text statusText;
  if (findBmsAwaitingConfirmation) {
    statusText = i18n::message("library.find_bms.chart_not_installed.label");
  } else if (retryRequest) {
    statusText = i18n::message("library.find_bms.download_failed.label");
  } else if (running) {
    if (findBmsPendingDecision) {
      statusText = *findBmsPendingDecision ==
                           BmsSearchPendingArtifactDecision::Keep
                       ? i18n::message("library.find_bms.keeping_files.label")
                       : i18n::message("library.find_bms.deleting_files.label");
    } else {
      statusText = findBmsStorageProgress ? findBmsStorageProgressMessage(*findBmsStorageProgress) :
          findBmsProgressDisplayMessage(findBmsProgressMessage,
                                              findBmsProgressCurrent,
                                              findBmsProgressTotal, true);
    }
  } else {
    switch (findBmsResult.status) {
    case BmsSearchResult::Status::Downloaded:
      statusText = i18n::message("library.find_bms.download_complete.label");
      break;
    case BmsSearchResult::Status::NoDownloadLink:
    case BmsSearchResult::Status::UnsupportedLink:
      statusText = i18n::message("library.find_bms.manual_download_needed.label");
      break;
    case BmsSearchResult::Status::NotFound:
      statusText = i18n::message("library.find_bms.not_found.label");
      break;
    case BmsSearchResult::Status::HashMismatch:
      statusText = findBmsResult.pendingArtifact ? i18n::message("library.find_bms.chart_mismatch.label")
                                                 : i18n::message("library.find_bms.decision_complete.label");
      break;
    case BmsSearchResult::Status::DownloadFailed:
      statusText = i18n::message("library.find_bms.download_failed.label");
      break;
    }
  }
  if (findBmsStatusText != nullptr) {
    findBmsStatusText->setLocalizedText(statusText);
    const bool failed =
        retryRequest.has_value() ||
        (!findBmsAwaitingConfirmation && !running &&
         (findBmsResult.status == BmsSearchResult::Status::DownloadFailed ||
          findBmsResult.status == BmsSearchResult::Status::HashMismatch ||
          findBmsResult.status == BmsSearchResult::Status::NotFound));
    findBmsStatusText->setColor(
        app_theme::sdl(failed ? app_theme::coral() : app_theme::textPrimary()));
  }

  i18n::Text detail;
  if (findBmsAwaitingConfirmation) {
    detail = i18n::message("library.find_bms.confirm_download.detail");
  } else if (retryRequest) {
    detail = i18n::message(
        retryRequest->canResume ? "library.find_bms.resume_prompt.message"
                                : "library.find_bms.retry_prompt.message",
        {{"error", retryRequest->message}});
  } else if (running && findBmsPendingDecision) {
    detail = i18n::message("library.find_bms.resolving_downloaded_files_dialog_unable_close_yet.message");
  } else if (!running && findBmsResult.pendingArtifact) {
    detail = findBmsResult.message.empty()
                  ? i18n::message("library.find_bms.choose_keep_files_delete_files_continue.message")
                  : findBmsDownloadFailureMessage(findBmsResult);
  } else if (!running &&
             findBmsResult.status == BmsSearchResult::Status::Downloaded) {
    detail = i18n::message("library.find_bms.adding_downloaded_charts_library.message");
  } else if (!running &&
             findBmsResult.status == BmsSearchResult::Status::NoDownloadLink) {
    detail = i18n::message("library.find_bms.download_from_source_then_refresh.message");
  } else if (!running &&
             findBmsResult.status == BmsSearchResult::Status::UnsupportedLink) {
    detail = i18n::message("library.find_bms.download_from_source_then_refresh.message");
  } else if (!running &&
             findBmsResult.status == BmsSearchResult::Status::NotFound) {
    detail = i18n::message("library.find_bms.try_searching_by_title.message");
  } else if (!running &&
             findBmsResult.status == BmsSearchResult::Status::HashMismatch) {
    detail = findBmsResult.message.empty()
                  ? i18n::message("library.find_bms.downloaded_archive_does_not_match_chart.message")
                  : findBmsDownloadFailureMessage(findBmsResult);
  } else if (!running &&
             findBmsResult.status == BmsSearchResult::Status::DownloadFailed) {
    detail = findBmsDownloadFailureMessage(findBmsResult);
  } else {
    detail = findBmsRunningDetailMessage(findBmsProgressMessage);
  }
  const auto displayedOptions = findBmsAwaitingConfirmation && callbacks_.downloadOptions
      ? callbacks_.downloadOptions() : findBmsCapturedOptions;
  if (findBmsAwaitingConfirmation || running || findBmsResult.status == BmsSearchResult::Status::Downloaded ||
      !storage_management::IsNoOp(displayedOptions.storagePolicy)) {
    detail = i18n::message("library.find_bms.storage.detail", {{"detail", detail},
        {"storage", findBmsStoragePolicyMessage(displayedOptions)}});
    if (findBmsResult.status == BmsSearchResult::Status::HashMismatch)
      detail = i18n::message("library.find_bms.storage.detail", {{"detail", detail},
          {"storage", i18n::message("library.find_bms.storage.mismatch_unchanged")}});
  }
  if (!running && findBmsResult.status == BmsSearchResult::Status::Downloaded && findBmsResult.storageReport)
    detail = i18n::message("library.find_bms.storage.detail", {{"detail", detail},
        {"storage", findBmsStorageReportMessage(*findBmsResult.storageReport)}});
  if (!findBmsModalChart.meta.Title.empty()) {
    detail = i18n::message("library.find_bms.chart_detail",
        {{"title", findBmsModalChart.meta.Title}, {"detail", detail}});
  }
#if TARGET_OS_IPHONE
  if (findBmsAwaitingConfirmation)
    detail = i18n::message("library.find_bms.storage.detail", {{"detail", detail},
        {"storage", i18n::message("downloads.ui.ios_notice")}});
#endif
  if (policy.showBackgroundClose)
    detail = i18n::message("library.find_bms.storage.detail", {{"detail", detail},
        {"storage", i18n::message("library.find_bms.background.message")}});
  if (findBmsDetailText != nullptr) {
    findBmsDetailText->setLocalizedText(detail);
  }

  if (findBmsProgressTrack != nullptr) {
    findBmsProgressTrack->setVisible(!findBmsAwaitingConfirmation);
  }
  const double fraction =
      (!running && findBmsResult.status == BmsSearchResult::Status::Downloaded)
          ? 1.0
          : findBmsProgressFraction;
  if (findBmsProgressFill != nullptr) {
    findBmsProgressFill->setWidthPercent(
        static_cast<float>(std::clamp(fraction, 0.0, 1.0) * 100.0));
  }

  const std::string manualSourceUrl = findBmsManualSourceUrl(findBmsResult);
  const bool downloaded =
      !running && findBmsResult.status == BmsSearchResult::Status::Downloaded;
  const bool hasSource =
      !findBmsAwaitingConfirmation && policy.showNormalResultActions &&
      !manualSourceUrl.empty() &&
      findBmsResult.status != BmsSearchResult::Status::Downloaded &&
      findBmsResult.status != BmsSearchResult::Status::NotFound;
  const bool hasSearchAction =
      !findBmsAwaitingConfirmation && policy.showNormalResultActions &&
      !downloaded &&
      (!findBmsModalChart.meta.SHA256.empty() ||
       !findBmsModalChart.meta.MD5.empty() ||
       !findBmsModalChart.meta.Title.empty() ||
       !findBmsModalChart.meta.Artist.empty());
  const bool hasSharedDownload = !findBmsResult.queuedDownloadId.empty();
  const bool hasRefreshAction = hasSharedDownload ||
      (!findBmsAwaitingConfirmation && policy.showNormalResultActions && !running && !downloaded);
  if (findBmsRefreshButtonText) findBmsRefreshButtonText->setLocalizedText(i18n::message(
      hasSharedDownload ? "downloads.ui.view" : "library.find_bms.refresh.label"));
  if (findBmsCloseButtonText != nullptr) {
    findBmsCloseButtonText->setLocalizedText(
        (running || findBmsAwaitingConfirmation)
            ? i18n::message("library.find_bms.cancel.label")
            : i18n::message("library.find_bms.close.label"));
  }
  if (findBmsCloseButton != nullptr) {
    findBmsCloseButton->setVisible(policy.showCloseOrCancel);
    findBmsCloseButton->setWidth(policy.showCloseOrCancel ? 130.0f : 0.0f);
  }
  if (findBmsBackgroundButton != nullptr) {
    findBmsBackgroundButton->setVisible(policy.showBackgroundClose);
    findBmsBackgroundButton->setWidth(policy.showBackgroundClose ? 240.0f : 0.0f);
  }
  if (findBmsConfirmButton != nullptr) {
    findBmsConfirmButton->setVisible(findBmsAwaitingConfirmation);
    findBmsConfirmButton->setWidth(findBmsAwaitingConfirmation ? 240.0f : 0.0f);
  }
  if (findBmsRetryButtonText != nullptr) {
    findBmsRetryButtonText->setLocalizedText(i18n::message(
        retryRequest && retryRequest->canResume ? "library.find_bms.resume.label"
                                               : "library.find_bms.retry.label"));
  }
  if (findBmsRetryButton != nullptr) {
    findBmsRetryButton->setVisible(canRetry);
    findBmsRetryButton->setWidth(canRetry ? 150.0f : 0.0f);
  }
  if (findBmsKeepFilesButton != nullptr) {
    findBmsKeepFilesButton->setVisible(policy.showPendingActions);
    findBmsKeepFilesButton->setWidth(policy.showPendingActions ? 150.0f
                                                              : 0.0f);
  }
  if (findBmsDeleteFilesButton != nullptr) {
    findBmsDeleteFilesButton->setVisible(policy.showPendingActions);
    findBmsDeleteFilesButton->setWidth(policy.showPendingActions ? 150.0f
                                                                : 0.0f);
  }
  if (findBmsOpenButtonText != nullptr) {
    const bool downloadSource =
        (findBmsResult.status == BmsSearchResult::Status::DownloadFailed ||
         findBmsResult.status == BmsSearchResult::Status::HashMismatch) &&
        findBmsResult.fallbackUrl.empty() && !findBmsResult.downloadUrl.empty();
    const bool bmsSearchSource =
        manualSourceUrl.find("bmssearch.net") != std::string::npos;
    findBmsOpenButtonText->setLocalizedText(
        downloadSource ? i18n::message("library.find_bms.download.label")
                       : (bmsSearchSource ? i18n::message("library.find_bms.bms_search.label") : i18n::message("library.find_bms.source.label")));
  }
  if (findBmsOpenButton != nullptr) {
    findBmsOpenButton->setVisible(!running && hasSource);
    findBmsOpenButton->setWidth((!running && hasSource) ? 180.0f : 0.0f);
  }
  if (findBmsGoogleButton != nullptr) {
    findBmsGoogleButton->setVisible(!running && hasSearchAction);
    findBmsGoogleButton->setWidth((!running && hasSearchAction) ? 150.0f
                                                                : 0.0f);
  }
  if (findBmsRefreshButton != nullptr) {
    findBmsRefreshButton->setVisible(hasRefreshAction);
    findBmsRefreshButton->setWidth(hasRefreshAction ? 150.0f : 0.0f);
  }
  // Visibility alone only suppresses drawing; hidden children still add gaps
  // to the footer's flex layout, even when their width is zero.
  for (auto *button : {findBmsCloseButton, findBmsBackgroundButton, findBmsConfirmButton, findBmsRetryButton,
                       findBmsKeepFilesButton, findBmsDeleteFilesButton,
                       findBmsOpenButton, findBmsGoogleButton, findBmsRefreshButton}) {
    if (button != nullptr) {
      button->setDisplay(button->getVisible() ? YGDisplayFlex : YGDisplayNone);
    }
  }

  styleThemedActionButton(findBmsCloseButton, findBmsCloseButtonText, true,
                          app_theme::control, app_theme::controlHover,
                          app_theme::controlPressed, app_theme::hairlineStrong);
  styleThemedActionButton(findBmsBackgroundButton, findBmsBackgroundButtonText, true,
                          app_theme::control, app_theme::controlHover,
                          app_theme::controlPressed, app_theme::hairlineStrong);
  styleThemedActionButton(findBmsConfirmButton, findBmsConfirmButtonText,
                          findBmsAwaitingConfirmation, app_theme::successAction,
                          app_theme::successActionHover,
                          app_theme::successActionPressed, app_theme::accentBorder);
  styleThemedActionButton(findBmsRetryButton, findBmsRetryButtonText,
                          canRetry, app_theme::successAction,
                          app_theme::successActionHover,
                          app_theme::successActionPressed, app_theme::accentBorder);
  styleThemedActionButton(
      findBmsKeepFilesButton, findBmsKeepFilesButtonText,
      policy.showPendingActions, app_theme::successAction,
      app_theme::successActionHover, app_theme::successActionPressed,
      app_theme::accentBorder);
  styleThemedActionButton(
      findBmsDeleteFilesButton, findBmsDeleteFilesButtonText,
      policy.showPendingActions, app_theme::dangerAction,
      app_theme::dangerActionHover, app_theme::dangerActionPressed,
      app_theme::accentBorder);
  styleThemedActionButton(findBmsOpenButton, findBmsOpenButtonText,
                          !running && hasSource, app_theme::infoAction,
                          app_theme::infoActionHover,
                          app_theme::infoActionPressed, app_theme::accentBorder);
  styleThemedActionButton(
      findBmsGoogleButton, findBmsGoogleButtonText, !running && hasSearchAction,
      app_theme::violetAction, app_theme::violetActionHover,
      app_theme::violetActionPressed, app_theme::violetActionHover);
  styleThemedActionButton(
      findBmsRefreshButton, findBmsRefreshButtonText, hasRefreshAction,
      app_theme::successAction, app_theme::successActionHover,
      app_theme::successActionPressed, app_theme::accentBorder);
  resize(rendering::window_width, rendering::window_height);
  findBmsModalRoot->applyYogaLayout();
}

void FindBmsModal::update() {
  auto updates = findBmsTask.takeUpdates();
  auto &progressEvents = updates.progress;
  auto &result = updates.result;

  auto appendLogLine = [this](const std::string &logLine, bool replace = false) {
    if (logLine.empty()) {
      return;
    }
    if (!findBmsProgressLog.empty() && replace) {
      findBmsProgressLog.back() = logLine;
    } else if (findBmsProgressLog.empty() ||
               findBmsProgressLog.back() != logLine) {
      findBmsProgressLog.push_back(logLine);
    }
    while (findBmsProgressLog.size() > kFindBmsMaxLogLines) {
      findBmsProgressLog.pop_front();
    }
  };

  bool shouldRefresh = updates.retryChanged;
  for (const auto &progress : progressEvents) {
    const bool replace =
        shouldReplaceFindBmsLogLine(findBmsProgressMessage, progress.message);
    findBmsStorageProgress = progress.storageProgress;
    findBmsProgressMessage = progress.message;
    findBmsProgressCurrent = progress.downloadedBytes;
    findBmsProgressTotal = progress.totalBytes;
    findBmsProgressFraction =
        findBmsProgressFractionFor(progress, findBmsProgressFraction);
    appendLogLine(findBmsProgressEventDisplayText(progress, true), replace);
    shouldRefresh = true;
  }
  if (result) {
    findBmsResult = std::move(*result);
    findBmsPendingDecision.reset();
    if (!findBmsResult.queuedDownloadId.empty()) {
      queuedDownloadRunning_ = true;
      queuedDownloadRetry_ = false;
      queuedDownloadSnapshot_.clear();
      findBmsProgressMessage = i18n::tr("downloads.ui.queued");
      findBmsProgressFraction = 0.0;
      updateQueuedDownload(true);
      refresh();
      return;
    }
    const bool keptMismatchedFiles =
        findBmsResult.status == BmsSearchResult::Status::HashMismatch &&
        !findBmsResult.pendingArtifact && !findBmsResult.outputPath.empty();
    if (findBmsResult.status == BmsSearchResult::Status::Downloaded ||
        keptMismatchedFiles) {
      findBmsProgressFraction = 1.0;
    }
    if (!findBmsResult.message.empty() &&
        (findBmsProgressLog.empty() ||
         findBmsProgressLog.back() != findBmsResult.message)) {
      appendLogLine(findBmsResult.message);
    }
    if (callbacks_.filesReady &&
        (findBmsResult.status == BmsSearchResult::Status::Downloaded ||
         keptMismatchedFiles)) {
      callbacks_.filesReady(
          findBmsModalChart, findBmsResult,
          findBmsResult.status == BmsSearchResult::Status::Downloaded);
    }
    shouldRefresh = true;
  }
  if (updateQueuedDownload()) shouldRefresh = true;
  if (shouldRefresh) {
    refresh();
  }
}

bool FindBmsModal::updateQueuedDownload(bool force) {
  const auto id = findBmsResult.queuedDownloadId;
  if (id.empty() || !callbacks_.readDownload) return false;
  const auto now = std::chrono::steady_clock::now();
  if (!force && now < queuedDownloadUpdateAt_) return false;
  queuedDownloadUpdateAt_ = now + std::chrono::milliseconds(300);
  try {
    const auto job = callbacks_.readDownload(id);
    if (!job) {
      queuedDownloadRunning_ = queuedDownloadRetry_ = false;
      findBmsResult.status = BmsSearchResult::Status::DownloadFailed;
      findBmsResult.pendingArtifact.reset();
      findBmsResult.presentationMessage = {};
      findBmsResult.message = i18n::tr("downloads.ui.unavailable");
      return true;
    }
    const auto snapshot = std::to_string(job->revision) + ":" + std::to_string(int(job->state)) + ":" +
        std::to_string(job->downloadedBytes) + ":" + std::to_string(job->totalBytes) + ":" + job->message +
        ":" + job->libraryError + ":" + std::to_string(job->libraryAcknowledged);
    if (!force && snapshot == queuedDownloadSnapshot_) return false;
    queuedDownloadSnapshot_ = snapshot;
    using downloads::State;
    queuedDownloadRunning_ = !job->persistenceBlocked && (job->state == State::Queued || job->state == State::Transferring ||
        job->state == State::Downloaded || job->state == State::Importing);
    queuedDownloadRetry_ = job->persistenceBlocked || job->state == State::Failed || !job->libraryError.empty() ||
        (job->state == State::NeedsAttention && (job->publicationPrepared || !job->result || !job->result->pendingArtifact));
    findBmsPendingDecision.reset();
    if (job->result) findBmsResult = *job->result;
    else findBmsResult = {};
    if (job->publicationPrepared) findBmsResult.pendingArtifact.reset();
    findBmsResult.queuedDownloadId = id;
    findBmsResult.downloadUrl = job->request.displayUrl.empty() ? job->request.url : job->request.displayUrl;
    findBmsProgressCurrent = job->downloadedBytes; findBmsProgressTotal = job->totalBytes;
    findBmsProgressMessage = job->message;
    if (job->state == State::Queued) findBmsProgressMessage = i18n::tr("downloads.ui.queued");
    else if (job->state == State::Transferring) findBmsProgressMessage = "Downloading archive";
    else if (job->state == State::Downloaded) findBmsProgressMessage = i18n::tr("downloads.ui.downloaded");
    else if (job->state == State::Importing && findBmsProgressMessage.empty()) findBmsProgressMessage = i18n::tr("downloads.ui.importing");
    if (job->state == State::Transferring)
      findBmsProgressFraction = job->totalBytes ? 0.1 + 0.75 * double(job->downloadedBytes) / job->totalBytes : 0.1;
    else if (job->state == State::Queued) findBmsProgressFraction = 0.0;
    else if (queuedDownloadRunning_) findBmsProgressFraction = 0.9;
    else if (job->state == State::Complete) findBmsProgressFraction = 1.0;
    if (job->persistenceBlocked || job->state == State::Failed || job->state == State::Cancelled || !job->libraryError.empty() ||
        (job->state == State::NeedsAttention && !findBmsResult.pendingArtifact)) {
      findBmsResult.status = BmsSearchResult::Status::DownloadFailed;
      findBmsResult.presentationMessage = {};
      findBmsResult.message = job->state == State::Cancelled ? i18n::tr("downloads.ui.cancelled") :
          (!job->libraryError.empty() ? job->libraryError : job->message);
    }
    return true;
  } catch (...) {
    queuedDownloadRunning_ = queuedDownloadRetry_ = false;
    findBmsResult.status = BmsSearchResult::Status::DownloadFailed;
    findBmsResult.pendingArtifact.reset();
    findBmsResult.presentationMessage = {};
    findBmsResult.message = i18n::tr("downloads.ui.storage_error");
    return true;
  }
}

void FindBmsModal::openResultUrl(const std::string &url) {
  std::string errorMessage;
  if (!platform_open::openExternalUrl(url, errorMessage)) {
    SDL_Log("Failed to open URL %s: %s", url.c_str(), errorMessage.c_str());
  }
}
