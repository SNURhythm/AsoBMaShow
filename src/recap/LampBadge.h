#pragma once
#include "../view/ClearLampColors.h"
#include "../view/TextView.h"

namespace recap {
inline void styleLampBadge(TextView *label, int rank) {
  auto color = clearLampColorForRank(rank);
  color.a = 255;
  label->setColor({8,12,12,255});
  label->setAlign(TextView::CENTER);
  label->setAlignSelf(YGAlignFlexStart)->setCornerRadius(8)
      ->setPadding(Edge::Left,10)->setPadding(Edge::Right,10)
      ->setPadding(Edge::Top,5)->setPadding(Edge::Bottom,5)
      ->setBackgroundColor(color)->setBorderWidth(1)->setBorderColor({128,128,128,120});
  YGNodeStyleSetMaxWidthPercent(label->getNode(),100);
}
} // namespace recap
