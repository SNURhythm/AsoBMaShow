#pragma once

#include "Button.h"
#include "TextView.h"
#include "UiTheme.h"

namespace modal_view {

inline void styleThemedActionButton(
    Button *button, TextView *text, bool enabled,
    View::ThemeColorProvider normal, View::ThemeColorProvider hover,
    View::ThemeColorProvider pressed, View::ThemeColorProvider border) {
  if (button == nullptr || text == nullptr) {
    return;
  }

  button->setEnabled(enabled);
  button->setCornerRadius(ui_theme::controlRadius());
  if (enabled) {
    button->setThemedBackgroundColors(normal, hover, pressed);
    button->setThemedBorderColors(
        [border] { return ui_theme::withAlpha(border(), 150); },
        [border] { return ui_theme::withAlpha(border(), 190); },
        [border] { return ui_theme::withAlpha(border(), 220); });
    text->setThemedColor([normal] { return ui_theme::textOn(normal()); });
  } else {
    button->setThemedBackgroundColors(
        ui_theme::panelSubtle, ui_theme::panelSubtle, ui_theme::panelSubtle);
    button->setThemedBorderColors(ui_theme::hairlineSubtle,
                                  ui_theme::hairlineSubtle,
                                  ui_theme::hairlineSubtle);
    text->setThemedColor(ui_theme::textMuted);
  }
}

inline Button *makeModalButton(const i18n::Text &label, int fontSize,
                               TextView **textOut = nullptr) {
  auto *button = new Button(0, 0, 160, 58);
  auto *text = new TextView("assets/fonts/notosanscjkjp.ttf", fontSize);
  text->setLocalizedText(label);
  text->setAlign(TextView::CENTER);
  text->setVAlign(TextView::MIDDLE);
  button->setContentView(text);
  button->setStyledBorderWidth(1);
  button->setCornerRadius(ui_theme::controlRadius());
  if (textOut != nullptr) {
    *textOut = text;
  }
  return button;
}

inline Color modalPanelBorder() {
  return ui_theme::activeMode() == ui_theme::ThemeMode::Light
             ? ui_theme::hairlineStrong()
             : Color(86, 118, 153, 210);
}

} // namespace modal_view
