#include "LibraryFolderItemView.h"
#include "ClearLampColors.h"
#include "IconText.h"
#include "UiTheme.h"
#include "MainMenuTheme.h"
#include <algorithm>

namespace {
constexpr int kBottomGap = 6;
constexpr int kDepthIndent = 28;
constexpr int kCardLeftPadding = 8;
constexpr int kLeafCardLeftPadding = 2;
constexpr int kItemGap = 8;
constexpr int kExpandableItemGap = 0;
constexpr int kDisclosureWidth = 14;
constexpr int kLeafDisclosureWidth = 4;
constexpr int kCountWidth = 48;
constexpr int kChevronRightIcon = 0xf0da;
constexpr int kChevronDownIcon = 0xf0d7;
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

  labelView = new TextView(main_menu_theme::font, 24);
  labelView->setVAlign(TextView::MIDDLE);
  labelView->setOverflow(TextView::TextOverflow::Marquee);
  labelView->setFlex(1);
  labelView->setFlexShrink(1);
  labelView->setMinWidth(0);
  contentCard->addView(labelView);
  groupLabelView = new TextView(main_menu_theme::font, 24, TextView::FontWeight::Bold);
  groupLabelView->setVAlign(TextView::MIDDLE);
  groupLabelView->setOverflow(TextView::TextOverflow::Hidden);
  groupLabelView->setFlex(1)->setMinWidth(0);
  contentCard->addView(groupLabelView);
  sectionLabelView = new TextView(main_menu_theme::font, 18);
  sectionLabelView->setVAlign(TextView::MIDDLE);
  sectionLabelView->setFlex(1)->setMinWidth(0);
  sectionLabelView->setThemedColor(main_menu_theme::muted);
  contentCard->addView(sectionLabelView);
  sectionRule = new View();
  sectionRule->setPositionType(YGPositionTypeAbsolute)->setPosition(Edge::Top, 0);
  sectionRule->setPosition(Edge::Left, 8)->setPosition(Edge::Right, 24)->setHeight(1);
  sectionRule->setThemedBackgroundColor(main_menu_theme::rule);
  contentCard->addView(sectionRule);

  countView = new TextView(main_menu_theme::font, 20);
  countView->setAlign(TextView::RIGHT);
  countView->setVAlign(TextView::MIDDLE);
  countView->setOverflow(TextView::TextOverflow::Hidden);
  countView->setWidth(kCountWidth);
  contentCard->addView(countView);
}

void LibraryFolderItemView::setItem(const std::string &label, int depth,
                                    int count, bool selected, int clearRank,
                                    bool clearMarkFolder, bool expandable,
                                    bool expanded, uint32_t icon, bool section) {
  sectionHeader = section;
  rootItem = depth == 0 && !section;
  itemDepth = depth;
  itemClearRank = clearRank;
  itemClearMarkFolder = clearMarkFolder;
  const bool leaf = !expandable;
  contentCard->setMargin(Edge::Left, 0);
  contentCard->setPadding(Edge::Left, std::max(0, itemDepth) * kDepthIndent + kCardLeftPadding);
  contentCard->setGap(icon ? 14 : 8);
  disclosureView->setWidth(section ? 0 : icon ? 28 : expandable ? kDisclosureWidth : kLeafDisclosureWidth);
  disclosureView->setText(icon ? ui_icons::textForCodepoint(icon) :
      expandable ? ui_icons::textForCodepoint(expanded ? kChevronDownIcon : kChevronRightIcon) : "");
  labelView->setVisible(!rootItem && !section);
  labelView->setDisplay(rootItem || section ? YGDisplayNone : YGDisplayFlex);
  sectionLabelView->setVisible(section);
  sectionLabelView->setDisplay(section ? YGDisplayFlex : YGDisplayNone);
  sectionLabelView->setText(label);
  groupLabelView->setVisible(rootItem);
  groupLabelView->setDisplay(rootItem ? YGDisplayFlex : YGDisplayNone);
  groupLabelView->setText(label);
  sectionRule->setVisible(section);
  clearLamp->setVisible(hasClearLampColor(clearRank));
  clearLamp->setDisplay(hasClearLampColor(clearRank) ? YGDisplayFlex : YGDisplayNone);
  labelView->setText(label);
  const bool showCount = count >= 0;
  countView->setWidth(showCount ? kCountWidth : 0);
  countView->setText(showCount ? std::to_string(count) : "");
  if (hasClearLampColor(clearRank)) {
    auto glow = clearLampColorForRank(clearRank);
    auto core = glow;
    core.r = static_cast<uint8_t>((3 * core.r + 255) / 4);
    core.g = static_cast<uint8_t>((3 * core.g + 255) / 4);
    core.b = static_cast<uint8_t>((3 * core.b + 255) / 4);
    core.a = 255;
    clearLamp->setBackgroundColor(core);
    if (clearRank == kClearTypeHardClearRank) glow = Color(140, 185, 255, 255);
    glow.a = 160;
    clearLamp->setShadow(glow, 0, 0, 10);
  } else {
    clearLamp->clearBackgroundColor();
    clearLamp->clearShadow();
  }
  if (selected && !section) {
    onSelected();
  } else {
    onUnselected();
  }
}

void LibraryFolderItemView::onSelected() {
  if (sectionHeader) return;
  groupLabelView->setOverflow(TextView::TextOverflow::Marquee);
  contentCard->setThemedBackgroundColor(main_menu_theme::action);
  contentCard->setCornerRadius(0);
  contentCard->setBorderWidth(0);
  disclosureView->setThemedColor(main_menu_theme::onAction);
  labelView->setThemedColor(main_menu_theme::onAction);
  groupLabelView->setThemedColor(main_menu_theme::onAction);
  countView->setThemedColor(main_menu_theme::onAction);
}

void LibraryFolderItemView::onUnselected() {
  groupLabelView->setOverflow(TextView::TextOverflow::Hidden);
  contentCard->clearBackgroundColor();
  contentCard->setCornerRadius(0);
  contentCard->setBorderWidth(0);
  disclosureView->setThemedColor(main_menu_theme::muted);
  labelView->setThemedColor(sectionHeader ? main_menu_theme::muted : main_menu_theme::ink);
  groupLabelView->setThemedColor(main_menu_theme::ink);
  countView->setThemedColor(main_menu_theme::muted);
}
