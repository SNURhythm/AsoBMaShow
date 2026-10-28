#pragma once

#include "ClearLampColors.h"
#include "View.h"

// Draw the same rounded shape as the tab, clipped to a narrow left strip.
class GoalLampTabAccent final : public View {
public:
  explicit GoalLampTabAccent(int rank) {
    rounded_.setBackgroundColor(clearLampColorForRank(rank));
    rounded_.setCornerRadius(16);
    setPositionType(YGPositionTypeAbsolute);
    setPosition(Edge::Left, 0)->setPosition(Edge::Right, 0)
        ->setPosition(Edge::Top, 0)->setPosition(Edge::Bottom, 0);
  }
  void onLayout() override {
    // Parent layout can establish this child's size without a resize callback.
    rounded_.setSize(getWidth(), getHeight());
    rounded_.applyYogaLayout();
    rounded_.setPositionNoLayout(getX(), getY());
  }
  void onMove(int, int) override { rounded_.setPositionNoLayout(getX(), getY()); }
protected:
  void renderImpl(RenderContext &context) override {
    ScissorScope clip(context, getX(), getY(), 7, getHeight());
    rounded_.render(context);
  }
private:
  View rounded_;
};
