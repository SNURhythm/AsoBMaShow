#include "ChartListItemView.h"
#include "ClearLampColors.h"

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
                                     const bms_parser::ChartMeta &meta)
    : View(x, y, width, height) {
  (void)meta;
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
  this->addView(clearLamp);

  // Stage file jacket
  artworkFrame->setWidth(84)
      ->setHeight(84)
      ->setFlexShrink(0)
      ->setPadding(Edge::All, 3)
      ->setAlignItems(YGAlignCenter)
      ->setJustifyContent(YGJustifyCenter)
      ->setBackgroundColor(Color(8, 14, 23, 224))
      ->setBorderColor(Color(42, 58, 78, 255))
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

void ChartListItemView::setMeta(const bms_parser::ChartMeta &meta) {
  std::string title = meta.Title;
  if (!meta.SubTitle.empty()) {
    title += " " + meta.SubTitle;
  }
  titleView->setText(title);
  artistView->setText(meta.Artist);
  levelView->setText(meta.DifficultyTableLabels.empty()
                         ? formatPlayLevel(meta.PlayLevel)
                         : meta.DifficultyTableLabels);
  keyModeView->setText(keyModeDescription(meta.KeyMode));
  if (!meta.StageFile.empty()) {
    jacketImage->setImage(meta.Folder / meta.StageFile);
  } else {
    jacketImage->freeImage();
  }
}

void ChartListItemView::setClearRank(int clearRank) {
  if (hasClearLampColor(clearRank)) {
    clearLamp->setBackgroundColor(clearLampColorForRank(clearRank));
  } else {
    clearLamp->clearBackgroundColor();
  }
}

void ChartListItemView::onSelected() {
  setBackgroundColor(Color(32, 55, 82, 214));
  setBorderColor(Color(93, 149, 208, 255));
  setBorderWidth(1);
  artworkFrame->setBackgroundColor(Color(13, 25, 39, 240));
  artworkFrame->setBorderColor(Color(126, 185, 238, 255));
  titleView->setColor({255, 255, 255, 255});
  artistView->setColor({219, 232, 247, 255});
  levelView->setColor({245, 250, 255, 255});
  keyModeView->setColor({180, 210, 239, 255});
}

void ChartListItemView::onUnselected() {
  setBackgroundColor(Color(7, 12, 20, 112));
  setBorderColor(Color(27, 39, 55, 128));
  setBorderWidth(1);
  artworkFrame->setBackgroundColor(Color(8, 14, 23, 224));
  artworkFrame->setBorderColor(Color(42, 58, 78, 255));
  titleView->setColor({235, 242, 250, 255});
  artistView->setColor({152, 174, 198, 255});
  levelView->setColor({178, 224, 248, 255});
  keyModeView->setColor({113, 141, 169, 255});
}
