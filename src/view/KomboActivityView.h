#pragma once
#include "TextView.h"
#include "AppTheme.h"
#include "IconText.h"
#include "KomboFlameView.h"
#include "../i18n/Localization.h"
#include "../kombo/KomboDashboardPresentation.h"

class KomboActivityView final : public View {
public:
  KomboActivityView(const kombo::State &state, kombo::DayId today) {
    setName("komboRecentActivity");
    setFlexDirection(FlexDirection::Column)->setGap(12)->setFlexShrink(0);
    text(this, i18n::tr("kombo.ui.activity_title"), 20);
    auto *days = new View();
    days->setFlexDirection(FlexDirection::Row)->setGap(6)->setFlexShrink(0);
    addView(days);
    for (const auto &day : kombo::recentActivity(state, today)) {
      auto *cell = new View();
      cell->setName("komboActivity:" + kombo::dateLabel(day.day));
      cell->setFlexDirection(FlexDirection::Column)->setAlignItems(YGAlignStretch)
          ->setFlex(1)->setFlexBasis(0)->setWidth(0)->setMinWidth(0)
          ->setPadding(Edge::All, 4)->setGap(6)->setCornerRadius(12)
          ->setThemedBackgroundColor(app_theme::insetSurface);
      if (day.day == today) cell->setBorderWidth(2)->setThemedBorderColor(app_theme::textSecondary);
      auto date = kombo::dateLabel(day.day).substr(5);
      std::replace(date.begin(), date.end(), '-', '/');
      text(cell, day.day == today ? i18n::tr("kombo.ui.today") : date, 16)->setAlign(TextView::CENTER);
      auto *symbol = icon(cell, day.kind, 24);
      symbol->setHeight(34)->setWidthPercent(100);
      days->addView(cell);
    }
    auto *legend = new View();
    legend->setFlexDirection(FlexDirection::Row)->setFlexWrap(YGWrapWrap)->setGap(12)->setFlexShrink(0);
    addView(legend);
    for (auto kind : {kombo::ActivityKind::GoalReached, kombo::ActivityKind::Played,
                      kombo::ActivityKind::Frozen, kombo::ActivityKind::NoPlay}) {
      auto *item = new View();
      item->setFlexDirection(FlexDirection::Row)->setAlignItems(YGAlignCenter)->setGap(6);
      icon(item, kind, 20)->setWidth(30)->setHeight(34);
      text(item, i18n::tr(description(kind)), 16);
      legend->addView(item);
    }
  }
private:
  static const char *description(kombo::ActivityKind kind) {
    switch (kind) {
      case kombo::ActivityKind::GoalReached: return "kombo.ui.activity_goal";
      case kombo::ActivityKind::Played: return "kombo.ui.activity_played";
      case kombo::ActivityKind::Frozen: return "kombo.ui.activity_frozen";
      case kombo::ActivityKind::NoPlay: return "kombo.ui.activity_no_play";
      case kombo::ActivityKind::NotStarted: break;
    }
    return "kombo.ui.activity_no_play";
  }
  static TextView *text(View *parent, const std::string &value, int size) {
    auto *view = new TextView(app_theme::font, size);
    view->setText(value);
    view->setWrap(true);
    view->setMinHeight(size + 8)->setFlexShrink(0);
    view->setThemedColor(app_theme::textSecondary);
    parent->addView(view);
    return view;
  }
  static View *icon(View *parent, kombo::ActivityKind kind, int size) {
    if (kind == kombo::ActivityKind::Played || kind == kombo::ActivityKind::GoalReached ||
        kind == kombo::ActivityKind::NoPlay) {
      // Reuse the hero's flame states, held still for the compact history strip.
      auto *view = new KomboFlameView(kind != kombo::ActivityKind::NoPlay,
          kind == kombo::ActivityKind::GoalReached, true);
      parent->addView(view);
      return view;
    }
    const bool notStarted = kind == kombo::ActivityKind::NotStarted;
    auto *view = new TextView(notStarted ? app_theme::font : ui_icons::kFontAwesomeSolidPath, size);
    view->setText(notStarted ? "—" : ui_icons::textForCodepoint(ui_icons::kSnowflake));
    view->setAlign(TextView::CENTER);
    view->setVAlign(TextView::MIDDLE);
    view->setThemedColor(notStarted ? app_theme::textMuted : app_theme::cyan);
    parent->addView(view);
    return view;
  }
};
