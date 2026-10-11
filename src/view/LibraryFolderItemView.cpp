#include "LibraryFolderItemView.h"
#include "ClearLampColors.h"
#include "IconText.h"
#include "UiTheme.h"
#include "MainMenuTheme.h"
#include <algorithm>

namespace {
constexpr int kBottomGap = 6;
constexpr int kDepthIndent = 18;
constexpr int kCardLeftPadding = 8;
constexpr int kLeafCardLeftPadding = 2;
constexpr int kItemGap = 8;
constexpr int kExpandableItemGap = 0;
constexpr int kDisclosureWidth = 14;
constexpr int kLeafDisclosureWidth = 4;
constexpr int kCountWidth = 48;
constexpr int kChevronRightIcon = 0xf054;
constexpr int kChevronDownIcon = 0xf078;
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
  contentCard->setGap(kItemGap);
  addView(contentCard);

  disclosureView = new TextView(ui_icons::kFontAwesomeSolidPath, 15);
  disclosureView->setWidth(kDisclosureWidth);
  disclosureView->setHeight(26);
  disclosureView->setAlign(TextView::CENTER);
  disclosureView->setVAlign(TextView::MIDDLE);
  disclosureView->setOverflow(TextView::TextOverflow::Hidden);
  disclosureView->setFlexShrink(0);
  contentCard->addView(disclosureView);

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
  countView->setWidth(kCountWidth);
  contentCard->addView(countView);
}

void LibraryFolderItemView::setItem(const std::string &label, int depth,
                                    int count, bool selected, int clearRank,
                                    bool clearMarkFolder, bool expandable,
                                    bool expanded) {
  itemDepth = depth;
  itemClearRank = clearRank;
  itemClearMarkFolder = clearMarkFolder;
  const bool leaf = !expandable;
  contentCard->setMargin(Edge::Left,
                         static_cast<float>(std::max(0, itemDepth) *
                                            kDepthIndent));
  contentCard->setPadding(Edge::Left,
                          leaf ? kLeafCardLeftPadding : kCardLeftPadding);
  contentCard->setGap(expandable ? kExpandableItemGap : kItemGap);
  disclosureView->setWidth(expandable ? kDisclosureWidth
                                      : kLeafDisclosureWidth);
  disclosureView->setText(
      expandable ? ui_icons::textForCodepoint(expanded ? kChevronDownIcon
                                                       : kChevronRightIcon)
                 : "");
  labelView->setText(label);
  const bool showCount = count >= 0;
  countView->setWidth(showCount ? kCountWidth : 0);
  countView->setText(showCount ? std::to_string(count) : "");
  if (hasClearLampColor(clearRank)) {
    const Color clearColor = clearLampColorForRank(clearRank);
    clearLamp->setBackgroundColor(
        clearColor);
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
  contentCard->setThemedBackgroundColor(main_menu_theme::action);
  contentCard->setCornerRadius(0);
  contentCard->setBorderWidth(0);
  disclosureView->setThemedColor(main_menu_theme::onAction);
  labelView->setThemedColor(main_menu_theme::onAction);
  countView->setThemedColor(main_menu_theme::onAction);
}

void LibraryFolderItemView::onUnselected() {
  contentCard->clearBackgroundColor();
  contentCard->setCornerRadius(0);
  contentCard->setBorderWidth(0);
  disclosureView->setThemedColor(main_menu_theme::muted);
  labelView->setThemedColor(main_menu_theme::ink);
  countView->setThemedColor(main_menu_theme::muted);
}
