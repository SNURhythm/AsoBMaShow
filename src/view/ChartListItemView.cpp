#include "ChartListItemView.h"

ChartListItemView::ChartListItemView(int x, int y, int width, int height,
                                     const bms_parser::ChartMeta &meta)
    : View(x, y, width, height) {
  this->setFlex(1);
  textLayout = new View();
  bannerImage = new ImageView(0, 0, 0, 0);
  titleView = new TextView("assets/fonts/notosanscjkjp.ttf", 32);
  titleView->setVAlign(TextView::TextVAlign::BOTTOM);
  artistView = new TextView("assets/fonts/notosanscjkjp.ttf", 16);
  artistView->setVAlign(TextView::TextVAlign::TOP);

  // Configure root layout
  this->setFlexDirection(FlexDirection::Row)->setAlignItems(YGAlignStretch);

  // Configure text layout
  textLayout->setFlexDirection(FlexDirection::Column);

  // Add banner image with configuration
  bannerImage->setWidth(static_cast<float>(height) / 80.0f * 300.0f)
      ->setHeight(static_cast<float>(height))
      ->setMargin(Edge::End, 8);
  this->addView(bannerImage);

  // Add text layout with configuration
  textLayout->setFlex(1);
  textLayout->setFlexGrow(1.5);
  this->addView(textLayout);

  // Add title and artist views to text layout
  titleView->setFlex(1);
  titleView->setFlexGrow(1.5f);
  textLayout->addView(titleView);

  artistView->setFlexGrow(1);
  textLayout->addView(artistView);

  // Add level view
  levelView = new TextView("assets/fonts/notosanscjkjp.ttf", 16);
  levelView->setAlign(TextView::TextAlign::RIGHT);
  levelView->setVAlign(TextView::TextVAlign::MIDDLE);
  levelView->setWidth(160)->setHeight(20);
  this->addView(levelView);

  keyModeOverlay = new TextView("assets/fonts/notosanscjkjp.ttf", 16);
  keyModeOverlay->setColor({255, 0, 0, 255});
  keyModeOverlay->setAlign(TextView::TextAlign::LEFT);
  keyModeOverlay->setVAlign(TextView::TextVAlign::MIDDLE);
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
                         ? std::to_string(meta.PlayLevel)
                         : meta.DifficultyTableLabels);
  std::string keyModeDesc;
  switch (meta.KeyMode) {
  case 5:
    keyModeDesc = "5K";
    break;
  case 7:
    keyModeDesc = "7K";
    break;
  case 10:
    keyModeDesc = "5KDP";
    break;
  case 14:
    keyModeDesc = "7KDP";
    break;
  }
  keyModeOverlay->setText(keyModeDesc);
  if (!meta.Banner.empty())
    bannerImage->setImage(meta.Folder / meta.Banner);
  else
    bannerImage->freeImage();
}

void ChartListItemView::renderImpl(RenderContext &context) {
  // print view size/position
  // SDL_Log("view size: %d, %d", getWidth(), getHeight());
  // SDL_Log("view position: %d, %d", getX(), getY());
  // print textlayout size/position
  // SDL_Log("textlayout size: %d, %d", textLayout->getWidth(),
  //         textLayout->getHeight());
  // SDL_Log("textLayout position: %d, %d", textLayout->getX(),
  //         textLayout->getY());
  keyModeOverlay->render(context);
  // SDL_Log("textLayout size: %d, %d", textLayout->getWidth(),
  //         textLayout->getHeight());
}

void ChartListItemView::onSelected() {
  titleView->setColor({255, 0, 0, 255});
  artistView->setColor({255, 0, 0, 255});
}

void ChartListItemView::onUnselected() {
  titleView->setColor({255, 255, 255, 255});
  artistView->setColor({255, 255, 255, 255});
}
