#pragma once

#include "AppTheme.h"
#include "Button.h"
#include "../rendering/ResponsiveUi.h"

namespace goal_ui {
// Add passive labels and progress to the returned container. The whole card
// owns the opening action; nested buttons would compete for pointer events.
inline View *clickableRow(View *parent, std::function<void()> onOpen) {
  auto *button = new Button();
  button->setName("goal-list-row");
  button->setWidthPercent(100)->setMinWidth(0)->setFlexShrink(0);
  button->setMinHeight(rendering::compactMenuCanvasActive ? 80 : 64);
  button->setFlexDirection(FlexDirection::Column);
  button->setPadding(Edge::All, rendering::compactMenuCanvasActive ? 16 : 12);
  button->setMargin(Edge::Bottom, 8);
  button->setCornerRadius(app_theme::panelRadius());
  button->setStyledBorderWidth(1);
  button->setThemedBackgroundColors(app_theme::panel, app_theme::controlHover,
                                    app_theme::controlPressed);
  button->setThemedBorderColors(app_theme::hairlineSubtle, app_theme::hairlineStrong,
                                app_theme::accentBorder);
  button->setOnClickListener(std::move(onOpen));

  auto *content = new View();
  content->setName("goal-list-row-content");
  content->setFlexDirection(FlexDirection::Column)->setMinWidth(0)->setGap(6);
  content->setFlexShrink(0);
  // A normal Yoga child determines the card's intrinsic height. Button's
  // separately owned contentView instead copies its frame and cannot size it.
  button->addView(content);
  parent->addView(button);
  return content;
}
} // namespace goal_ui
