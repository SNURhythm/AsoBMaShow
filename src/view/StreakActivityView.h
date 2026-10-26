#pragma once
#include "TextView.h"
#include "AppTheme.h"
#include "IconText.h"
#include "../i18n/Localization.h"
#include "../streak/StreakDashboardPresentation.h"

class StreakActivityView final : public View {
public:
  StreakActivityView(const streak::State &state, streak::DayId today) {
    setName("streakRecentActivity");
    setFlexDirection(FlexDirection::Column)->setGap(12)->setFlexShrink(0);
    text(this, i18n::tr("streak.ui.activity_title"), 20);
    auto *days = new View();
    days->setFlexDirection(FlexDirection::Row)->setGap(6)->setFlexShrink(0);
    addView(days);
    bool beforeStart = false;
    for (const auto &day : streak::recentActivity(state, today)) {
      beforeStart |= day.kind == streak::ActivityKind::NotStarted;
      auto *cell = new View();
      cell->setName("streakActivity:" + streak::dateLabel(day.day));
      cell->setFlexDirection(FlexDirection::Column)->setAlignItems(YGAlignStretch)
          ->setFlex(1)->setFlexBasis(0)->setWidth(0)->setMinWidth(0)
          ->setPadding(Edge::All, 4)->setGap(6)->setCornerRadius(12)
          ->setThemedBackgroundColor(app_theme::insetSurface);
      if (day.day == today) cell->setBorderWidth(2)->setThemedBorderColor(app_theme::textSecondary);
      auto date = streak::dateLabel(day.day).substr(5);
      std::replace(date.begin(), date.end(), '-', '/');
      text(cell, day.day == today ? i18n::tr("streak.ui.today") : date, 16)->setAlign(TextView::CENTER);
      auto *symbol = icon(cell, day.kind, 24);
      symbol->setHeight(34)->setWidthPercent(100);
      days->addView(cell);
    }
    auto *legend = new View();
    legend->setFlexDirection(FlexDirection::Row)->setFlexWrap(YGWrapWrap)->setGap(12)->setFlexShrink(0);
    addView(legend);
    for (auto kind : {streak::ActivityKind::GoalReached, streak::ActivityKind::Played,
                      streak::ActivityKind::Frozen, streak::ActivityKind::NoPlay,
                      streak::ActivityKind::NotStarted}) {
      if (kind == streak::ActivityKind::NotStarted && !beforeStart) continue;
      auto *item = new View();
      item->setFlexDirection(FlexDirection::Row)->setAlignItems(YGAlignCenter)->setGap(6);
      icon(item, kind, 14)->setWidth(18)->setHeight(24);
      text(item, i18n::tr(description(kind)), 16);
      legend->addView(item);
    }
  }
private:
  static const char *description(streak::ActivityKind kind) {
    switch (kind) {
      case streak::ActivityKind::GoalReached: return "streak.ui.activity_goal";
      case streak::ActivityKind::Played: return "streak.ui.activity_played";
      case streak::ActivityKind::Frozen: return "streak.ui.activity_frozen";
      case streak::ActivityKind::NoPlay: return "streak.ui.activity_no_play";
      case streak::ActivityKind::NotStarted: return "streak.ui.activity_not_started";
    }
    return "streak.ui.activity_no_play";
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
  static TextView *icon(View *parent, streak::ActivityKind kind, int size) {
    const bool notStarted = kind == streak::ActivityKind::NotStarted;
    auto *view = new TextView(notStarted ? app_theme::font : ui_icons::kFontAwesomeSolidPath, size);
    const auto glyph = kind == streak::ActivityKind::GoalReached ? ui_icons::kCheck :
        kind == streak::ActivityKind::Played ? ui_icons::kPlay :
        kind == streak::ActivityKind::Frozen ? ui_icons::kSnowflake : ui_icons::kSquare;
    view->setText(notStarted ? "—" : ui_icons::textForCodepoint(glyph));
    view->setAlign(TextView::CENTER);
    view->setVAlign(TextView::MIDDLE);
    view->setThemedColor(kind == streak::ActivityKind::GoalReached ? app_theme::lime :
        kind == streak::ActivityKind::Played ? app_theme::amber :
        kind == streak::ActivityKind::Frozen ? app_theme::cyan : app_theme::textMuted);
    parent->addView(view);
    return view;
  }
};
