#include "ChartListItemView.h"
#include "Button.h"
#include "ClearLampColors.h"
#include "IconText.h"
#include "UiTheme.h"

#include <cmath>
#include <cstdint>
#include <iomanip>
#include <sstream>

namespace {
constexpr int kBottomGap = 8;
constexpr int kArtworkFramePadding = 3;
constexpr int kArtworkFrameBorderWidth = 1;
constexpr uint32_t kIconStar = 0xf005;

std::string formatPlayLevel(double level) {
  const double rounded = std::round(level);
  if (std::fabs(level - rounded) < 0.001) {
    return std::to_string(static_cast<int>(rounded));
  }

  std::ostringstream stream;
  stream << std::fixed << std::setprecision(1) << level;
  return stream.str();
}

std::string keyModeDescription(int keyMode) {
  switch (keyMode) {
  case 5:
    return "5K";
  case 7:
    return "7K";
  case 10:
    return "5KDP";
  case 14:
    return "7KDP";
  default:
    return std::to_string(keyMode) + "K";
  }
}
} // namespace

ChartListItemView::ChartListItemView(int x, int y, int width, int height,
                                     const ChartMetaRecord &record)
    : View(x, y, width, height) {
  (void)record;
  contentCard = new View();
  clearLamp = new View();
  artworkFrame = new View();
  jacketImage = new ImageView(0, 0, 0, 0);
  textLayout = new View();
  detailsLayout = new View();
  titleView = new TextView("assets/fonts/notosanscjkjp.ttf", 26);
  artistView = new TextView("assets/fonts/notosanscjkjp.ttf", 17);
  levelView = new TextView("assets/fonts/notosanscjkjp.ttf", 18);
  keyModeView = new TextView("assets/fonts/notosanscjkjp.ttf", 14);
  favoriteButton = new Button();
  favoriteIconView = new TextView(ui_icons::kFontAwesomeSolidPath, 24);

  this->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setPadding(Edge::Bottom, kBottomGap);

  contentCard->setFlexDirection(FlexDirection::Row)
      ->setAlignItems(YGAlignCenter)
      ->setHeight(height > kBottomGap ? height - kBottomGap : height)
      ->setFlexShrink(0)
      ->setPadding(Edge::All, 8)
      ->setPadding(Edge::End, 24)
      ->setGap(12);
  this->addView(contentCard);

  clearLamp->setWidth(6)->setHeight(78)->setFlexShrink(0);
  clearLamp->setCornerRadius(3.0f);
  contentCard->addView(clearLamp);

  // Stage file jacket
  artworkFrame->setWidth(84)
      ->setHeight(84)
      ->setFlexShrink(0)
      ->setPadding(Edge::All, kArtworkFramePadding)
      ->setAlignItems(YGAlignCenter)
      ->setJustifyContent(YGJustifyCenter)
      ->setThemedBackgroundColor(ui_theme::panelSubtle)
      ->setCornerRadius(ui_theme::controlRadius())
      ->setThemedBorderColor(ui_theme::hairlineSubtle)
      ->setBorderWidth(kArtworkFrameBorderWidth);
  jacketImage->setWidth(78)->setHeight(78);
  jacketImage->setCornerRadius(ui_theme::childRadiusForInset(
      ui_theme::controlRadius(), static_cast<float>(kArtworkFrameBorderWidth),
      static_cast<float>(kArtworkFramePadding)));
  artworkFrame->addView(jacketImage);
  contentCard->addView(artworkFrame);

  // Main text
  textLayout->setFlexDirection(FlexDirection::Column)
      ->setJustifyContent(YGJustifyCenter)
      ->setFlexGrow(1)
      ->setFlexBasis(0)
      ->setFlexShrink(1)
      ->setMinWidth(0)
      ->setGap(4);
  contentCard->addView(textLayout);

  titleView->setHeight(36);
  titleView->setVAlign(TextView::TextVAlign::BOTTOM);
  titleView->setOverflow(TextView::TextOverflow::Marquee);
  titleView->setAlignSelf(YGAlignStretch);
  titleView->setFlexShrink(1);
  titleView->setMinWidth(0);
  textLayout->addView(titleView);

  artistView->setHeight(24);
  artistView->setVAlign(TextView::TextVAlign::TOP);
  artistView->setOverflow(TextView::TextOverflow::Marquee);
  artistView->setAlignSelf(YGAlignStretch);
  artistView->setFlexShrink(1);
  artistView->setMinWidth(0);
  textLayout->addView(artistView);

  // Difficulty and key mode
  detailsLayout->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignFlexEnd)
      ->setJustifyContent(YGJustifyCenter)
      ->setWidth(210)
      ->setHeight(84)
      ->setFlexShrink(0)
      ->setGap(6);
  contentCard->addView(detailsLayout);

  levelView->setAlign(TextView::TextAlign::RIGHT);
  levelView->setVAlign(TextView::TextVAlign::MIDDLE);
  levelView->setOverflow(TextView::TextOverflow::Marquee);
  levelView->setWidth(210)->setHeight(28);
  detailsLayout->addView(levelView);

  keyModeView->setAlign(TextView::TextAlign::RIGHT);
  keyModeView->setVAlign(TextView::TextVAlign::MIDDLE);
  keyModeView->setOverflow(TextView::TextOverflow::Hidden);
  keyModeView->setWidth(210)->setHeight(20);
  detailsLayout->addView(keyModeView);
  favoriteButton->setWidth(52)
      ->setHeight(52)
      ->setFlexShrink(0)
      ->setCornerRadius(ui_theme::controlRadius());
  favoriteButton
      ->setThemedBackgroundColors(
          [] { return Color(0, 0, 0, 0); }, ui_theme::controlHover,
          ui_theme::controlPressed)
      ->setThemedBorderColors(
          [] { return Color(0, 0, 0, 0); }, ui_theme::hairlineStrong,
          ui_theme::accentBorder)
      ->setStyledBorderWidth(1);
  favoriteIconView->setText(ui_icons::textForCodepoint(kIconStar));
  favoriteIconView->setAlign(TextView::CENTER);
  favoriteIconView->setVAlign(TextView::MIDDLE);
  favoriteIconView->setOverflow(TextView::TextOverflow::Hidden);
  favoriteButton->setContentView(favoriteIconView);
  favoriteButton->setOnClickListener([this]() {
    if (solidArchive || unavailable || currentRecord.meta.BmsPath.empty()) {
      return;
    }
    const bool nextFavorite = !favorite;
    const auto toggledPath = currentRecord.meta.BmsPath;
    if (!favoriteToggleHandler || favoriteToggleHandler(currentRecord,
                                                        nextFavorite)) {
      if (currentRecord.meta.BmsPath == toggledPath) {
        setFavoriteState(nextFavorite);
      }
    }
  });
  contentCard->addView(favoriteButton);

  onUnselected();
  this->applyYogaLayout();
}

void ChartListItemView::setMeta(const ChartMetaRecord &record) {
  currentRecord = record;
  const auto &meta = record.meta;
  unavailable = record.unavailable;
  solidArchive = record.solidArchive;
  favorite = record.favorite;
  std::string title = meta.Title;
  if (!meta.SubTitle.empty()) {
    title += " " + meta.SubTitle;
  }
  titleView->setText(title);
  artistView->setText(meta.Artist);
  if (solidArchive) {
    levelView->setText(record.difficultyTableLabels.empty()
                           ? "Unzip required"
                           : record.difficultyTableLabels);
    keyModeView->setText("ARCHIVE");
  } else {
    levelView->setText(record.difficultyTableLabels.empty()
                           ? formatPlayLevel(meta.PlayLevel)
                           : record.difficultyTableLabels);
    keyModeView->setText(unavailable ? "MISSING"
                                     : keyModeDescription(meta.KeyMode));
  }
  if (!unavailable && !solidArchive && !meta.StageFile.empty()) {
    jacketImage->setImageAsync(meta.Folder / meta.StageFile);
  } else {
    jacketImage->freeImage();
  }
  favoriteButton->setVisible(!unavailable && !solidArchive &&
                             !meta.BmsPath.empty());
  refreshFavoriteButton();
}

void ChartListItemView::setClearRank(int clearRank) {
  if (!solidArchive && hasClearLampColor(clearRank)) {
    clearLamp->setBackgroundColor(clearLampColorForRank(clearRank));
  } else {
    clearLamp->clearBackgroundColor();
  }
}

void ChartListItemView::setFavoriteToggleHandler(
    std::function<bool(const ChartMetaRecord &, bool)> handler) {
  favoriteToggleHandler = std::move(handler);
}

void ChartListItemView::setFavoriteState(bool nextFavorite) {
  favorite = nextFavorite;
  currentRecord.favorite = nextFavorite;
  refreshFavoriteButton();
}

void ChartListItemView::refreshFavoriteButton() {
  if (favoriteIconView == nullptr) {
    return;
  }
  if (favorite) {
    favoriteIconView->setThemedColor(ui_theme::amber);
  } else {
    favoriteIconView->setThemedColor(selected ? ui_theme::textSecondary
                                              : ui_theme::textMuted);
  }
}

void ChartListItemView::onSelected() {
  selected = true;
  contentCard->setThemedBackgroundColor(ui_theme::mainMenuItemSelected);
  contentCard->setCornerRadius(ui_theme::controlRadius());
  contentCard->setThemedBorderColor(ui_theme::accentBorderStrong);
  contentCard->setBorderWidth(1);
  artworkFrame->setThemedBackgroundColor(ui_theme::controlHover);
  artworkFrame->setThemedBorderColor(ui_theme::accentBorder);
  applyTextColors(true);
  refreshFavoriteButton();
}

void ChartListItemView::onUnselected() {
  selected = false;
  contentCard->setThemedBackgroundColor(ui_theme::mainMenuItem);
  contentCard->setCornerRadius(ui_theme::controlRadius());
  contentCard->setThemedBorderColor(ui_theme::hairlineSubtle);
  contentCard->setBorderWidth(1);
  artworkFrame->setThemedBackgroundColor(ui_theme::control);
  artworkFrame->setThemedBorderColor(ui_theme::hairlineStrong);
  applyTextColors(false);
  refreshFavoriteButton();
}

void ChartListItemView::applyTextColors(bool selected) {
  if (solidArchive) {
    if (selected) {
      titleView->setThemedColor(ui_theme::amber);
    } else {
      titleView->setColor(ui_theme::sdl(Color(226, 181, 82, 255)));
    }
    artistView->setThemedColor(selected ? ui_theme::textSecondary
                                        : ui_theme::textMuted);
    levelView->setThemedColor(ui_theme::amber);
    keyModeView->setThemedColor(selected ? ui_theme::textSecondary
                                         : ui_theme::textMuted);
    return;
  }

  if (unavailable) {
    titleView->setThemedColor(ui_theme::coral);
    artistView->setColor(ui_theme::sdl(selected ? Color(255, 171, 158, 255)
                                                : Color(219, 101, 94, 255)));
    levelView->setColor(ui_theme::sdl(selected ? Color(255, 218, 208, 255)
                                               : Color(240, 132, 116, 255)));
    keyModeView->setColor(ui_theme::sdl(selected ? Color(255, 171, 158, 255)
                                                 : Color(211, 91, 84, 255)));
    return;
  }

  titleView->setThemedColor(ui_theme::textPrimary);
  artistView->setThemedColor(selected ? ui_theme::textSecondary
                                      : ui_theme::textMuted);
  if (selected) {
    levelView->setThemedColor(ui_theme::lime);
  } else {
    levelView->setThemedColor(
        [] { return ui_theme::withAlpha(ui_theme::cyan(), 218); });
  }
  keyModeView->setThemedColor(selected ? ui_theme::textSecondary
                                       : ui_theme::textMuted);
}
