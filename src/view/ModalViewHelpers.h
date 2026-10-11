#pragma once

#include "Button.h"
#include "TextView.h"
#include "AppTheme.h"

namespace modal_view {

inline void styleThemedActionButton(
    Button *button, TextView *text, bool enabled,
    View::ThemeColorProvider normal, View::ThemeColorProvider hover,
    View::ThemeColorProvider pressed, View::ThemeColorProvider border) {
  if (button == nullptr || text == nullptr) {
    return;
  }

  button->setEnabled(enabled);
  button->setCornerRadius(app_theme::controlRadius());
  if (enabled) {
    button->setThemedBackgroundColors(normal, hover, pressed);
    button->setThemedBorderColors(
        [border] { return app_theme::withAlpha(border(), 150); },
        [border] { return app_theme::withAlpha(border(), 190); },
        [border] { return app_theme::withAlpha(border(), 220); });
    text->setThemedColor([normal] { return app_theme::textOn(normal()); });
  } else {
    button->setThemedBackgroundColors(
        app_theme::panelSubtle, app_theme::panelSubtle, app_theme::panelSubtle);
    button->setThemedBorderColors(app_theme::hairlineSubtle,
                                  app_theme::hairlineSubtle,
                                  app_theme::hairlineSubtle);
    text->setThemedColor(app_theme::textMuted);
  }
}

inline Button *makeModalButton(const i18n::Text &label, int fontSize,
                               TextView **textOut = nullptr) {
  auto *button = new Button(0, 0, 160, 58);
  auto *text = new TextView(app_theme::font, fontSize);
  text->setLocalizedText(label);
  text->setAlign(TextView::CENTER);
  text->setVAlign(TextView::MIDDLE);
  button->setContentView(text);
  button->setStyledBorderWidth(1);
  button->setCornerRadius(app_theme::controlRadius());
  if (textOut != nullptr) {
    *textOut = text;
  }
  return button;
}

inline Color modalPanelBorder() { return app_theme::hairlineStrong(); }

} // namespace modal_view
