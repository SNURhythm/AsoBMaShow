#pragma once
#include "View.h"
#include "AppTheme.h"
#include "ClearLampColors.h"
#include <algorithm>

// A static, responsive metric bar. Numeric labels belong to the enclosing card.
class GoalProgressBar final : public View {
public:
  GoalProgressBar(double currentPercent, double targetPercent, int lamp, bool achieved) {
    setHeight(24)->setFlexShrink(0)->setMargin(Edge::Top,6)->setMargin(Edge::Bottom,6);
    auto *track=new View();
    track->setPositionType(YGPositionTypeAbsolute)->setPosition(Edge::Left,0)->setPosition(Edge::Right,0)
        ->setPosition(Edge::Top,6)->setHeight(12)->setCornerRadius(6)->setThemedBackgroundColor(app_theme::insetSurface);
    addView(track);
    auto *fill=new View();
    fill->setHeight(12)->setWidthPercent(std::clamp(currentPercent,0.0,100.0))->setCornerRadius(6);
    fill->setThemedBackgroundColor([lamp,achieved] {
      return achieved ? app_theme::lime() : lamp>=0 ? clearLampColorForRank(lamp) : app_theme::cyan();
    });
    track->addView(fill);
    auto *marker=new View();
    marker->setPositionType(YGPositionTypeAbsolute)->setPosition(Edge::Top,1)->setHeight(22)
        ->setWidth(3)->setCornerRadius(1.5)->setThemedBackgroundColor(app_theme::textPrimary);
    // Align the end markers inside the track instead of clipping at 100%.
    YGNodeStyleSetPositionPercent(marker->getNode(),YGEdgeLeft,std::clamp(targetPercent,0.0,100.0));
    marker->setMargin(Edge::Left,targetPercent>=100 ? -3 : -1.5);
    addView(marker);
  }
};
