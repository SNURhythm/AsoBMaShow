#include "../i18n/Localization.h"
#include "MainMenuPlayOptionsModal.h"

#include "../rendering/common.h"
#include "../view/BlockingOverlayView.h"
#include "../view/Button.h"
#include "../view/OverlayPortal.h"
#include "../view/ScrollView.h"
#include "../view/TextView.h"
#include "../view/AppTheme.h"

#include <algorithm>
#include <utility>

namespace {
constexpr const char *kFontPath = app_theme::font;

Color modalPanelBorder() {
  return app_theme::hairlineStrong();
}

Button *makeCloseButton() {
  auto *button = new Button(0, 0, 132, 48);
  auto *text = new TextView(kFontPath, 20);
  text->setLocalizedText(i18n::message("menu.play_options.close.label"));
  text->setAlign(TextView::CENTER);
  text->setVAlign(TextView::MIDDLE);
  text->setThemedColor(
      [] { return app_theme::textOn(app_theme::control()); });
  button->setContentView(text);
  button->setStyledBorderWidth(1);
  button->setCornerRadius(app_theme::controlRadius());
  button->setThemedBackgroundColors(app_theme::control, app_theme::controlHover,
                                    app_theme::controlPressed);
  button->setThemedBorderColors(
      [] { return app_theme::withAlpha(app_theme::hairlineStrong(), 150); },
      [] { return app_theme::withAlpha(app_theme::hairlineStrong(), 190); },
      [] { return app_theme::withAlpha(app_theme::hairlineStrong(), 220); });
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
  constexpr float kPanelPadding = 18.0f;
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
  root->setThemedBackgroundColor(app_theme::scrim);

  auto *panel = new View();
  panel->setWidth(panelWidth)
      ->setHeight(panelHeight)
      ->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setGap(0)
      ->setPadding(Edge::All, 0)
      ->setThemedBackgroundColor(app_theme::panelStrong)
      ->setCornerRadius(app_theme::panelRadius())
      ->setThemedBorderColor(modalPanelBorder)
      ->setBorderWidth(1);

  const auto addRule = [](View *region, Edge edge) {
    auto *rule = new View();
    rule->setPositionType(YGPositionTypeAbsolute);
    rule->setPosition(Edge::Left, 0)->setPosition(edge, 0);
    rule->setWidthPercent(100)->setHeight(1);
    rule->setThemedBackgroundColor(app_theme::hairline);
    region->addView(rule);
  };
  auto *header = new View();
  header->setHeight(64)->setFlexShrink(0);
  header->setPadding(Edge::Left, kPanelPadding);
  header->setPadding(Edge::Right, kPanelPadding);
  header->setJustifyContent(YGJustifyCenter);
  auto *title = new TextView(app_theme::boldFont, 26, TextView::FontWeight::Bold);
  title->setLocalizedText(i18n::message("menu.play_options.play_options.label"));
  title->setThemedColor(app_theme::textPrimary);
  title->setHeight(36);
  header->addView(title);
  addRule(header, Edge::Bottom);
  panel->addView(header);

  auto *scrollView = new ScrollView(0, 0, static_cast<int>(contentWidth), 1);
  scrollView->setWidth(contentWidth);
  scrollView->setFlex(1.0f);
  scrollView->setMinHeight(0);
  scrollView->setMargin(Edge::Left, kPanelPadding);
  scrollView->setMargin(Edge::Right, kPanelPadding);
  scrollView->setContentPadding(Edge::Top, 12);
  scrollView->setContentPadding(Edge::Bottom, 12);
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
  footer->setAlignItems(YGAlignCenter);
  footer->setHeight(72)->setFlexShrink(0);
  footer->setPadding(Edge::Left, kPanelPadding);
  footer->setPadding(Edge::Right, kPanelPadding);
  addRule(footer, Edge::Top);
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
