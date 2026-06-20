#include "ChartListItemView.h"
#include "ClearLampColors.h"
#include "UiTheme.h"

#include <cmath>
#include <iomanip>
#include <sstream>

namespace {
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
  clearLamp = new View();
  artworkFrame = new View();
  jacketImage = new ImageView(0, 0, 0, 0);
  textLayout = new View();
  detailsLayout = new View();
  titleView = new TextView("assets/fonts/notosanscjkjp.ttf", 26);
  artistView = new TextView("assets/fonts/notosanscjkjp.ttf", 17);
  levelView = new TextView("assets/fonts/notosanscjkjp.ttf", 18);
  keyModeView = new TextView("assets/fonts/notosanscjkjp.ttf", 14);

  // Configure root layout
  this->setFlexDirection(FlexDirection::Row)
      ->setAlignItems(YGAlignCenter)
      ->setPadding(Edge::All, 8)
      ->setPadding(Edge::End, 24)
      ->setGap(12);

  clearLamp->setWidth(6)->setHeight(78)->setFlexShrink(0);
  clearLamp->setCornerRadius(3.0f);
  this->addView(clearLamp);

  // Stage file jacket
  artworkFrame->setWidth(84)
      ->setHeight(84)
      ->setFlexShrink(0)
      ->setPadding(Edge::All, 3)
      ->setAlignItems(YGAlignCenter)
      ->setJustifyContent(YGJustifyCenter)
      ->setBackgroundColor(ui_theme::panelSubtle())
      ->setCornerRadius(ui_theme::controlRadius())
      ->setBorderColor(ui_theme::hairlineSubtle())
      ->setBorderWidth(1);
  jacketImage->setWidth(78)->setHeight(78);
  artworkFrame->addView(jacketImage);
  this->addView(artworkFrame);

  // Main text
  textLayout->setFlexDirection(FlexDirection::Column)
      ->setJustifyContent(YGJustifyCenter)
      ->setFlexGrow(1)
      ->setFlexBasis(0)
      ->setFlexShrink(1)
      ->setMinWidth(0)
      ->setGap(4);
  this->addView(textLayout);

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
  this->addView(detailsLayout);

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

  onUnselected();
  this->applyYogaLayout();
}

void ChartListItemView::setMeta(const ChartMetaRecord &record) {
  const auto &meta = record.meta;
  unavailable = record.unavailable;
  solidArchive = record.solidArchive;
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
}

void ChartListItemView::setClearRank(int clearRank) {
  if (!solidArchive && hasClearLampColor(clearRank)) {
    clearLamp->setBackgroundColor(clearLampColorForRank(clearRank));
  } else {
    clearLamp->clearBackgroundColor();
  }
}

void ChartListItemView::onSelected() {
  setBackgroundColor(ui_theme::panelStrong());
  setCornerRadius(ui_theme::controlRadius());
  setBorderColor(ui_theme::accentBorderStrong());
  setBorderWidth(1);
  artworkFrame->setBackgroundColor(ui_theme::controlHover());
  artworkFrame->setBorderColor(ui_theme::accentBorder());
  applyTextColors(true);
}

void ChartListItemView::onUnselected() {
  setBackgroundColor(ui_theme::panelSubtle());
  setCornerRadius(ui_theme::controlRadius());
  setBorderColor(ui_theme::hairlineSubtle());
  setBorderWidth(1);
  artworkFrame->setBackgroundColor(ui_theme::control());
  artworkFrame->setBorderColor(ui_theme::hairlineStrong());
  applyTextColors(false);
}

void ChartListItemView::applyTextColors(bool selected) {
  if (solidArchive) {
    titleView->setColor(
        ui_theme::sdl(selected ? ui_theme::amber() : Color(226, 181, 82, 255)));
    artistView->setColor(ui_theme::sdl(selected ? ui_theme::textSecondary()
                                                : ui_theme::textMuted()));
    levelView->setColor(ui_theme::sdl(ui_theme::amber()));
    keyModeView->setColor(ui_theme::sdl(selected ? ui_theme::textSecondary()
                                                 : ui_theme::textMuted()));
    return;
  }

  if (unavailable) {
    titleView->setColor(ui_theme::sdl(ui_theme::coral()));
    artistView->setColor(ui_theme::sdl(selected ? Color(255, 171, 158, 255)
                                                : Color(219, 101, 94, 255)));
    levelView->setColor(ui_theme::sdl(selected ? Color(255, 218, 208, 255)
                                               : Color(240, 132, 116, 255)));
    keyModeView->setColor(ui_theme::sdl(selected ? Color(255, 171, 158, 255)
                                                 : Color(211, 91, 84, 255)));
    return;
  }

  titleView->setColor(ui_theme::sdl(ui_theme::textPrimary()));
  artistView->setColor(ui_theme::sdl(selected ? ui_theme::textSecondary()
                                              : ui_theme::textMuted()));
  levelView->setColor(
      ui_theme::sdl(selected ? ui_theme::lime()
                             : ui_theme::withAlpha(ui_theme::cyan(), 218)));
  keyModeView->setColor(ui_theme::sdl(selected ? ui_theme::textSecondary()
                                               : ui_theme::textMuted()));
}
