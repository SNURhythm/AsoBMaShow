#pragma once

#include "AppTheme.h"
#include "TextView.h"
#include "../i18n/Localization.h"

namespace goal_ui {
inline View *targetPanel(View *parent) {
  auto *panel = new View();
  panel->setFlexDirection(FlexDirection::Column)->setGap(6)->setMinWidth(0)->setFlexShrink(0)
      ->setPadding(Edge::All, 12)->setCornerRadius(12);
  panel->setThemedBackgroundColor([] { return app_theme::withAlpha(app_theme::cyan(), 24); });
  auto *caption = new TextView(app_theme::font, 16);
  caption->setText(i18n::tr("goals.ui.target"));
  caption->setWrap(true);
  caption->setMinHeight(24)->setFlexShrink(0);
  caption->setThemedColor(app_theme::cyan);
  panel->addView(caption);
  parent->addView(panel);
  return panel;
}
} // namespace goal_ui
