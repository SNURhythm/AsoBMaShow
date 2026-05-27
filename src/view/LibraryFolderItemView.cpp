#include "LibraryFolderItemView.h"
#include "ClearLampColors.h"
#include <algorithm>

namespace {
std::string indentLabel(const std::string &label, int depth) {
  return std::string(static_cast<size_t>(std::max(0, depth)) * 2, ' ') + label;
}
} // namespace

LibraryFolderItemView::LibraryFolderItemView(int x, int y, int width,
                                             int height)
    : View(x, y, width, height) {
  setFlexDirection(FlexDirection::Row);
  setAlignItems(YGAlignCenter);
  setPadding(Edge::All, 8);
  setPadding(Edge::End, 24);
  setGap(8);

  clearLamp = new View();
  clearLamp->setWidth(5)->setHeight(26)->setFlexShrink(0);
  addView(clearLamp);

  labelView = new TextView("assets/fonts/notosanscjkjp.ttf", 20);
  labelView->setVAlign(TextView::MIDDLE);
  labelView->setOverflow(TextView::TextOverflow::Marquee);
  labelView->setFlex(1);
  labelView->setFlexShrink(1);
  labelView->setMinWidth(0);
  addView(labelView);

  countView = new TextView("assets/fonts/notosanscjkjp.ttf", 16);
  countView->setAlign(TextView::RIGHT);
  countView->setVAlign(TextView::MIDDLE);
  countView->setOverflow(TextView::TextOverflow::Hidden);
  countView->setWidth(48);
  addView(countView);
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
  setBackgroundColor(Color(45, 79, 116, 220));
  setBorderColor(Color(105, 162, 222, 255));
  setBorderWidth(1);
  labelView->setColor({255, 255, 255, 255});
  countView->setColor({218, 232, 249, 255});
}

void LibraryFolderItemView::onUnselected() {
  setBackgroundColor(Color(12, 20, 32, 112));
  clearBorderColor();
  labelView->setColor({213, 224, 239, 255});
  countView->setColor({142, 163, 188, 255});
}
