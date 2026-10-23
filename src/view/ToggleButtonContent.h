#pragma once

#include "AppTheme.h"
#include "Button.h"
#include "IconText.h"
#include "TextView.h"

#include <algorithm>
#include <cmath>

namespace toggle_button {
// All toggle forms hug their content and yield only when the parent is narrower.
class CompactButton final : public Button {
public:
  explicit CompactButton(std::function<float()> preferredWidth)
      : preferredWidth_(std::move(preferredWidth)) {
    setAlignSelf(YGAlignFlexStart)->setFlexGrow(0)->setFlexShrink(1)->setMinWidth(0);
    YGNodeStyleSetMaxWidthPercent(getNode(), 100);
  }
  void refreshWidth() {
    setWidth(std::max(96.0f, std::ceil(preferredWidth_())));
  }
  void propagateLanguageChange() override {
    Button::propagateLanguageChange();
    refreshWidth();
  }
private:
  std::function<float()> preferredWidth_;
};

inline void setForeground(View *content, const View::ThemeColorProvider &color) {
  if (!content) return;
  if (auto *text = dynamic_cast<TextView *>(content)) text->setThemedColor(color);
  for (auto *child : content->getChildren()) setForeground(child, color);
}

// Club Beat's active/neutral palette, shared by named and current-value toggles.
inline void style(Button *button, bool active) {
  if (!button) return;
  if (auto *compact = dynamic_cast<CompactButton *>(button)) compact->refreshWidth();
  const bool enabled = button->isEnabled();
  button->setSelected(active);
  button->setCornerRadius(app_theme::controlRadius());
  button->setStyledBorderWidth(1);
  const View::ThemeColorProvider normal = !enabled ? app_theme::panelSubtle
      : active ? app_theme::primaryAction : app_theme::control;
  button->setThemedBackgroundColors(normal,
      !enabled ? normal : active ? app_theme::primaryActionHover : app_theme::controlHover,
      !enabled ? normal : active ? app_theme::primaryActionPressed : app_theme::controlPressed);
  button->setThemedBorderColors(active ? app_theme::accentBorderStrong : app_theme::hairlineSubtle,
      active ? app_theme::accentBorderStrong : app_theme::hairlineStrong, app_theme::accentBorder);
  const View::ThemeColorProvider foreground = !enabled ? app_theme::textMuted
      : active ? View::ThemeColorProvider{[] { return app_theme::textOn(app_theme::primaryAction()); }}
               : View::ThemeColorProvider{app_theme::textPrimary};
  setForeground(button->getContentView(), foreground);
  for (auto *child : button->getChildren()) setForeground(child, foreground);
}
} // namespace toggle_button

namespace cycle_button {
inline constexpr float kContentInset = 12 * 2 + 20 + 8;
inline View *makeContent(TextView *value) {
  auto *content = new View();
  content->setFlexDirection(FlexDirection::Row)->setAlignItems(YGAlignCenter);
  content->setPadding(Edge::Left, 12)->setPadding(Edge::Right, 12)->setGap(8);
  auto *icon = new TextView(ui_icons::kFontAwesomeSolidPath, 16);
  icon->setText(ui_icons::textForCodepoint(ui_icons::kRotate));
  icon->setWidth(20)->setHeight(24)->setFlexShrink(0);
  icon->setVAlign(TextView::MIDDLE);
  icon->setThemedColor(app_theme::textPrimary);
  icon->setName("cycle-icon");
  content->addView(icon);
  value->setFlex(1)->setMinWidth(0);
  YGNodeStyleSetHeightPercent(value->getNode(), 100);
  value->setAlign(TextView::CENTER);
  value->setVAlign(TextView::MIDDLE);
  content->addView(value);
  return content;
}
} // namespace cycle_button

// A named two-state setting: cycle affordance, persistent name, current state.
class ToggleButtonContent final : public View {
public:
  ToggleButtonContent(std::string label, int labelSize, int iconSize) {
    setFlexDirection(FlexDirection::Row)->setAlignItems(YGAlignCenter)->setGap(8);
    setPadding(Edge::Left, 10)->setPadding(Edge::Right, 10);
    icon_ = new TextView(ui_icons::kFontAwesomeSolidPath, iconSize);
    icon_->setText(ui_icons::textForCodepoint(ui_icons::kRotate));
    icon_->setWidth(iconSize + 2)->setHeight(iconSize + 4)->setFlexShrink(0);
    icon_->setVAlign(TextView::MIDDLE);
    addView(icon_);
    label_ = new TextView(app_theme::font, labelSize);
    label_->setText(std::move(label));
    label_->setFlex(1)->setMinWidth(0);
    label_->setVAlign(TextView::MIDDLE);
    addView(label_);
    state_ = new TextView(app_theme::font, labelSize);
    state_->setVAlign(TextView::MIDDLE);
    state_->setFlexShrink(0);
    addView(state_);
    setThemedColor(app_theme::textPrimary);
    setChecked(false);
  }
  void setChecked(bool value) {
    checked_ = value;
    state_->setLocalizedText(i18n::message(value ? "settings.skins.judgement_hud.on.label"
                                               : "settings.skins.judgement_hud.off.label"));
  }
  float preferredWidth() {
    const float stateWidth = std::max(
        state_->measureTextWidth(i18n::tr("settings.skins.judgement_hud.on.label")),
        state_->measureTextWidth(i18n::tr("settings.skins.judgement_hud.off.label")));
    return label_->measureTextWidth(label_->getText()) + stateWidth +
        YGNodeStyleGetWidth(icon_->getNode()).value + 12 * 2 + 8 * 2;
  }
  bool checked() const { return checked_; }
  TextView *labelView() { return label_; }
  TextView *iconView() { return icon_; }
  void setThemedColor(ThemeColorProvider provider) {
    label_->setThemedColor(provider);
    icon_->setThemedColor(provider);
    state_->setThemedColor(std::move(provider));
  }
private:
  TextView *icon_ = nullptr, *label_ = nullptr, *state_ = nullptr;
  bool checked_ = false;
};

namespace toggle_button {
inline CompactButton *makeButton(ToggleButtonContent *content, float height) {
  auto *button = new CompactButton([content] { return content->preferredWidth(); });
  content->setPadding(Edge::Left, 12)->setPadding(Edge::Right, 12);
  button->setHeight(height);
  button->setContentView(content);
  button->refreshWidth();
  return button;
}
} // namespace toggle_button
