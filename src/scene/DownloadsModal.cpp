#include "DownloadsModal.h"
#include "../downloads/DownloadService.h"
#include "../PlatformOpen.h"
#include "../view/BlockingOverlayView.h"
#include "../view/ScrollView.h"
#include "../view/ModalViewHelpers.h"
#include "../rendering/common.h"
#include "../targets.h"
#include "FindBmsProgressPresentation.h"
#include <algorithm>
#include <sstream>

namespace {
TextView *label(View *parent, const i18n::Text &text, int size = 18) {
  auto *view = new TextView(app_theme::font, size);
  view->setLocalizedText(text); view->setThemedColor(app_theme::textPrimary);
  view->setWrap(true); view->setOverflow(TextView::TextOverflow::Hidden);
  view->setWidthPercent(100)->setMinWidth(0)->setFlexShrink(0);
  parent->addView(view); return view;
}
Button *action(View *parent, const i18n::Text &title, std::function<void()> callback) {
  TextView *text = nullptr;
  auto *button = modal_view::makeModalButton(title, 16, &text);
  button->setWidth(145)->setHeight(48)->setFlexShrink(0);
  modal_view::styleThemedActionButton(button, text, true,
      app_theme::control, app_theme::controlHover, app_theme::controlPressed, app_theme::hairlineStrong);
  button->setOnClickListener(std::move(callback)); parent->addView(button); return button;
}
i18n::Text stateLabel(downloads::State state) {
  using downloads::State;
  switch (state) {
  case State::Queued: return i18n::message("downloads.ui.queued");
  case State::Transferring: return i18n::message("downloads.ui.transferring");
  case State::Downloaded: return i18n::message("downloads.ui.downloaded");
  case State::Importing: return i18n::message("downloads.ui.importing");
  case State::NeedsAttention: return i18n::message("downloads.ui.attention");
  case State::Complete: return i18n::message("downloads.ui.complete");
  case State::Cancelled: return i18n::message("downloads.ui.cancelled");
  case State::Failed: return i18n::message("downloads.ui.failed");
  }
  return {};
}
}
DownloadsModal::DownloadsModal(View *parent) {
  root_ = new BlockingOverlayView(0, 0, rendering::window_width, rendering::window_height);
  root_->setName("downloadsModal"); root_->setPositionType(YGPositionTypeAbsolute);
  root_->setPosition(Edge::Left, 0)->setPosition(Edge::Top, 0)->setZIndex(1200);
  root_->setVisible(false);
  root_->setAlignItems(YGAlignCenter)->setJustifyContent(YGJustifyCenter);
  root_->setThemedBackgroundColor(app_theme::scrim);
  panel_ = new View(); panel_->setFlexDirection(FlexDirection::Column)->setGap(14)->setPadding(Edge::All, 20);
  panel_->setThemedBackgroundColor(app_theme::panelStrong)->setCornerRadius(app_theme::panelRadius());
  label(panel_, i18n::message("downloads.ui.title"), 28);
#if TARGET_OS_IPHONE
  label(panel_, i18n::message("downloads.ui.ios_notice"), 16)->setThemedColor(app_theme::textSecondary);
#else
  label(panel_, i18n::message("downloads.ui.background_notice"), 16)->setThemedColor(app_theme::textSecondary);
#endif
  error_ = label(panel_, "", 16);
  scroll_ = new ScrollView(); scroll_->setFlex(1)->setMinHeight(0)->setWidthPercent(100);
  panel_->addView(scroll_);
  auto *footer = new View(); footer->setFlexDirection(FlexDirection::Row)->setJustifyContent(YGJustifyFlexEnd);
  action(footer, i18n::message("menu.tasks.close.label"), [this] { hide(); });
  panel_->addView(footer); root_->addView(panel_); parent->addView(root_);
  resize(rendering::window_width, rendering::window_height);
}
void DownloadsModal::show() { root_->setVisible(true); snapshot_.clear(); nextRefresh_ = 0; update(); }
bool DownloadsModal::isVisible() const { return root_->getVisible(); }
void DownloadsModal::hide() { root_->setVisible(false); }
void DownloadsModal::resize(int width, int height) {
  root_->setWidth(width)->setHeight(height);
  panel_->setWidth(std::min(820, std::max(240, width - 32)))->setHeight(std::max(180, height - 48));
  scroll_->refreshContentLayout();
}
bool DownloadsModal::handleEvents(SDL_Event &event) {
  if (!root_->getVisible()) return true;
  if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) { hide(); return false; }
  root_->handleEvents(event); return false;
}
void DownloadsModal::update() {
  if (!root_->getVisible() || SDL_GetTicks() < nextRefresh_) return;
  nextRefresh_ = SDL_GetTicks() + 300;
  try {
    if (!owner_) owner_ = downloads::service();
    error_->setText(owner_->error());
    refresh();
  } catch (...) { error_->setLocalizedText(i18n::message("downloads.ui.storage_error")); }
}
void DownloadsModal::refresh() {
  auto jobs = owner_->jobs();
  std::reverse(jobs.begin(), jobs.end());
  std::ostringstream snapshot;
  snapshot << i18n::revision();
  for (const auto &job : jobs) snapshot << job.id << ':' << job.revision << ':' << job.message << ':' << job.downloadedBytes << ':' << job.totalBytes;
  if (snapshot.str() == snapshot_) return;
  snapshot_ = snapshot.str();
  const auto offset = scroll_->getScrollOffset();
  auto *content = new View(); content->setFlexDirection(FlexDirection::Column)->setGap(12)->setWidthPercent(100);
  if (jobs.empty()) label(content, i18n::message("downloads.ui.empty"));
  for (const auto &job : jobs) {
    auto *row = new View(); row->setFlexDirection(FlexDirection::Column)->setGap(8)->setPadding(Edge::All, 14);
    row->setWidthPercent(100)->setMinWidth(0)->setFlexShrink(0);
    row->setThemedBackgroundColor(app_theme::insetSurface)->setCornerRadius(app_theme::controlRadius());
    label(row, job.request.title.empty() ? job.request.archiveName : job.request.title, 20);
    auto state = stateLabel(job.state).resolve();
    if (job.state == downloads::State::Complete && !job.libraryAcknowledged)
      state = i18n::tr(job.libraryError.empty() ? "downloads.ui.indexing" : "downloads.ui.attention");
    if (job.state == downloads::State::Transferring && job.totalBytes)
      state += " · " + std::to_string(static_cast<int>(100.0 * job.downloadedBytes / job.totalBytes)) + "%";
    label(row, state, 16)->setThemedColor(app_theme::textSecondary);
    if (!job.libraryError.empty()) label(row, job.libraryError, 16);
    else if (!job.message.empty()) label(row, findBmsProgressDisplayText(job.message, job.downloadedBytes, job.totalBytes, true), 16);
    auto *buttons = new View(); buttons->setFlexDirection(FlexDirection::Row)->setFlexWrap(YGWrapWrap)->setGap(8);
    const auto id = job.id;
    const auto invoke = [this](auto operation) { try { operation(); snapshot_.clear(); } catch (...) { error_->setLocalizedText(i18n::message("downloads.ui.storage_error")); } };
    if (job.state == downloads::State::Failed || job.state == downloads::State::NeedsAttention || !job.libraryError.empty()) {
      action(buttons, i18n::message("library.find_bms.retry.label"), [this, id, invoke] { invoke([&] { owner_->retry(id); }); });
    }
    if (job.state == downloads::State::NeedsAttention && job.result && job.result->pendingArtifact && !job.publicationPrepared) {
      action(buttons, i18n::message("library.find_bms.keep_files.label"), [this, id, invoke] {
        invoke([&] { owner_->decide(id, BmsSearchPendingArtifactDecision::Keep); });
      });
      action(buttons, i18n::message("library.find_bms.delete_files.label"), [this, id, invoke] {
        invoke([&] { owner_->decide(id, BmsSearchPendingArtifactDecision::Delete); });
      });
    }
    if (!downloads::terminal(job.state) && !job.publicationPrepared) {
      action(buttons, i18n::message("library.find_bms.cancel.label"), [this, id, invoke] { invoke([&] { owner_->cancel(id); }); });
    }
    const std::string source = job.request.displayUrl.empty() ? job.request.url : job.request.displayUrl;
    action(buttons, i18n::message("library.find_bms.source.label"), [source] { std::string error; (void)platform_open::openExternalUrl(source, error); });
    if (downloads::terminal(job.state) && (job.state != downloads::State::Complete || job.libraryAcknowledged)) {
      action(buttons, i18n::message("downloads.ui.remove"), [this, id, invoke] { invoke([&] { owner_->forget(id); }); });
    }
    row->addView(buttons); content->addView(row);
  }
  scroll_->setContentView(content); scroll_->refreshContentLayout(); scroll_->setScrollOffset(offset);
}
