#pragma once

#include "AppTheme.h"
#include "IconText.h"
#include "TextView.h"

namespace cycle_button {
inline View *makeContent(TextView *value) {
  auto *content = new View();
  content->setFlexDirection(FlexDirection::Row)->setAlignItems(YGAlignCenter);
  content->setPadding(Edge::Left, 12)->setPadding(Edge::Right, 12)->setGap(8);
  auto *icon = new TextView(ui_icons::kFontAwesomeSolidPath, 16);
  icon->setText(ui_icons::textForCodepoint(ui_icons::kRotate));
  icon->setWidth(20)->setHeight(24)->setFlexShrink(0);
  icon->setVAlign(TextView::MIDDLE);
  icon->setThemedColor(app_theme::textSecondary);
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
