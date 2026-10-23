#pragma once

#include "ToggleButtonContent.h"

namespace tab_button {
// Navigation stays open and unboxed; choices retain their filled button shape.
inline void style(Button *button, bool active, bool vertical = false) {
  if (!button) return;
  button->setSelected(active);
  button->setCornerRadius(0);
  button->setStyledBorderWidth(0);
  button->setThemedBackgroundColors([] { return Color(0, 0, 0, 0); },
                                    app_theme::controlHover, app_theme::controlPressed);
  const View::ThemeColorProvider foreground = active ? app_theme::textPrimary : app_theme::textSecondary;
  toggle_button::setForeground(button->getContentView(), foreground);
  for (auto *child : button->getChildren()) toggle_button::setForeground(child, foreground);
  auto *indicator = button->findViewByName("tab-active-indicator");
  if (!indicator) {
    indicator = new View();
    indicator->setName("tab-active-indicator");
    indicator->setPositionType(YGPositionTypeAbsolute);
    indicator->setThemedBackgroundColor(app_theme::accentBorderStrong);
    button->addView(indicator);
  }
  if (vertical) {
    indicator->setWidth(3)->setHeight(YGUndefined);
    indicator->setPosition(Edge::Right, YGUndefined);
    indicator->setPosition(Edge::Left, 0)->setPosition(Edge::Top, 8)->setPosition(Edge::Bottom, 8);
  } else {
    indicator->setHeight(3)->setWidth(YGUndefined);
    indicator->setPosition(Edge::Top, YGUndefined);
    indicator->setPosition(Edge::Left, 8)->setPosition(Edge::Right, 8)->setPosition(Edge::Bottom, 0);
  }
  indicator->setVisible(active);
}
} // namespace tab_button
