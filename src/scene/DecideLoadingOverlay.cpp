#include "DecideLoadingOverlay.h"

#include "../view/UiTheme.h"
#include "../view/TextView.h"
#include "../view/ImageView.h"

#include <cstdio>
#include <vector>

DecideLoadingOverlay::DecideLoadingOverlay(int x, int y, int width, int height,
                                           const ChartMetaRecord &record)
    : BlockingOverlayView(x, y, width, height) {
  setPositionType(YGPositionTypeAbsolute);
  setFlexDirection(FlexDirection::Column);
  setJustifyContent(YGJustifyCenter);
  setAlignItems(YGAlignCenter);
  setPadding(Edge::All, 24);
  setThemedBackgroundColor(ui_theme::backdrop);
  setChart(record);
}

void DecideLoadingOverlay::setChart(const ChartMetaRecord &record) {
  titleText_ = record.meta.Title;
  if (!record.meta.SubTitle.empty()) {
    titleText_ += " " + record.meta.SubTitle;
  }
  artistText_ = record.meta.Artist;
  difficultyText_ = record.meta.PlayLevelText;
  if (difficultyText_.empty() && record.meta.PlayLevel > 0) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.0f", record.meta.PlayLevel);
    difficultyText_ = buffer;
  }
  if (!record.meta.StageFile.empty()) {
    stageFileResourcePath_ =
        (record.meta.BmsPath.parent_path() / record.meta.StageFile)
            .generic_string();
  } else {
    stageFileResourcePath_.clear();
  }
  rebuild();
}

void DecideLoadingOverlay::rebuild() {
  clearChildren();

  auto *content = new View();
  content->setFlexDirection(FlexDirection::Column);
  content->setAlignItems(YGAlignCenter);
  content->setJustifyContent(YGJustifyCenter);
  content->setPadding(Edge::All, 32);
  content->setCornerRadius(ui_theme::panelRadius());
  content->setThemedBackgroundColor(ui_theme::panelStrong);

  if (!stageFileResourcePath_.empty()) {
    auto *stage = new ImageView(0, 0, 512, 288, stageFileResourcePath_);
    stage->setMargin(Edge::Bottom, 24);
    stage->setImageAsync(stageFileResourcePath_);
    content->addView(stage);
  }

  auto *title = new TextView("assets/fonts/notosanscjkjp.ttf", 36);
  title->setText(titleText_);
  title->setThemedColor(ui_theme::textPrimary);
  title->setAlign(TextView::CENTER);
  title->setWrap(true);
  title->setOverflow(TextView::TextOverflow::Hidden);
  content->addView(title);

  auto *artist = new TextView("assets/fonts/notosanscjkjp.ttf", 20);
  artist->setText(artistText_);
  artist->setThemedColor(ui_theme::textSecondary);
  artist->setAlign(TextView::CENTER);
  artist->setWrap(true);
  artist->setOverflow(TextView::TextOverflow::Hidden);
  content->addView(artist);

  auto *row = new View();
  row->setFlexDirection(FlexDirection::Row);
  row->setJustifyContent(YGJustifyCenter);
  row->setAlignItems(YGAlignCenter);
  row->setMargin(Edge::Top, 20);

  if (!difficultyText_.empty()) {
    auto *difficulty = new TextView("assets/fonts/notosanscjkjp.ttf", 20);
    difficulty->setText(difficultyText_);
    difficulty->setThemedColor(ui_theme::textPrimary);
    difficulty->setMargin(Edge::Right, 12);
    row->addView(difficulty);
  }

  auto *loading = new TextView("assets/fonts/notosanscjkjp.ttf", 18);
  loading->setText("Loading...");
  loading->setThemedColor(ui_theme::textSecondary);
  row->addView(loading);

  content->addView(row);
  addView(content);
}