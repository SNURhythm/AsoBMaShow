#pragma once

#include "AppTheme.h"
#include "IconText.h"
#include "TextView.h"

#include <algorithm>

namespace icon_button {

inline void setBadgeCount(TextView *badge, int count) {
  if (!badge) return;
  count = std::max(0, count);
  const std::string text = count > 99 ? "99+" : std::to_string(count);
  View::LayoutBatchScope batch;
  badge->setText(text);
  badge->setWidth(count > 99 ? 30 : count > 9 ? 24 : 20);
  badge->setVisible(count > 0);
}

// Overlay the badge inside the hit target so updates never resize the toolbar.
inline View *makeContent(uint32_t glyph, TextView **badgeOut = nullptr) {
  auto *content = new View();
  content->setAlignItems(YGAlignCenter)->setJustifyContent(YGJustifyCenter);
  auto *icon = new TextView(ui_icons::kFontAwesomeSolidPath, 22);
  icon->setName("selectorButtonIcon");
  icon->setText(ui_icons::textForCodepoint(glyph));
  icon->setAlign(TextView::CENTER);
  icon->setVAlign(TextView::MIDDLE);
  icon->setWidth(28)->setHeight(28)->setFlexShrink(0);
  icon->setThemedColor(app_theme::textPrimary);
  content->addView(icon);
  if (badgeOut) {
    auto *badge = new TextView(app_theme::boldFont, 12, TextView::FontWeight::Bold);
    badge->setName("iconButtonBadge");
    badge->setPositionType(YGPositionTypeAbsolute);
    badge->setPosition(Edge::Top, 2)->setPosition(Edge::Right, 2);
    badge->setHeight(20)->setCornerRadius(10);
    badge->setBackgroundColor(Color(196, 36, 55, 255));
    badge->setColor(SDL_Color{255, 255, 255, 255});
    badge->setAlign(TextView::CENTER);
    badge->setVAlign(TextView::MIDDLE);
    badge->setAutoFitText(true);
    setBadgeCount(badge, 0);
    content->addView(badge);
    *badgeOut = badge;
  }
  return content;
}

} // namespace icon_button
