#include "TasksModal.h"
#include "../downloads/DownloadService.h"
#include "../PlatformOpen.h"
#include "../view/BlockingOverlayView.h"
#include "../view/ScrollView.h"
#include "../view/IconButtonContent.h"
#include "../view/ModalViewHelpers.h"
#include "../rendering/common.h"
#include "../rendering/UiSafeArea.h"
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
TasksModal::TasksModal(View *parent, std::function<void()> refreshLibrary,
    std::function<bool(const std::filesystem::path &)> locate)
    : locate_(std::move(locate)) {
  root_ = new BlockingOverlayView(0, 0, rendering::window_width, rendering::window_height);
  root_->setName("tasksModal"); root_->setPositionType(YGPositionTypeAbsolute);
  root_->setPosition(Edge::Left, 0)->setPosition(Edge::Top, 0)->setZIndex(1200);
  root_->setVisible(false);
  root_->setAlignItems(YGAlignCenter)->setJustifyContent(YGJustifyCenter);
  root_->setThemedBackgroundColor(app_theme::scrim);
  panel_ = new View(); panel_->setFlexDirection(FlexDirection::Column)->setGap(14)->setPadding(Edge::All, 20);
  panel_->setThemedBackgroundColor(app_theme::panelStrong)->setCornerRadius(app_theme::panelRadius());
  panel_->setName("mainMenuTasksPanel");
  label(panel_, i18n::message("menu.tasks.label"), 28);
  auto *tabs = new View();
  tabs->setFlexDirection(FlexDirection::Row)->setGap(8)->setHeight(48)->setFlexShrink(0);
  const auto tabButton = [&](const char *key, Tab tab, TextView **text, TextView **badge) {
    auto *button = new Button();
    button->setName(tab == Tab::Library ? "tasksLibraryTab" : "tasksDownloadsTab");
    button->setWidth(0)->setFlex(1)->setMinWidth(0)->setHeight(48);
    button->setStyledBorderWidth(1)->setCornerRadius(app_theme::controlRadius());
    auto *content = new View();
    content->setFlexDirection(FlexDirection::Row)->setAlignItems(YGAlignCenter)->setPadding(Edge::Left, 30)->setPadding(Edge::Right, 30);
    *text = new TextView(app_theme::font, 18);
    (*text)->setLocalizedText(i18n::message(key));
    (*text)->setAlign(TextView::CENTER); (*text)->setVAlign(TextView::MIDDLE);
    (*text)->setAutoFitText(true); (*text)->setFlex(1)->setMinWidth(0)->setHeight(46);
    content->addView(*text);
    *badge = icon_button::makeBadge();
    content->addView(*badge);
    button->setContentView(content);
    button->setOnClickListener([this, tab] { selectTab(tab); });
    tabs->addView(button);
    return button;
  };
  libraryTab_ = tabButton("menu.tasks.library.label", Tab::Library, &libraryTabText_, &libraryBadge_);
#if TARGET_OS_IPHONE || TARGET_OS_ANDROID
  downloadsTab_ = tabButton("downloads.ui.title", Tab::Downloads, &downloadsTabText_, &downloadsBadge_);
#endif
  panel_->addView(tabs);
  libraryScroll_ = new ScrollView(); libraryScroll_->setName("tasksLibraryContent");
  libraryScroll_->setFlex(1)->setMinHeight(0)->setWidthPercent(100);
  libraryScroll_->setThemedBackgroundColor(app_theme::insetSurface)->setCornerRadius(app_theme::controlRadius());
  libraryScroll_->setContentPadding(Edge::All, 12);
  auto *libraryContent = new View();
  libraryContent->setFlexDirection(FlexDirection::Column)->setWidthPercent(100);
  libraryText_ = label(libraryContent, "");
  libraryText_->setThemedColor(app_theme::textSecondary);
  libraryScroll_->setContentView(libraryContent);
  panel_->addView(libraryScroll_);
  downloadsContent_ = new View(); downloadsContent_->setName("tasksDownloadsContent");
  downloadsContent_->setFlexDirection(FlexDirection::Column)->setGap(12)->setFlex(1)->setMinHeight(0)->setWidthPercent(100);
#if TARGET_OS_IPHONE
  label(downloadsContent_, i18n::message("downloads.ui.ios_notice"), 16)->setThemedColor(app_theme::textSecondary);
#else
  label(downloadsContent_, i18n::message("downloads.ui.background_notice"), 16)->setThemedColor(app_theme::textSecondary);
#endif
  error_ = label(downloadsContent_, "", 16);
  scroll_ = new ScrollView(); scroll_->setFlex(1)->setMinHeight(0)->setWidthPercent(100);
  downloadsContent_->addView(scroll_);
  panel_->addView(downloadsContent_);
  auto *footer = new View(); footer->setFlexDirection(FlexDirection::Row)->setJustifyContent(YGJustifyFlexEnd);
  footer->setGap(12)->setHeight(48)->setFlexShrink(0);
  refreshLibrary_ = action(footer, i18n::message("menu.refresh_list.label"), [this, refreshLibrary = std::move(refreshLibrary)] {
    if (refreshLibrary) refreshLibrary();
    hide();
  });
  auto *close = action(footer, i18n::message("menu.tasks.close.label"), [this] { hide(); });
  for (auto *button : {refreshLibrary_, close}) button->setWidth(0)->setFlex(1)->setMinWidth(0);
  panel_->addView(footer); root_->addView(panel_); parent->addView(root_);
  selectTab(Tab::Library);
  resize(rendering::window_width, rendering::window_height);
}
void TasksModal::show(Tab tab) {
  root_->setVisible(true); locateFailed_ = false; snapshot_.clear(); nextRefresh_ = 0;
  selectTab(tab); update(); root_->applyYogaLayout();
}
void TasksModal::setLibraryCount(int count) {
  if (libraryCount_ == count) return;
  libraryCount_ = std::max(0, count); refreshBadges();
}
void TasksModal::refreshBadges() {
  icon_button::setBadgeCount(libraryBadge_, libraryCount_);
  icon_button::setBadgeCount(downloadsBadge_, downloadCount_);
}
void TasksModal::selectTab(Tab tab) {
  if (!downloadsTab_) tab = Tab::Library;
  selectedTab_ = tab;
  const bool library = tab == Tab::Library;
  libraryScroll_->setVisible(library); libraryScroll_->setDisplay(library ? YGDisplayFlex : YGDisplayNone);
  downloadsContent_->setVisible(!library); downloadsContent_->setDisplay(library ? YGDisplayNone : YGDisplayFlex);
  refreshLibrary_->setVisible(library); refreshLibrary_->setDisplay(library ? YGDisplayFlex : YGDisplayNone);
  const auto style = [](Button *button, TextView *text, bool selected) {
    if (!button) return;
    button->setSelected(selected);
    modal_view::styleThemedActionButton(button, text, true,
        selected ? app_theme::infoAction : app_theme::control,
        selected ? app_theme::infoActionHover : app_theme::controlHover,
        selected ? app_theme::infoActionPressed : app_theme::controlPressed,
        selected ? app_theme::accentBorder : app_theme::hairlineStrong);
  };
  style(libraryTab_, libraryTabText_, library);
  style(downloadsTab_, downloadsTabText_, !library);
  nextRefresh_ = 0;
  update();
  libraryScroll_->refreshContentLayout(); scroll_->refreshContentLayout();
}
bool TasksModal::isVisible() const { return root_->getVisible(); }
void TasksModal::hide() { root_->setVisible(false); }
void TasksModal::resize(int width, int height) {
  const auto safe = rendering::uiSafeAreaInsets();
  const std::array layout{width, height, safe.top, safe.right, safe.bottom, safe.left};
  if (layout == lastLayout_) return;
  lastLayout_ = layout;
  root_->setWidth(width)->setHeight(height);
  root_->setPadding(Edge::Top, safe.top)->setPadding(Edge::Bottom, safe.bottom);
  root_->setPadding(Edge::Left, safe.left)->setPadding(Edge::Right, safe.right);
  panel_->setWidth(std::min(760, std::max(0, width - safe.left - safe.right - 32)))
      ->setHeight(std::min(660, std::max(0, height - safe.top - safe.bottom - 48)));
  libraryScroll_->refreshContentLayout();
  scroll_->refreshContentLayout();
}
bool TasksModal::handleEvents(SDL_Event &event) {
  if (!root_->getVisible()) return true;
  if (event.type == SDL_EVENT_KEY_DOWN && (event.key.key == SDLK_ESCAPE || event.key.key == SDLK_AC_BACK)) { hide(); return false; }
  root_->handleEvents(event); return false;
}
void TasksModal::update() {
  if (SDL_GetTicks() < nextRefresh_) return;
  nextRefresh_ = SDL_GetTicks() + 300;
#if TARGET_OS_IPHONE || TARGET_OS_ANDROID
  try {
    if (!owner_) owner_ = downloads::service();
    const auto jobs = owner_->jobs();
    downloadCount_ = static_cast<int>(std::count_if(jobs.begin(), jobs.end(), downloads::needsNotification));
    refreshBadges();
    if (!root_->getVisible() || selectedTab_ != Tab::Downloads) return;
    if (locateFailed_) error_->setLocalizedText(i18n::message("downloads.ui.locate_failed"));
    else error_->setText(owner_->error());
    refresh();
  } catch (...) { error_->setLocalizedText(i18n::message("downloads.ui.storage_error")); }
#endif
}
void TasksModal::refresh() {
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
    if (job.persistenceBlocked) state = i18n::tr("downloads.ui.attention");
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
    if (job.persistenceBlocked || job.state == downloads::State::Failed || job.state == downloads::State::NeedsAttention || !job.libraryError.empty()) {
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
    if (const auto path = downloads::importedLibraryPath(job); path && locate_) {
      action(buttons, i18n::message("downloads.ui.locate"), [this, path = *path] {
        std::error_code error;
        try {
          if (std::filesystem::exists(path, error)) {
            hide();
            if (locate_(path)) return;
          }
        } catch (const std::exception &) {}
        root_->setVisible(true);
        locateFailed_ = true;
        error_->setLocalizedText(i18n::message("downloads.ui.locate_failed"));
      });
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
