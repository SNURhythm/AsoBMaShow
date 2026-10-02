#include "FindBmsModal.h"
#include "FindBmsDialogPolicy.h"
#include "FindBmsProgressPresentation.h"
#include "../PlatformOpen.h"
#include "../Utils.h"
#include "../library/ChartLibraryPlatform.h"
#include "../rendering/common.h"
#include "../view/BlockingOverlayView.h"
#include "../view/Button.h"
#include "../view/ModalViewHelpers.h"
#include "../view/RecyclerView.h"
#include "../view/TextView.h"
#include "../view/UiTheme.h"

#include <algorithm>
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

i18n::Text findBmsCandidateLabel(const BmsSearchCandidate &candidate,
                                  size_t index) {
  i18n::Text name;
  if (!candidate.artist.empty() || !candidate.title.empty()) {
    std::string metadata;
    if (!candidate.artist.empty()) {
      metadata = "[" + candidate.artist + "] ";
    }
    metadata += candidate.title.empty() ? candidate.name : candidate.title;
    name = std::move(metadata);
  } else {
    name = candidate.name.empty() ? i18n::message("library.find_bms.horie_archive.label")
                                  : i18n::Text(candidate.name);
  }
  return i18n::message("library.find_bms.candidate.download",
      {{"number", std::to_string(index + 1)}, {"name", name}});
}

class FindBmsCandidateItemView : public View {
public:
  FindBmsCandidateItemView() : View() {
    setFlexDirection(FlexDirection::Column);
    setJustifyContent(YGJustifyCenter);
    setPadding(Edge::Left, 14);
    setPadding(Edge::Right, 14);
    setCornerRadius(ui_theme::controlRadius());
    setBorderWidth(1);

    label = new TextView("assets/fonts/notosanscjkjp.ttf", 16);
    label->setWrap(true);
    label->setOverflow(TextView::TextOverflow::Hidden);
    label->setVAlign(TextView::MIDDLE);
    label->setFlex(1);
    addView(label);
    onUnselected();
  }

  void setCandidate(const BmsSearchCandidate &candidate, size_t index,
                    bool selected) {
    if (label != nullptr) {
      label->setLocalizedText(findBmsCandidateLabel(candidate, index));
    }
    if (selected) {
      onSelected();
    } else {
      onUnselected();
    }
  }

  void onSelected() override {
    setThemedBackgroundColor(ui_theme::infoActionHover);
    setThemedBorderColor(
        [] { return ui_theme::withAlpha(ui_theme::infoActionPressed(), 210); });
    if (label != nullptr) {
      label->setThemedColor(
          [] { return ui_theme::textOn(ui_theme::infoActionHover()); });
    }
  }

  void onUnselected() override {
    setThemedBackgroundColor(ui_theme::control);
    setThemedBorderColor(ui_theme::hairlineStrong);
    if (label != nullptr) {
      label->setThemedColor(ui_theme::textPrimary);
    }
  }

private:
  TextView *label = nullptr;
};

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
  if (message == "Searching Horie archive") {
    return std::max(previous, 0.08);
  }
  if (message == "Preparing Horie archive download") {
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
    : callbacks_(std::move(callbacks)) {}

FindBmsModal::~FindBmsModal() {
  cancelAndWait();
  for (auto *button : {findBmsCloseButton, findBmsConfirmButton, findBmsRetryButton,
                       findBmsKeepFilesButton,
                       findBmsDeleteFilesButton, findBmsOpenButton,
                       findBmsGoogleButton, findBmsRefreshButton}) {
    if (button != nullptr) button->setOnClickListener(nullptr);
  }
  if (findBmsCandidateRecyclerView != nullptr) {
    findBmsCandidateRecyclerView->onSelected = nullptr;
  }
}

bool FindBmsModal::inProgress() const { return findBmsTask.running(); }

bool FindBmsModal::isVisible() const {
  return findBmsModalRoot != nullptr && findBmsModalRoot->getVisible();
}

View *FindBmsModal::root() const { return findBmsModalRoot; }

void FindBmsModal::resize(int width, int height) {
  if (findBmsModalRoot != nullptr) findBmsModalRoot->setSize(width, height);
}

void FindBmsModal::cancelAndWait() {
  findBmsAwaitingConfirmation = false;
  findBmsTask.stopAndWait();
  if (findBmsModalRoot != nullptr) findBmsModalRoot->setVisible(false);
}

bool FindBmsModal::handleEvents(SDL_Event &event) {
  if (!isVisible()) return true;
  if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) {
    if (event.key.repeat == 0) cancelOrClose();
    return false;
  }
  (void)findBmsModalRoot->handleEvents(event);
  return false;
}

void FindBmsModal::cancelOrClose() {
  const bool wasRunning = findBmsTask.running();
  update();
  const bool running = wasRunning || findBmsTask.running();
  if (!findBmsDialogPolicy(running, findBmsResult).showCloseOrCancel) {
    return;
  }
  if (running) {
    findBmsTask.requestCancel();
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
  findBmsModalRoot->setThemedBackgroundColor(ui_theme::scrim);

  auto *panel = new View();
  panel->setWidth(kModalPanelWidth)
      ->setHeight(560)
      ->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setGap(14)
      ->setPadding(Edge::All, kModalPanelPadding)
      ->setThemedBackgroundColor(ui_theme::panelStrong)
      ->setCornerRadius(ui_theme::panelRadius())
      ->setThemedShadow(ui_theme::shadow, ui_theme::kModalShadow)
      ->setThemedBorderColor(modalPanelBorder)
      ->setBorderWidth(1);

  findBmsModalTitleText = new TextView("assets/fonts/notosanscjkjp.ttf", 30);
  findBmsModalTitleText->setLocalizedText(i18n::message("library.find_bms.find_bms.label"));
  findBmsModalTitleText->setThemedColor(ui_theme::textPrimary);
  findBmsModalTitleText->setHeight(42);
  panel->addView(findBmsModalTitleText);

  findBmsStatusText = new TextView("assets/fonts/notosanscjkjp.ttf", 22);
  findBmsStatusText->setLocalizedText(i18n::message("library.find_bms.preparing_lookup.label"));
  findBmsStatusText->setThemedColor(ui_theme::textPrimary);
  findBmsStatusText->setWrap(true);
  findBmsStatusText->setOverflow(TextView::TextOverflow::Hidden);
  findBmsStatusText->setHeight(58);
  panel->addView(findBmsStatusText);

  findBmsDetailText = new TextView("assets/fonts/notosanscjkjp.ttf", 18);
  findBmsDetailText->setText("");
  findBmsDetailText->setThemedColor(ui_theme::textSecondary);
  findBmsDetailText->setWrap(true);
  findBmsDetailText->setOverflow(TextView::TextOverflow::Hidden);
  findBmsDetailText->setFlex(1);
  panel->addView(findBmsDetailText);

  findBmsCandidateRecyclerView = new RecyclerView<BmsSearchCandidate>(
      [](const BmsSearchCandidate &a, const BmsSearchCandidate &b) {
        return a.source == b.source && a.id == b.id && a.name == b.name;
      });
  findBmsCandidateRecyclerView->itemHeight = 52;
  findBmsCandidateRecyclerView->reserveScrollbarGutter = true;
  findBmsCandidateRecyclerView->setWidth(kModalContentWidth);
  findBmsCandidateRecyclerView->setHeight(0);
  findBmsCandidateRecyclerView->setThemedBackgroundColor(
      ui_theme::insetSurface);
  findBmsCandidateRecyclerView->setCornerRadius(ui_theme::controlRadius());
  findBmsCandidateRecyclerView->setThemedBorderColor(ui_theme::hairline);
  findBmsCandidateRecyclerView->setBorderWidth(1);
  findBmsCandidateRecyclerView->setVisible(false);
  findBmsCandidateRecyclerView->onCreateView = [](const BmsSearchCandidate &) {
    return new FindBmsCandidateItemView();
  };
  findBmsCandidateRecyclerView->onBind = [](View *view,
                                            const BmsSearchCandidate &candidate,
                                            int idx, bool isSelected) {
    auto *itemView = dynamic_cast<FindBmsCandidateItemView *>(view);
    if (itemView != nullptr) {
      itemView->setCandidate(candidate, static_cast<size_t>(idx), isSelected);
    }
  };
  findBmsCandidateRecyclerView->onSelected = [this](const BmsSearchCandidate &,
                                                    int idx) {
    startCandidateDownload(static_cast<size_t>(idx));
  };
  panel->addView(findBmsCandidateRecyclerView);

  findBmsProgressTrack = new View();
  findBmsProgressTrack->setWidth(kModalContentWidth)
      ->setHeight(24)
      ->setThemedBackgroundColor(ui_theme::progressTrack)
      ->setCornerRadius(ui_theme::controlRadius())
      ->setThemedBorderColor(ui_theme::hairline)
      ->setBorderWidth(1);
  findBmsProgressFill = new View();
  findBmsProgressFill->setWidth(0)->setHeight(20)->setBackgroundColor(
      ui_theme::progressFill());
  findBmsProgressTrack->addView(findBmsProgressFill);
  panel->addView(findBmsProgressTrack);

  auto *footer = new View();
  footer->setFlexDirection(FlexDirection::Row);
  footer->setJustifyContent(YGJustifyFlexEnd);
  footer->setAlignItems(YGAlignStretch);
  footer->setGap(12);
  footer->setHeight(58);

  findBmsCloseButton = makeModalButton(i18n::message("library.find_bms.cancel.label"), 20, &findBmsCloseButtonText);
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
  findBmsConfirmButton->setWidth(240);
  findBmsRetryButton->setWidth(150);
  findBmsKeepFilesButton->setWidth(150);
  findBmsDeleteFilesButton->setWidth(150);
  findBmsOpenButton->setWidth(180);
  findBmsGoogleButton->setWidth(150);
  findBmsRefreshButton->setWidth(150);
  findBmsCloseButton->setOnClickListener([this]() { cancelOrClose(); });
  findBmsConfirmButton->setOnClickListener([this]() { startLookup(); });
  findBmsRetryButton->setOnClickListener([this]() {
    findBmsTask.retryDownload();
    refresh(false);
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
    if (callbacks_.refreshLibrary) callbacks_.refreshLibrary();
    hide();
  });

  footer->addView(findBmsCloseButton);
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
  if (findBmsModalRoot == nullptr) return;
  if (findBmsTask.running() || findBmsResult.pendingArtifact) {
    // A retained scene may hide the dialog while a keep/delete decision is
    // pending. Reopen that decision without replacing its staged download.
    findBmsModalRoot->setVisible(true);
    refresh();
    return;
  }

  findBmsModalChart = record;
  findBmsResult = {};
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
  if (!findBmsAwaitingConfirmation || findBmsTask.running() ||
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

void FindBmsModal::startCandidateDownload(size_t candidateIndex) {
  if (findBmsTask.running() ||
      candidateIndex >= findBmsResult.candidates.size()) {
    return;
  }
  const BmsSearchCandidate candidate = findBmsResult.candidates[candidateIndex];
  const ChartMetaRecord record = findBmsModalChart;
  const std::filesystem::path downloadRoot =
      callbacks_.downloadRoot ? callbacks_.downloadRoot() : std::filesystem::path{};
  BmsSearchDownloadOptions downloadOptions =
      callbacks_.downloadOptions ? callbacks_.downloadOptions()
                                 : BmsSearchDownloadOptions{};
  downloadOptions.requestRetry = findBmsTask.retryCallback();
  findBmsResult = {};
  findBmsResult.candidates = {candidate};
  findBmsPendingDecision.reset();
  findBmsProgressMessage = "Preparing Horie archive download";
  findBmsProgressCurrent = 0;
  findBmsProgressTotal = 0;
  findBmsProgressFraction = 0.09;
  findBmsProgressLog.clear();
  findBmsProgressLog.push_back(i18n::tr("library.find_bms.preparing_horie_archive_download.label"));
  if (callbacks_.downloadStarted) callbacks_.downloadStarted();
  findBmsTask.start([candidate, record, downloadRoot, downloadOptions](
                        std::atomic_bool &cancelled,
                        BmsSearchDownloadProgressCallback progress) {
    BmsSearchService service;
    return service.downloadCandidate(
        candidate, record.meta.SHA256, record.meta.MD5, downloadRoot,
        cancelled, std::move(progress), downloadOptions);
  });
  refresh();
}

void FindBmsModal::startPendingArtifactResolution(
    BmsSearchPendingArtifactDecision decision) {
  if (findBmsTask.running() || !findBmsResult.pendingArtifact) {
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
  const bool wasRunning = findBmsTask.running();
  update();
  const bool running = wasRunning || findBmsTask.running();
  if (findBmsModalRoot == nullptr ||
      !findBmsDialogPolicy(running, findBmsResult).canDismiss) {
    return;
  }
  findBmsAwaitingConfirmation = false;
  findBmsModalRoot->setVisible(false);
}

void FindBmsModal::refresh(bool refreshCandidates) {
  if (findBmsModalRoot == nullptr) {
    return;
  }

  const bool running = findBmsTask.running();
  const auto retryRequest = findBmsTask.retryRequest();
  const auto policy =
      findBmsDialogPolicy(findBmsTask.running(), findBmsResult);
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
      statusText = findBmsProgressDisplayMessage(findBmsProgressMessage,
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
    case BmsSearchResult::Status::AmbiguousCandidates:
      statusText = i18n::message("library.find_bms.choose_match.label");
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
        ui_theme::sdl(failed ? ui_theme::coral() : ui_theme::textPrimary()));
  }

  const bool showCandidateList =
      !findBmsAwaitingConfirmation && !running &&
      policy.showNormalResultActions &&
      findBmsResult.status == BmsSearchResult::Status::AmbiguousCandidates &&
      !findBmsResult.candidates.empty();

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
  } else if (!running && findBmsResult.status ==
                             BmsSearchResult::Status::AmbiguousCandidates) {
    detail = i18n::message("library.find_bms.choose_archive_below.message");
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
  if (!findBmsModalChart.meta.Title.empty()) {
    detail = i18n::message("library.find_bms.chart_detail",
        {{"title", findBmsModalChart.meta.Title}, {"detail", detail}});
  }
  if (findBmsDetailText != nullptr) {
    findBmsDetailText->setLocalizedText(detail);
  }

  if (refreshCandidates && findBmsCandidateRecyclerView != nullptr) {
    findBmsCandidateRecyclerView->setVisible(showCandidateList);
    const int visibleRows =
        showCandidateList
            ? std::min<int>(static_cast<int>(findBmsResult.candidates.size()),
                            3)
            : 0;
    findBmsCandidateRecyclerView->setHeight(static_cast<float>(
        visibleRows * findBmsCandidateRecyclerView->itemHeight));
    if (showCandidateList) {
      findBmsCandidateRecyclerView->setItems(findBmsResult.candidates);
    } else {
      findBmsCandidateRecyclerView->clear();
    }
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
  const bool hasRefreshAction =
      !findBmsAwaitingConfirmation && policy.showNormalResultActions &&
      !running && !downloaded;
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
    findBmsRetryButton->setVisible(retryRequest.has_value());
    findBmsRetryButton->setWidth(retryRequest ? 150.0f : 0.0f);
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
  for (auto *button : {findBmsCloseButton, findBmsConfirmButton, findBmsRetryButton,
                       findBmsKeepFilesButton, findBmsDeleteFilesButton,
                       findBmsOpenButton, findBmsGoogleButton, findBmsRefreshButton}) {
    if (button != nullptr) {
      button->setDisplay(button->getVisible() ? YGDisplayFlex : YGDisplayNone);
    }
  }

  styleThemedActionButton(findBmsCloseButton, findBmsCloseButtonText, true,
                          ui_theme::control, ui_theme::controlHover,
                          ui_theme::controlPressed, ui_theme::hairlineStrong);
  styleThemedActionButton(findBmsConfirmButton, findBmsConfirmButtonText,
                          findBmsAwaitingConfirmation, ui_theme::successAction,
                          ui_theme::successActionHover,
                          ui_theme::successActionPressed, ui_theme::accentBorder);
  styleThemedActionButton(findBmsRetryButton, findBmsRetryButtonText,
                          retryRequest.has_value(), ui_theme::successAction,
                          ui_theme::successActionHover,
                          ui_theme::successActionPressed, ui_theme::accentBorder);
  styleThemedActionButton(
      findBmsKeepFilesButton, findBmsKeepFilesButtonText,
      policy.showPendingActions, ui_theme::successAction,
      ui_theme::successActionHover, ui_theme::successActionPressed,
      ui_theme::accentBorder);
  styleThemedActionButton(
      findBmsDeleteFilesButton, findBmsDeleteFilesButtonText,
      policy.showPendingActions, ui_theme::dangerAction,
      ui_theme::dangerActionHover, ui_theme::dangerActionPressed,
      ui_theme::accentBorder);
  styleThemedActionButton(findBmsOpenButton, findBmsOpenButtonText,
                          !running && hasSource, ui_theme::infoAction,
                          ui_theme::infoActionHover,
                          ui_theme::infoActionPressed, ui_theme::accentBorder);
  styleThemedActionButton(
      findBmsGoogleButton, findBmsGoogleButtonText, !running && hasSearchAction,
      ui_theme::violetAction, ui_theme::violetActionHover,
      ui_theme::violetActionPressed, ui_theme::violetActionHover);
  styleThemedActionButton(
      findBmsRefreshButton, findBmsRefreshButtonText, hasRefreshAction,
      ui_theme::successAction, ui_theme::successActionHover,
      ui_theme::successActionPressed, ui_theme::accentBorder);
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
  if (shouldRefresh) {
    refresh();
  }
}

void FindBmsModal::openResultUrl(const std::string &url) {
  std::string errorMessage;
  if (!platform_open::openExternalUrl(url, errorMessage)) {
    SDL_Log("Failed to open URL %s: %s", url.c_str(), errorMessage.c_str());
  }
}
