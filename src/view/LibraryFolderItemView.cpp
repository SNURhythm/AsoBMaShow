#include "LibraryFolderItemView.h"
#include "ClearLampColors.h"
#include "UiTheme.h"
#include <algorithm>

namespace {
constexpr int kBottomGap = 6;

std::string indentLabel(const std::string &label, int depth) {
  return std::string(static_cast<size_t>(std::max(0, depth)) * 2, ' ') + label;
}
} // namespace

LibraryFolderItemView::LibraryFolderItemView(int x, int y, int width,
                                             int height)
    : View(x, y, width, height) {
  setFlexDirection(FlexDirection::Column);
  setAlignItems(YGAlignStretch);
  setPadding(Edge::Bottom, kBottomGap);

  contentCard = new View();
  contentCard->setFlexDirection(FlexDirection::Row);
  contentCard->setAlignItems(YGAlignCenter);
  contentCard->setHeight(height > kBottomGap ? height - kBottomGap : height);
  contentCard->setFlexShrink(0);
  contentCard->setPadding(Edge::All, 8);
  contentCard->setPadding(Edge::End, 24);
  contentCard->setGap(8);
  addView(contentCard);

  clearLamp = new View();
  clearLamp->setWidth(5)->setHeight(26)->setFlexShrink(0);
  clearLamp->setCornerRadius(3.0f);
  contentCard->addView(clearLamp);

  labelView = new TextView("assets/fonts/notosanscjkjp.ttf", 20);
  labelView->setVAlign(TextView::MIDDLE);
  labelView->setOverflow(TextView::TextOverflow::Marquee);
  labelView->setFlex(1);
  labelView->setFlexShrink(1);
  labelView->setMinWidth(0);
  contentCard->addView(labelView);

  countView = new TextView("assets/fonts/notosanscjkjp.ttf", 16);
  countView->setAlign(TextView::RIGHT);
  countView->setVAlign(TextView::MIDDLE);
  countView->setOverflow(TextView::TextOverflow::Hidden);
  countView->setWidth(48);
  contentCard->addView(countView);
}

void LibraryFolderItemView::setItem(const std::string &label, int depth,
                                    int count, bool selected, int clearRank) {
  itemDepth = depth;
  labelView->setText(indentLabel(label, itemDepth));
  countView->setText(count >= 0 ? std::to_string(count) : "");
  if (hasClearLampColor(clearRank)) {
    clearLamp->setBackgroundColor(clearLampColorForRank(clearRank));
  } else {
    clearLamp->clearBackgroundColor();
  }
  if (selected) {
    onSelected();
  } else {
    onUnselected();
  }
}

void LibraryFolderItemView::onSelected() {
  contentCard->setThemedBackgroundColor(ui_theme::mainMenuItemSelected);
  contentCard->setCornerRadius(ui_theme::controlRadius());
  contentCard->setThemedBorderColor(ui_theme::accentBorderStrong);
  contentCard->setBorderWidth(1);
  labelView->setThemedColor(ui_theme::textPrimary);
  countView->setThemedColor(ui_theme::lime);
}

void LibraryFolderItemView::onUnselected() {
  contentCard->setThemedBackgroundColor(ui_theme::mainMenuItem);
  contentCard->setCornerRadius(ui_theme::controlRadius());
  contentCard->setThemedBorderColor(ui_theme::hairlineSubtle);
  contentCard->setBorderWidth(1);
  labelView->setThemedColor(ui_theme::textPrimary);
  countView->setThemedColor(ui_theme::textMuted);
}
