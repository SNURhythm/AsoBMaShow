#pragma once
#include "AppTheme.h"
#include "Button.h"
#include "TextView.h"

namespace action_button {
inline void applyStyle(Button *button, TextView *label, app_theme::ActionCategory category) {
  const auto normal = [category] { return app_theme::actionPalette(category).normal; };
  const auto hover = [category] { return app_theme::actionPalette(category).hover; };
  const auto pressed = [category] { return app_theme::actionPalette(category).pressed; };
  button->setThemedBackgroundColors(normal, hover, pressed);
  if (category == app_theme::ActionCategory::Neutral) {
    button->setThemedBorderColors(app_theme::hairline, app_theme::accentBorder,
                                  app_theme::accentBorderStrong);
  } else {
    button->setThemedBorderColors(
        [normal] { return app_theme::withAlpha(normal(), 164); },
        [hover] { return app_theme::withAlpha(hover(), 206); },
        [pressed] { return app_theme::withAlpha(pressed(), 232); });
  }
  button->setStyledBorderWidth(1);
  label->setThemedColor([category] { return app_theme::actionPalette(category).label; });
}
} // namespace action_button
