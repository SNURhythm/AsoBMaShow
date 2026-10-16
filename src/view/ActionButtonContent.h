#pragma once

#include "Button.h"
#include "IconText.h"
#include "TextView.h"

namespace action_button {

// Keep a localized caption below the glyph so touch users do not need tooltips.
inline View *makeContent(TextView *caption, uint32_t glyph) {
  auto *content = new View();
  content->setFlexDirection(FlexDirection::Column);
  content->setAlignItems(YGAlignCenter)->setJustifyContent(YGJustifyCenter);
  content->setGap(3)->setPadding(Edge::Left, 8)->setPadding(Edge::Right, 8);
  auto *icon = new TextView(ui_icons::kFontAwesomeSolidPath, 22);
  icon->setName("actionIcon");
  icon->setText(ui_icons::textForCodepoint(glyph));
  icon->setAlign(TextView::CENTER);
  icon->setVAlign(TextView::MIDDLE);
  icon->setWidth(28)->setHeight(26)->setFlexShrink(0);
  caption->setName("actionCaption");
  caption->setAlign(TextView::CENTER);
  caption->setVAlign(TextView::MIDDLE);
  caption->setWidthPercent(100)->setHeight(26)->setFlexShrink(0);
  caption->setAutoFitText(true);
  content->addView(icon);
  content->addView(caption);
  return content;
}

inline void setIconColor(Button *button, View::ThemeColorProvider color) {
  if (auto *content = button->getContentView()) {
    if (auto *icon = dynamic_cast<TextView *>(content->findViewByName("actionIcon")))
      icon->setThemedColor(std::move(color));
  }
}

} // namespace action_button
