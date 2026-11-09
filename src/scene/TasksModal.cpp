#include "TasksModal.h"
#include "../view/BlockingOverlayView.h"
#include "../view/ScrollView.h"
#include "../view/IconButtonContent.h"
#include "../view/ModalViewHelpers.h"
#include "../rendering/common.h"
#include "../rendering/UiSafeArea.h"
#include "../targets.h"
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
}
TasksModal::TasksModal(View *parent, std::function<void()> refreshLibrary) {
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
  auto *footer = new View(); footer->setFlexDirection(FlexDirection::Row)->setJustifyContent(YGJustifyFlexEnd);
  footer->setGap(12)->setHeight(48)->setFlexShrink(0);
  refreshLibrary_ = action(footer, i18n::message("menu.refresh_list.label"), [this, refreshLibrary = std::move(refreshLibrary)] {
    if (refreshLibrary) refreshLibrary();
    hide();
  });
  auto *close = action(footer, i18n::message("menu.tasks.close.label"), [this] { hide(); });
  for (auto *button : {refreshLibrary_, close}) button->setWidth(0)->setFlex(1)->setMinWidth(0);
  panel_->addView(footer); root_->addView(panel_); parent->addView(root_);
  resize(rendering::window_width, rendering::window_height);
}
void TasksModal::show() {
  root_->setVisible(true);
  root_->applyYogaLayout();
  libraryScroll_->refreshContentLayout();
}
void TasksModal::setLibraryCount(int count) {
  libraryCount_ = std::max(0, count);
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
}
bool TasksModal::handleEvents(SDL_Event &event) {
  if (!root_->getVisible()) return true;
  if (event.type == SDL_EVENT_KEY_DOWN && (event.key.key == SDLK_ESCAPE || event.key.key == SDLK_AC_BACK)) { hide(); return false; }
  root_->handleEvents(event); return false;
}
