#pragma once

#include "AppTheme.h"
#include "IconText.h"
#include "TextView.h"

namespace selector_button {

inline View *makeContent(const i18n::Text &label, int size, TextView **out = nullptr,
                          uint32_t glyph = 0, bool trailing = false) {
  auto *content = new View();
  content->setFlexDirection(FlexDirection::Row)->setAlignItems(YGAlignCenter);
  content->setJustifyContent(YGJustifyCenter)->setGap(10);
  auto *text = new TextView(main_menu_theme::font, size);
  text->setName("selectorButtonLabel");
  text->setLocalizedText(label);
  text->setHeight(40)->setMinWidth(0)->setFlexShrink(1);
  text->setVAlign(TextView::MIDDLE);
  auto *icon = new TextView(ui_icons::kFontAwesomeSolidPath, size - 3);
  icon->setName("selectorButtonIcon");
  icon->setText(glyph ? ui_icons::textForCodepoint(glyph) : "");
  icon->setWidth(glyph ? size : 0)->setHeight(40)->setFlexShrink(0);
  icon->setVAlign(TextView::MIDDLE);
  icon->setThemedColor(main_menu_theme::ink);
  if (!trailing && glyph) content->addView(icon);
  content->addView(text);
  if (trailing && glyph) content->addView(icon);
  if (!glyph) delete icon;
  if (out) *out = text;
  return content;
}

} // namespace selector_button
