#include "../i18n/Localization.h"
#include "ChartListItemView.h"
#include "Button.h"
#include "ClearLampColors.h"
#include "IconText.h"
#include "UiTheme.h"
#include "MainMenuTheme.h"
#include "../ScoreRankUtils.h"

#include <cmath>
#include <cstdint>
#include <iomanip>
#include <locale>
#include <sstream>

namespace {
constexpr int kBottomGap = 2;
constexpr int kArtworkFramePadding = 3;
constexpr int kArtworkFrameBorderWidth = 1;
constexpr int kBannerWidth = 368;
constexpr int kDifficultyColumnWidth = 112;
constexpr uint32_t kIconStar = 0xf005;
constexpr const char *kUiFont = "assets/fonts/notosanscjkjp.ttf";

std::string formatPlayLevel(double level) {
  const double rounded = std::round(level);
  if (std::fabs(level - rounded) < 0.001) {
    return std::to_string(static_cast<int>(rounded));
  }

  std::ostringstream stream;
  stream << std::fixed << std::setprecision(1) << level;
  return stream.str();
}

std::string keyModeDescription(const bms_parser::ChartMeta &meta) {
  const bool hasScratch = meta.TotalScratchNotes > 0 || meta.TotalBackSpinNotes > 0;
  switch (meta.KeyMode) {
  case 5:
    return hasScratch ? "5K1S" : "5K";
  case 7:
    return hasScratch ? "7K1S" : "7K";
  case 10:
    return "5KDP";
  case 14:
    return "7KDP";
  default:
    return std::to_string(meta.KeyMode) + "K";
  }
}
} // namespace

ChartListItemView::ChartListItemView(int x, int y, int width, int height,
                                     const ChartMetaRecord &record)
    : View(x, y, width, height) {
  (void)record;
  contentCard = new View();
  bannerImage = new ImageView(0, 0, 0, 0);
  clearLamp = new View();
  artworkFrame = new View();
  jacketImage = new ImageView(0, 0, 0, 0);
  textLayout = new View();
  detailsLayout = new View();
  scoreRankColumn = new View();
  titleView = new TextView(kUiFont, 28, TextView::FontWeight::Bold);
  artistView = new TextView(kUiFont, 17);
  levelView = new TextView(kUiFont, 24, TextView::FontWeight::Bold);
  keyModeView = new TextView(kUiFont, 14, TextView::FontWeight::Bold);
  scoreRankShadowView = new TextView(kUiFont, 34);
  scoreRankWeightView = new TextView(kUiFont, 34);
  scoreRankView = new TextView(kUiFont, 34, TextView::FontWeight::Bold);
  bestScoreView = new TextView(kUiFont, 18, TextView::FontWeight::Bold);
  favoriteButton = new Button();
  favoriteIconView = new TextView(ui_icons::kFontAwesomeSolidPath, 20);

  this->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setPadding(Edge::Bottom, kBottomGap);

  const int contentCardHeight = height > kBottomGap ? height - kBottomGap
                                                     : height;
  contentCard->setName("chartListContentCard");
  contentCard->setFlexDirection(FlexDirection::Row)
      ->setAlignItems(YGAlignCenter)
      ->setHeight(contentCardHeight)
      ->setFlexShrink(0)
      ->setPadding(Edge::All, 8)
      ->setPadding(Edge::End, 24)
      ->setGap(12);
  this->addView(contentCard);

  bannerImage->setName("chartListBanner");
  bannerImage->setPositionType(YGPositionTypeAbsolute)
      ->setPosition(Edge::Left, YGUndefined)
      ->setPosition(Edge::Top, 0)
      ->setPosition(Edge::Right, 0)
      ->setWidth(kBannerWidth)
      ->setHeight(std::max(0, contentCardHeight - 2))
      ->setCornerRadius(ui_theme::childRadiusForInset(
          ui_theme::controlRadius(), 1.0F, 0.0F));
  bannerImage->setFade(ImageFadeDirection::LeftToRight, 1.0F);
  bannerImage->setThemedScrimColor([] {
    return ui_theme::activeMode() == ui_theme::ThemeMode::Light
               ? Color(255, 255, 255, 168)
               : Color(5, 10, 18, 144);
  });
  contentCard->addView(bannerImage);

  clearLamp->setName("chartListClearLamp");
  clearLamp->setWidth(8)->setHeight(78)->setFlexShrink(0);
  clearLamp->setBorderColor(Color(76, 80, 89, 255));
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
      ->setCornerRadius(0)
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
      ->setWidth(kDifficultyColumnWidth)
      ->setHeight(84)
      ->setFlexShrink(0)
      ->setGap(6);

  levelView->setAlign(TextView::TextAlign::RIGHT);
  levelView->setName("chartListDifficulty");
  levelView->setVAlign(TextView::TextVAlign::MIDDLE);
  levelView->setOverflow(TextView::TextOverflow::Marquee);
  levelView->setWidth(kDifficultyColumnWidth)->setHeight(32);
  detailsLayout->addView(levelView);

  keyModeView->setAlign(TextView::TextAlign::RIGHT);
  keyModeView->setName("chartListKeyMode");
  keyModeView->setVAlign(TextView::TextVAlign::MIDDLE);
  keyModeView->setOverflow(TextView::TextOverflow::Hidden);
  keyModeView->setWidth(kDifficultyColumnWidth)->setHeight(24);
  detailsLayout->addView(keyModeView);

  scoreRankColumn->setName("chartListScoreRank");
  scoreRankColumn->setWidth(96)
      ->setHeight(72)
      ->setFlexShrink(0)
      ->setAlignItems(YGAlignStretch)
      ->setJustifyContent(YGJustifyCenter)
      ->setCornerRadius(0)
      ->setBorderWidth(0);
  scoreRankColumn->setDisplay(YGDisplayNone);
  scoreRankColumn->setVisible(false);
  scoreRankShadowView->setPositionType(YGPositionTypeAbsolute);
  scoreRankShadowView->setPosition(Edge::Left, 1);
  scoreRankShadowView->setPosition(Edge::Top, 2);
  scoreRankShadowView->setAlign(TextView::TextAlign::CENTER);
  scoreRankShadowView->setVAlign(TextView::TextVAlign::MIDDLE);
  scoreRankShadowView->setOverflow(TextView::TextOverflow::Hidden);
  scoreRankShadowView->setWidth(96)->setHeight(44);
  scoreRankWeightView->setPositionType(YGPositionTypeAbsolute);
  scoreRankWeightView->setPosition(Edge::Left, 1);
  scoreRankWeightView->setPosition(Edge::Top, 0);
  scoreRankWeightView->setAlign(TextView::TextAlign::CENTER);
  scoreRankWeightView->setVAlign(TextView::TextVAlign::MIDDLE);
  scoreRankWeightView->setOverflow(TextView::TextOverflow::Hidden);
  scoreRankWeightView->setWidth(96)->setHeight(44);
  scoreRankView->setAlign(TextView::TextAlign::CENTER);
  scoreRankView->setVAlign(TextView::TextVAlign::MIDDLE);
  scoreRankView->setOverflow(TextView::TextOverflow::Hidden);
  scoreRankView->setWidth(96)->setHeight(44);
  bestScoreView->setName("chartListBestScore");
  bestScoreView->setHeight(24)->setFlexShrink(0);
  bestScoreView->setAlign(TextView::CENTER);
  bestScoreView->setVAlign(TextView::MIDDLE);
  bestScoreView->setOverflow(TextView::TextOverflow::Hidden);
  bestScoreView->setThemedColor(ui_theme::textPrimary);
  scoreRankShadowView->setVisible(false);
  scoreRankWeightView->setVisible(false);
  scoreRankColumn->addView(scoreRankShadowView);
  scoreRankColumn->addView(scoreRankWeightView);
  scoreRankColumn->addView(scoreRankView);
  scoreRankColumn->addView(bestScoreView);
  auto *scoreDetails = new View();
  scoreDetails->setFlexDirection(FlexDirection::Row)
      ->setAlignItems(YGAlignCenter)
      ->setFlexShrink(0)
      ->setGap(6);
  scoreDetails->addView(scoreRankColumn);
  scoreDetails->addView(detailsLayout);
  contentCard->addView(scoreDetails);

  favoriteButton->setWidth(56)
      ->setHeight(56)
      ->setFlexShrink(0)
      ->setCornerRadius(0);
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
    if (currentRecord.courseStart || solidArchive || unavailable ||
        currentRecord.meta.BmsPath.empty()) {
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

void ChartListItemView::setMeta(const ChartMetaRecord &record,
                                bool prioritizeArtwork) {
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
  scoreRank.clear();
  scoreRankShadowView->setText("");
  scoreRankWeightView->setText("");
  scoreRankView->setText("");
  bestScoreView->setText("");
  scoreRankColumn->setDisplay(YGDisplayNone);
  scoreRankColumn->setVisible(false);
  if (record.courseStart) {
    levelView->setLocalizedText(record.difficultyTableLabels.empty()
                           ? i18n::message("library.chart.course.label")
                           : record.difficultyTableLabels);
    keyModeView->setLocalizedText(i18n::message("library.chart.course.badge"));
  } else if (solidArchive) {
    levelView->setLocalizedText(record.difficultyTableLabels.empty()
                           ? i18n::message("library.chart.unzip_required.label")
                           : record.difficultyTableLabels);
    keyModeView->setLocalizedText(i18n::message("library.chart.archive.badge"));
  } else {
    levelView->setText(record.difficultyTableLabels.empty()
                           ? formatPlayLevel(meta.PlayLevel)
                           : record.difficultyTableLabels);
    keyModeView->setLocalizedText(unavailable ? i18n::message("library.chart.missing.badge")
                                     : keyModeDescription(meta));
  }
  if (!unavailable && !solidArchive && !meta.StageFile.empty()) {
    artworkFrame->setDisplay(YGDisplayFlex);
    jacketImage->setImageAsync(meta.Folder / meta.StageFile,
                               prioritizeArtwork);
  } else {
    artworkFrame->setDisplay(YGDisplayNone);
    jacketImage->freeImage();
  }
  if (!record.courseStart && !unavailable && !solidArchive &&
      !meta.Banner.empty()) {
    bannerImage->setImageAsync(meta.Folder / meta.Banner,
                               prioritizeArtwork);
  } else {
    bannerImage->freeImage();
  }
  favoriteButton->setVisible(!record.courseStart && !unavailable &&
                             !solidArchive &&
                             !meta.BmsPath.empty());
  refreshFavoriteButton();
}

void ChartListItemView::setClearRank(int clearRank) {
  if (!solidArchive && hasClearLampColor(clearRank)) {
    clearLamp->setBorderWidth(1);
    clearLamp->setBackgroundColor(clearLampColorForRank(clearRank));
  } else {
    clearLamp->setBorderWidth(0);
    clearLamp->clearBackgroundColor();
  }
}

void ChartListItemView::setBestScoreRank(int score, int maxScore) {
  if (currentRecord.courseStart || solidArchive || unavailable ||
      maxScore <= 0 || score < 0) {
    scoreRankShadowView->setText("");
    scoreRankWeightView->setText("");
    scoreRankView->setText("");
    bestScoreView->setText("");
    scoreRank.clear();
    scoreRankColumn->setDisplay(YGDisplayNone);
    scoreRankColumn->setVisible(false);
    return;
  }

  scoreRank = score_rank::labelForScore(score, maxScore);
  const std::string displayRank = score_rank::displayLabel(scoreRank);
  scoreRankShadowView->setText(displayRank);
  scoreRankWeightView->setText(displayRank);
  scoreRankView->setText(displayRank);
  std::ostringstream rate;
  rate.imbue(std::locale::classic());
  rate << std::fixed << std::setprecision(2)
       << 100.0 * static_cast<double>(score) / maxScore << '%';
  bestScoreView->setText(rate.str());
  const bool visible = !scoreRank.empty();
  scoreRankColumn->setDisplay(visible ? YGDisplayFlex : YGDisplayNone);
  scoreRankColumn->setVisible(visible);
  refreshScoreRankColor();
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
  if (!favoriteIconView) return;
  favoriteIconView->setThemedColor(favorite
      ? (selected ? main_menu_theme::selectedAccent : main_menu_theme::accent)
      : (selected ? main_menu_theme::selectedSecondary : main_menu_theme::muted));
}

void ChartListItemView::refreshScoreRankColor() {
  if (!scoreRankView) return;
  scoreRankView->setThemedColor(selected ? main_menu_theme::onSelected
                                        : main_menu_theme::ink);
  bestScoreView->setThemedColor(selected ? main_menu_theme::selectedSecondary
                                        : main_menu_theme::secondary);
  scoreRankColumn->clearBackgroundColor();
  scoreRankColumn->setBorderWidth(0);
}

void ChartListItemView::onSelected() {
  selected = true;
  contentCard->setThemedBackgroundColor(main_menu_theme::selected);
  contentCard->setCornerRadius(0);
  contentCard->setThemedBorderColor(main_menu_theme::selected);
  contentCard->setBorderWidth(1);
  artworkFrame->setThemedBackgroundColor(main_menu_theme::selected);
  artworkFrame->setThemedBorderColor(main_menu_theme::selected);
  // A recycled banner keeps its identity, but cannot compromise selection contrast.
  bannerImage->setVisible(false);
  applyTextColors(true);
  refreshFavoriteButton();
}

void ChartListItemView::onUnselected() {
  selected = false;
  contentCard->setThemedBackgroundColor(main_menu_theme::canvas);
  contentCard->setCornerRadius(0);
  contentCard->setThemedBorderColor(main_menu_theme::canvas);
  contentCard->setBorderWidth(1);
  artworkFrame->setThemedBackgroundColor(main_menu_theme::surface);
  artworkFrame->setThemedBorderColor(main_menu_theme::surface);
  bannerImage->setVisible(true);
  applyTextColors(false);
  refreshFavoriteButton();
}

void ChartListItemView::applyTextColors(bool selected) {
  titleView->setThemedColor(selected ? main_menu_theme::onSelected : main_menu_theme::ink);
  artistView->setThemedColor(selected ? main_menu_theme::selectedSecondary : main_menu_theme::secondary);
  levelView->setThemedColor(selected ? main_menu_theme::selectedAccent : main_menu_theme::accent);
  keyModeView->setThemedColor(selected ? main_menu_theme::selectedSecondary : main_menu_theme::muted);
  if (!selected && unavailable) {
    titleView->setThemedColor(main_menu_theme::error);
  } else if (!selected && solidArchive) {
    titleView->setThemedColor(main_menu_theme::warning);
  }
  refreshScoreRankColor();
}
