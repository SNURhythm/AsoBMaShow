#include "MainMenuPlayOptionsModal.h"

#include "../rendering/common.h"
#include "../view/BlockingOverlayView.h"
#include "../view/Button.h"
#include "../view/OverlayPortal.h"
#include "../view/ScrollView.h"
#include "../view/TextView.h"
#include "../view/UiTheme.h"

#include <algorithm>
#include <utility>

namespace {
constexpr const char *kFontPath = "assets/fonts/notosanscjkjp.ttf";

Color modalPanelBorder() {
  return ui_theme::activeMode() == ui_theme::ThemeMode::Light
             ? ui_theme::hairlineStrong()
             : Color(86, 118, 153, 210);
}

Button *makeCloseButton() {
  auto *button = new Button(0, 0, 160, 58);
  auto *text = new TextView(kFontPath, 20);
  text->setText("Close");
  text->setAlign(TextView::CENTER);
  text->setVAlign(TextView::MIDDLE);
  text->setThemedColor(
      [] { return ui_theme::textOn(ui_theme::control()); });
  button->setContentView(text);
  button->setStyledBorderWidth(1);
  button->setCornerRadius(ui_theme::controlRadius());
  button->setThemedBackgroundColors(ui_theme::control, ui_theme::controlHover,
                                    ui_theme::controlPressed);
  button->setThemedBorderColors(
      [] { return ui_theme::withAlpha(ui_theme::hairlineStrong(), 150); },
      [] { return ui_theme::withAlpha(ui_theme::hairlineStrong(), 190); },
      [] { return ui_theme::withAlpha(ui_theme::hairlineStrong(), 220); });
  return button;
}
} // namespace

std::unique_ptr<MainMenuPlayOptionsModal>
MainMenuPlayOptionsModal::Create(View *parent,
                                 PlayOptionsPanelCallbacks callbacks,
                                 OverlayPortal *overlayPortal) {
  if (parent == nullptr) return nullptr;

  const float availablePanelWidth =
      std::max(300.0f, static_cast<float>(rendering::window_width) - 48.0f);
  const float panelWidth = std::min(760.0f, availablePanelWidth);
  constexpr float kPanelPadding = 22.0f;
  constexpr float kScrollRightPadding = 18.0f;
  const float contentWidth = panelWidth - kPanelPadding * 2.0f;
  const float optionWidth = std::max(0.0f, contentWidth - kScrollRightPadding);
  const float availablePanelHeight =
      std::max(240.0f, static_cast<float>(rendering::window_height) - 72.0f);
  const float panelHeight = std::min(820.0f, availablePanelHeight);

  auto modal = std::unique_ptr<MainMenuPlayOptionsModal>(
      new MainMenuPlayOptionsModal());
  auto *root = new BlockingOverlayView(0, 0, rendering::window_width,
                                       rendering::window_height);
  root->setPositionType(YGPositionTypeAbsolute);
  root->setPosition(Edge::Left, 0);
  root->setPosition(Edge::Top, 0);
  root->setZIndex(1000);
  root->setVisible(false);
  root->setFlexDirection(FlexDirection::Column);
  root->setAlignItems(YGAlignCenter);
  root->setJustifyContent(YGJustifyCenter);
  root->setThemedBackgroundColor(ui_theme::scrim);

  auto *panel = new View();
  panel->setWidth(panelWidth)
      ->setHeight(panelHeight)
      ->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setGap(12)
      ->setPadding(Edge::All, kPanelPadding)
      ->setThemedBackgroundColor(ui_theme::panelStrong)
      ->setCornerRadius(ui_theme::panelRadius())
      ->setThemedShadow(ui_theme::shadow, ui_theme::kModalShadow)
      ->setThemedBorderColor(modalPanelBorder)
      ->setBorderWidth(1);

  auto *title = new TextView(kFontPath, 30);
  title->setText("Play Options");
  title->setThemedColor(ui_theme::textPrimary);
  title->setHeight(42);
  panel->addView(title);

  auto *scrollView = new ScrollView(0, 0, static_cast<int>(contentWidth), 1);
  scrollView->setWidth(contentWidth);
  scrollView->setFlex(1.0f);
  scrollView->setContentPadding(Edge::Right, kScrollRightPadding);

  auto *optionsContent = new View();
  optionsContent->setFlexDirection(FlexDirection::Column);
  optionsContent->setAlignItems(YGAlignStretch);
  optionsContent->setGap(12);
  optionsContent->setWidth(optionWidth);

  const std::size_t columns = optionWidth >= 620.0f ? 4U : 2U;
  auto *options = new PlayOptionsPanelView(
      std::move(callbacks),
      {.width = optionWidth,
       .playOptionColumns = static_cast<int>(columns),
       .showGauge = true,
       .showLaneOrder = false,
       .showPacemaker = true},
      overlayPortal);
  optionsContent->addView(options);
  scrollView->setContentView(optionsContent);
  panel->addView(scrollView);

  auto *footer = new View();
  footer->setFlexDirection(FlexDirection::Row);
  footer->setJustifyContent(YGJustifyFlexEnd);
  footer->setAlignItems(YGAlignStretch);
  footer->setHeight(58);
  auto *close = makeCloseButton();
  close->setOnClickListener([raw = modal.get()] { raw->hide(); });
  footer->addView(close);
  panel->addView(footer);

  root->addView(panel);
  parent->addView(root);
  modal->root_ = root;
  modal->panel_ = options;
  return modal;
}

void MainMenuPlayOptionsModal::refresh(const PlayOptionsPanelState &state) {
  if (panel_ != nullptr) panel_->refresh(state);
}

void MainMenuPlayOptionsModal::show() {
  if (root_ == nullptr) return;
  resize(rendering::window_width, rendering::window_height);
  root_->setVisible(true);
  root_->applyYogaLayout();
}

void MainMenuPlayOptionsModal::hide() {
  if (root_ == nullptr) return;
  if (panel_ != nullptr) panel_->closeDropdowns();
  root_->setVisible(false);
}

void MainMenuPlayOptionsModal::resize(int width, int height) {
  if (root_ != nullptr) root_->setSize(width, height);
}
