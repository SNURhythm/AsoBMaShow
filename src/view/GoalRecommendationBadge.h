#pragma once
#include "View.h"

inline View *makeGoalRecommendationBadge() {
  auto *badge = new View();
  badge->setName("goalRecommendationBadge");
  badge->setPositionType(YGPositionTypeAbsolute);
  badge->setPosition(Edge::Top, 8)->setPosition(Edge::Right, 4);
  badge->setWidth(8)->setHeight(8)->setCornerRadius(4);
  badge->setBackgroundColor(Color(232, 64, 64, 255));
  badge->setVisible(false);
  return badge;
}
