#pragma once
#include "View.h"
#include "TextView.h"
#include "ClearLampColors.h"
#include "IconText.h"
#include "AppTheme.h"
#include "../rendering/UiSafeArea.h"
#include <algorithm>
#include <cstdint>

struct GoalBubblePlacement { int x, y, width, height; };
inline GoalBubblePlacement placeGoalBubble(int anchorX, int anchorY, int anchorWidth, int anchorHeight,
                                          int viewportWidth, int viewportHeight,
                                          rendering::UiSafeAreaInsets safe,
                                          bool outsideContainer = false,
                                          int containerY = 0, int containerHeight = 0) {
  const int left = std::clamp(safe.left + 4, 0, std::max(0, viewportWidth));
  const int right = std::clamp(viewportWidth - safe.right - 4, left, std::max(left, viewportWidth));
  const int top = std::clamp(safe.top + 4, 0, std::max(0, viewportHeight));
  const int bottom = std::clamp(viewportHeight - safe.bottom - 4, top, std::max(top, viewportHeight));
  const int width = std::min(150, right - left), height = std::min(48, bottom - top);
  const int before = anchorX - width - 4;
  const int after = anchorX + anchorWidth + 4;
  const int preferred = before >= left ? before : after;
  const int above = containerY - height - 4;
  const int below = containerY + containerHeight + 4;
  const int x = outsideContainer ? anchorX + (anchorWidth - width) / 2 : preferred;
  const int y = outsideContainer ? (above >= top ? above : below)
                                 : anchorY + (anchorHeight - height) / 2;
  return {.x = std::clamp(x, left, right - width),
          .y = std::clamp(y, top, bottom - height),
          .width = width, .height = height};
}

class GoalTargetBubble final : public View {
public:
  GoalTargetBubble() {
    setName("chartGoalBubble");
    setPositionType(YGPositionTypeAbsolute)->setPosition(Edge::Left, 0)->setPosition(Edge::Top, 0);
    setWidth(150)->setHeight(48)->setPadding(Edge::All, 4)->setCornerRadius(8);
    setThemedBackgroundColor(app_theme::panelStrong);
    setThemedBorderColor(app_theme::accentBorderStrong); setBorderWidth(1);
    setFlexDirection(FlexDirection::Column)->setJustifyContent(YGJustifyCenter);
    setZIndex(100);
    auto *lampRow = new View();
    lampRow->setName("chartGoalBubbleLampRow");
    lampRow->setFlexDirection(FlexDirection::Row)->setAlignItems(YGAlignCenter)->setGap(6);
    lampRow->setHeight(19)->setFlexShrink(0);
    lampBar_ = new View(); lampBar_->setName("chartGoalBubbleLampBar");
    lampBar_->setWidth(4)->setHeight(17)->setFlexShrink(0)->setCornerRadius(2);
    lampRow->addView(lampBar_);
    lamp_ = new TextView(main_menu_theme::font, 15);
    lamp_->setName("chartGoalBubbleLamp"); lamp_->setHeight(19)->setFlex(1)->setMinWidth(0);
    lamp_->setMinimumFontSize(10); lamp_->setOverflow(TextView::TextOverflow::Hidden);
    lamp_->setThemedColor(app_theme::textPrimary); lampRow->addView(lamp_);
    addView(lampRow); lampRow_ = lampRow;
    score_ = new TextView(main_menu_theme::font, 15);
    score_->setName("chartGoalBubbleScore"); score_->setHeight(19)->setFlexShrink(0);
    score_->setAlign(TextView::CENTER); score_->setThemedColor(app_theme::textPrimary);
    addView(score_); setVisible(false);
  }
  void setTarget(int rank, const std::string &lamp, const std::string &score) {
    hasTarget_ = !lamp.empty() || !score.empty();
    lamp_->setText(lamp); score_->setText(score);
    lampRow_->setVisible(!lamp.empty()); lampRow_->setDisplay(lamp.empty() ? YGDisplayNone : YGDisplayFlex);
    score_->setVisible(!score.empty()); score_->setDisplay(score.empty() ? YGDisplayNone : YGDisplayFlex);
    const bool colored = !lamp.empty() && hasClearLampColor(rank);
    lampBar_->setVisible(colored); lampBar_->setDisplay(colored ? YGDisplayFlex : YGDisplayNone);
    if (colored) lampBar_->setBackgroundColor(clearLampColorForRank(rank));
  }
  // Place beside the flag and clamp to the usable screen, including safe areas.
  void placeFor(const View &anchor, const View &container, int viewportWidth, int viewportHeight,
                bool outsideContainer = false) {
    const auto placement = placeGoalBubble(anchor.getX(), anchor.getY(), anchor.getWidth(), anchor.getHeight(),
                                           viewportWidth, viewportHeight, rendering::uiSafeAreaInsets(),
                                           outsideContainer, container.getY(), container.getHeight());
    if (placement.width != placedWidth_) { placedWidth_ = placement.width; setWidth(placement.width); }
    if (placement.height != placedHeight_) { placedHeight_ = placement.height; setHeight(placement.height); }
    setPositionNoLayout(placement.x - container.getX(), placement.y - container.getY());
  }
  void showFor(bool visible) {
    const bool next = visible && hasTarget_;
    if (next && !getVisible()) startedAt_ = SDL_GetTicks();
    setVisible(next);
  }
protected:
  float renderOpacity() const noexcept override {
    return std::clamp(float(SDL_GetTicks() - startedAt_) / 350.0f, 0.0f, 1.0f);
  }
  void renderImpl(RenderContext &) override {
    if (SDL_GetTicks() - startedAt_ < 350) ui::requestAnimationFrame();
  }
private:
  View *lampRow_ = nullptr, *lampBar_ = nullptr;
  TextView *lamp_ = nullptr, *score_ = nullptr;
  bool hasTarget_ = false;
  int placedWidth_ = -1, placedHeight_ = -1;
  std::uint64_t startedAt_ = 0;
};

