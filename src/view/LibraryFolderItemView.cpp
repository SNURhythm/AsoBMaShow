#include "LibraryFolderItemView.h"
#include "ClearLampColors.h"
#include "UiTheme.h"
#include <algorithm>

namespace {
constexpr int kBottomGap = 6;
constexpr int kDepthIndent = 18;
constexpr int kCardLeftPadding = 8;
constexpr int kLeafCardLeftPadding = 2;
constexpr int kDisclosureWidth = 14;
constexpr int kLeafDisclosureWidth = 4;
constexpr int kCountWidth = 48;
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

  disclosureView = new TextView("assets/fonts/notosanscjkjp.ttf", 16);
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
  disclosureView->setWidth(expandable ? kDisclosureWidth
                                      : kLeafDisclosureWidth);
  disclosureView->setText(expandable ? (expanded ? "v" : ">") : "");
  labelView->setText(label);
  const bool showCount = count >= 0;
  countView->setWidth(showCount ? kCountWidth : 0);
  countView->setText(showCount ? std::to_string(count) : "");
  if (hasClearLampColor(clearRank)) {
    const Color clearColor = clearLampColorForRank(clearRank);
    clearLamp->setBackgroundColor(
        itemClearMarkFolder ? ui_theme::withAlpha(ui_theme::textOn(clearColor),
                                                  176)
                            : clearColor);
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
  if (itemClearMarkFolder && hasClearLampColor(itemClearRank)) {
    const Color accent = clearLampColorForRank(itemClearRank);
    contentCard->setBackgroundColor(Color(accent.r, accent.g, accent.b, 255));
    contentCard->setCornerRadius(ui_theme::controlRadius());
    contentCard->setBorderColor(
        ui_theme::withAlpha(ui_theme::textOn(accent), 230));
    contentCard->setBorderWidth(1);
    const SDL_Color text = ui_theme::sdl(ui_theme::textOn(accent));
    disclosureView->setColor(text);
    labelView->setColor(text);
    countView->setColor(text);
    return;
  }

  contentCard->setThemedBackgroundColor(ui_theme::mainMenuItemSelected);
  contentCard->setCornerRadius(ui_theme::controlRadius());
  contentCard->setThemedBorderColor(ui_theme::accentBorderStrong);
  contentCard->setBorderWidth(1);
  disclosureView->setThemedColor(ui_theme::textSecondary);
  labelView->setThemedColor(ui_theme::textPrimary);
  countView->setThemedColor(ui_theme::lime);
}

void LibraryFolderItemView::onUnselected() {
  if (itemClearMarkFolder && hasClearLampColor(itemClearRank)) {
    const Color accent = clearLampColorForRank(itemClearRank);
    contentCard->setBackgroundColor(accent);
    contentCard->setCornerRadius(ui_theme::controlRadius());
    contentCard->setBorderColor(
        ui_theme::withAlpha(ui_theme::textOn(accent), 112));
    contentCard->setBorderWidth(1);
    const SDL_Color text = ui_theme::sdl(ui_theme::textOn(accent));
    disclosureView->setColor(text);
    labelView->setColor(text);
    countView->setColor(text);
    return;
  }

  contentCard->setThemedBackgroundColor(ui_theme::mainMenuItem);
  contentCard->setCornerRadius(ui_theme::controlRadius());
  contentCard->setThemedBorderColor(ui_theme::hairlineSubtle);
  contentCard->setBorderWidth(1);
  disclosureView->setThemedColor(ui_theme::textMuted);
  labelView->setThemedColor(ui_theme::textPrimary);
  countView->setThemedColor(ui_theme::textMuted);
}
