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
constexpr int kArtworkWidth = 72;
constexpr int kRowPadding = 12;
constexpr int kColumnGap = 12;
constexpr int kTitleMinimumWidth = 120;
constexpr int kDifficultyColumnWidth = 86;
constexpr int kBestWidth = 86;
constexpr int kCompactBestWidth = 76;
constexpr int kFavoriteWidth = 40;
constexpr const char *kUiFont = main_menu_theme::font;

TextView *rowText(const char *name, int size, int height,
                  TextView::FontWeight weight = TextView::FontWeight::Regular) {
  auto *view = new TextView(kUiFont, size, weight);
  view->setName(name);
  view->setHeight(height)->setMinWidth(0)->setFlexShrink(0);
  view->setVAlign(TextView::MIDDLE);
  view->setOverflow(TextView::TextOverflow::Marquee);
  view->setThemedColor(main_menu_theme::ink);
  return view;
}

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
  View::LayoutBatchScope layoutBatch;
  (void)record;
  setFlexDirection(FlexDirection::Column)->setAlignItems(YGAlignStretch);
  contentCard = new View();
  contentCard->setName("chartListContentCard");
  contentCard->setFlexDirection(FlexDirection::Row)->setAlignItems(YGAlignCenter);
  contentCard->setHeight(height - 1)->setFlexShrink(0);
  contentCard->setPadding(Edge::Left, kRowPadding)->setPadding(Edge::Right, kRowPadding)->setGap(kColumnGap);
  addView(contentCard);
  auto *rule = new View();
  rule->setHeight(1)->setFlexShrink(0)->setThemedBackgroundColor(main_menu_theme::rule);
  addView(rule);
  clearLamp = new View();
  clearLamp->setName("chartListClearLamp");
  clearLamp->setPositionType(YGPositionTypeAbsolute)->setPosition(Edge::Left, 0);
  clearLamp->setPosition(Edge::Top, 8)->setWidth(8)->setHeight(height - 17);
  clearLamp->setBorderWidth(0);
  contentCard->addView(clearLamp);

  artworkFrame = new View();
  artworkFrame->setName("chartListArtwork");
  artworkFrame->setWidth(kArtworkWidth)->setHeight(kArtworkWidth)->setFlexShrink(0);
  artworkFrame->setThemedBackgroundColor(main_menu_theme::surface);
  artworkPlaceholder = new TextView(ui_icons::kFontAwesomeSolidPath, 24);
  artworkPlaceholder->setText(ui_icons::textForCodepoint(ui_icons::kMusic));
  artworkPlaceholder->setAlign(TextView::CENTER);
  artworkPlaceholder->setVAlign(TextView::MIDDLE);
  artworkPlaceholder->setWidth(kArtworkWidth)->setHeight(kArtworkWidth);
  artworkPlaceholder->setThemedColor(main_menu_theme::muted);
  artworkFrame->addView(artworkPlaceholder);
  jacketImage = new ImageView(0, 0, kArtworkWidth, kArtworkWidth);
  jacketImage->setName("chartListJacket");
  jacketImage->setPositionType(YGPositionTypeAbsolute);
  jacketImage->setPosition(Edge::Left, 0)->setPosition(Edge::Top, 0);
  jacketImage->setContentMode(ImageView::ContentMode::Contain);
  artworkFrame->addView(jacketImage);
  contentCard->addView(artworkFrame);

  textLayout = new View();
  textLayout->setFlex(1)->setMinWidth(0)->setJustifyContent(YGJustifyCenter)->setGap(4);
  titleView = rowText("chartListTitle", 28, 40, TextView::FontWeight::Bold);
  textLayout->addView(titleView);
  contentCard->addView(textLayout);
  artistView = rowText("chartListArtist", 21, 32);
  textLayout->addView(artistView);

  detailsLayout = new View();
  detailsLayout->setWidth(kDifficultyColumnWidth)->setFlexShrink(0);
  detailsLayout->setAlignItems(YGAlignStretch)->setGap(3);
  levelView = rowText("chartListDifficulty", 27, 44, TextView::FontWeight::Bold);
  levelView->setAlign(TextView::RIGHT);
  levelView->setThemedBackgroundColor([] { return main_menu_theme::light() ? main_menu_theme::action() : Color(0, 0, 0, 0); });
  levelView->setThemedColor([] { return main_menu_theme::light() ? main_menu_theme::onAction() : main_menu_theme::accent(); });
  detailsLayout->addView(levelView);
  keyModeView = rowText("chartListKeyMode", 17, 24, TextView::FontWeight::Bold);
  keyModeView->setAlign(TextView::RIGHT);
  detailsLayout->addView(keyModeView);
  contentCard->addView(detailsLayout);

  scoreRankColumn = new View();
  scoreRankColumn->setName("chartListScoreRank");
  scoreRankColumn->setWidth(kBestWidth)->setHeight(76)->setFlexShrink(0);
  scoreRankColumn->setJustifyContent(YGJustifyCenter);
  scoreRankView = rowText("chartListGrade", 34, 44, TextView::FontWeight::Bold);
  bestScoreView = rowText("chartListBestScore", 18, 25);
  scoreRankColumn->addView(scoreRankView);
  scoreRankColumn->addView(bestScoreView);
  contentCard->addView(scoreRankColumn);

  favoriteButton = new Button();
  favoriteButton->setName("chartListFavorite");
  favoriteButton->setWidth(kFavoriteWidth)->setHeight(56)->setFlexShrink(0);
  favoriteButton->setThemedBackgroundColors(
      [] { return Color(0, 0, 0, 0); },
      [this] {
        auto color = selected ? main_menu_theme::onSelected() : main_menu_theme::ink();
        color.a = 18;
        return color;
      },
      [this] {
        auto color = selected ? main_menu_theme::onSelected() : main_menu_theme::ink();
        color.a = 34;
        return color;
      });
  favoriteButton->setStyledBorderWidth(0);
  favoriteIconView = new TextView(ui_icons::kFontAwesomeSolidPath, 25);
  favoriteIconView->setText(ui_icons::textForCodepoint(0xf005));
  favoriteIconView->setAlign(TextView::CENTER);
  favoriteIconView->setVAlign(TextView::MIDDLE);
  auto *favoriteContent = new View();
  favoriteContent->setAlignItems(YGAlignCenter)->setJustifyContent(YGJustifyCenter);
  favoriteIconView->setWidth(36)->setHeight(40);
  favoriteOutlineView = new TextView("assets/fonts/fa-regular-400.ttf", 25);
  favoriteOutlineView->setText(ui_icons::textForCodepoint(0xf005));
  favoriteOutlineView->setWidth(36)->setHeight(40);
  favoriteOutlineView->setAlign(TextView::CENTER);
  favoriteOutlineView->setVAlign(TextView::MIDDLE);
  favoriteContent->addView(favoriteIconView);
  favoriteContent->addView(favoriteOutlineView);
  favoriteButton->setContentView(favoriteContent);
  favoriteButton->setOnClickListener([this]() {
    if (currentRecord.courseStart || solidArchive || unavailable ||
        currentRecord.meta.BmsPath.empty()) return;
    const bool nextFavorite = !favorite;
    const auto toggledPath = currentRecord.meta.BmsPath;
    if (!favoriteToggleHandler || favoriteToggleHandler(currentRecord, nextFavorite)) {
      if (currentRecord.meta.BmsPath == toggledPath) setFavoriteState(nextFavorite);
    }
  });
  contentCard->addView(favoriteButton);
  onUnselected();
  applyYogaLayout();
}

void ChartListItemView::onLayout() {
  View::onLayout();
  const bool compact = getWidth() < 840;
  View::LayoutBatchScope batch;
  if (compact != compact_) {
    compact_ = compact;
    scoreRankColumn->setWidth(compact ? kCompactBestWidth : kBestWidth);
  }
  refreshColumnWidths();
}

void ChartListItemView::refreshColumnWidths() {
  const int availableWidth = std::max(0, getWidth() - 2 * kRowPadding -
      4 * kColumnGap - kArtworkWidth - kFavoriteWidth -
      (compact_ ? kCompactBestWidth : kBestWidth));
  const int titleMinimum = std::min(kTitleMinimumWidth, availableWidth / 2);
  const int difficultyWidth = std::min(desiredDifficultyWidth_, availableWidth - titleMinimum);
  if (difficultyWidth == appliedDifficultyWidth_ && titleMinimum == appliedTitleMinimumWidth_) return;
  appliedDifficultyWidth_ = difficultyWidth;
  appliedTitleMinimumWidth_ = titleMinimum;
  View::LayoutBatchScope batch;
  detailsLayout->setWidth(difficultyWidth);
  textLayout->setMinWidth(titleMinimum);
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
  scoreRankView->setText("");
  bestScoreView->setText("");
  scoreRankView->setText("—");
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
  desiredDifficultyWidth_ = std::max({kDifficultyColumnWidth,
      levelView->measureTextWidth(levelView->getText()) + 12,
      keyModeView->measureTextWidth(keyModeView->getText())});
  refreshColumnWidths();
  const auto artwork = !meta.StageFile.empty() ? meta.StageFile : meta.Banner;
  const bool hasArtwork = !record.courseStart && !unavailable && !solidArchive && !artwork.empty();
  artworkPlaceholder->setVisible(!hasArtwork);
  if (hasArtwork) jacketImage->setImageAsync(meta.Folder / artwork, prioritizeArtwork);
  else jacketImage->freeImage();
  favoriteButton->setVisible(!record.courseStart && !unavailable &&
                             !solidArchive &&
                             !meta.BmsPath.empty());
  refreshFavoriteButton();
}

void ChartListItemView::setClearRank(int clearRank) {
  if (!solidArchive && hasClearLampColor(clearRank)) {
    auto glow = clearLampColorForRank(clearRank);
    clearLamp->setBackgroundColor(glow);
    glow.a = 150;
    clearLamp->setShadow(glow, 0, 0, 8);
  } else {
    clearLamp->clearShadow();
    clearLamp->clearBackgroundColor();
  }
}

void ChartListItemView::setBestScoreRank(int score, int maxScore) {
  if (currentRecord.courseStart || solidArchive || unavailable || maxScore <= 0 || score < 0) {
    scoreRankView->setText("—");
    bestScoreView->setText("");
    scoreRank.clear();
    return;
  }
  scoreRank = score_rank::labelForScore(score, maxScore);
  scoreRankView->setText(score_rank::displayLabel(scoreRank));
  std::ostringstream rate;
  rate.imbue(std::locale::classic());
  rate << std::fixed << std::setprecision(2) << 100.0 * score / maxScore << '%';
  bestScoreView->setText(rate.str());
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
  favoriteButton->propagateThemeChange();
  favoriteIconView->setVisible(favorite);
  favoriteIconView->setDisplay(favorite ? YGDisplayFlex : YGDisplayNone);
  favoriteOutlineView->setVisible(!favorite);
  favoriteOutlineView->setDisplay(favorite ? YGDisplayNone : YGDisplayFlex);
  favoriteOutlineView->setThemedColor(selected ? main_menu_theme::onSelected : main_menu_theme::secondary);
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
  applyTextColors(true);
  refreshFavoriteButton();
}

void ChartListItemView::onUnselected() {
  selected = false;
  contentCard->setThemedBackgroundColor(main_menu_theme::canvas);
  applyTextColors(false);
  refreshFavoriteButton();
}

void ChartListItemView::applyTextColors(bool selected) {
  levelView->setThemedColor([selected] {
    return main_menu_theme::light() ? main_menu_theme::onAction() :
        selected ? main_menu_theme::selectedAccent() : main_menu_theme::accent();
  });
  titleView->setOverflow(selected ? TextView::TextOverflow::Marquee : TextView::TextOverflow::Hidden);
  artistView->setOverflow(selected ? TextView::TextOverflow::Marquee : TextView::TextOverflow::Hidden);
  titleView->setThemedColor(selected ? main_menu_theme::onSelected : main_menu_theme::ink);
  artistView->setThemedColor(selected ? main_menu_theme::selectedSecondary : main_menu_theme::secondary);
  keyModeView->setThemedColor(selected ? main_menu_theme::selectedSecondary : main_menu_theme::muted);
  if (!selected && unavailable) titleView->setThemedColor(main_menu_theme::error);
  else if (!selected && solidArchive) titleView->setThemedColor(main_menu_theme::warning);
  refreshScoreRankColor();
}
