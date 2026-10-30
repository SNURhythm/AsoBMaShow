#pragma once
#include "ToggleButtonContent.h"
#include "../rendering/ResponsiveUi.h"

inline Button *makeGoalKindCycleButton(bool folders, std::function<void()> click) {
  auto *label = new TextView(app_theme::font, 21);
  label->setLocalizedText(i18n::message(folders ? "folder_goals.ui.title" : "goals.ui.chart_goals"));
  auto *button = new toggle_button::CompactButton([label] {
    return std::max(label->measureTextWidth(i18n::tr("folder_goals.ui.title")),
        label->measureTextWidth(i18n::tr("goals.ui.chart_goals"))) + cycle_button::kContentInset;
  });
  button->setName("goal-kind-cycle");
  button->setHeight(rendering::compactMenuCanvasActive ? 66 : 52)->setMargin(Edge::Bottom, 6);
  button->setContentView(cycle_button::makeContent(label));
  button->setOnClickListener(std::move(click));
  toggle_button::style(button, false);
  return button;
}
