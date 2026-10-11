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
constexpr int kRowNumberWidth = 36;
constexpr int kArtworkWidth = 72;
constexpr int kArtistWidth = 140;
constexpr int kDifficultyColumnWidth = 86;
constexpr int kClearWidth = 104;
constexpr int kBestWidth = 86;
constexpr int kFavoriteWidth = 40;
constexpr const char *kUiFont = "assets/fonts/notosanscjkjp.ttf";

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

View *ChartListItemView::createColumnHeader() {
  auto *header = new View();
  header->setName("chartListColumnHeader");
  header->setFlexDirection(FlexDirection::Row)->setAlignItems(YGAlignCenter);
  header->setHeight(38)->setFlexShrink(0)->setPadding(Edge::Left, 12)->setPadding(Edge::Right, 12)->setGap(12);
  auto cell = [header](const char *key, int width, bool grow = false) {
    auto *text = rowText("", 16, 30);
    if (*key) text->setLocalizedText(i18n::message(key));
    text->setThemedColor(main_menu_theme::muted);
    if (grow) text->setFlex(1);
    else text->setWidth(width);
    header->addView(text);
    return text;
  };
  cell("", kRowNumberWidth)->setText("#");
  cell("", kArtworkWidth);
  cell("library.sort.title.label", 0, true);
  cell("music_player.artist.label", kArtistWidth);
  cell("library.sort.difficulty.label", kDifficultyColumnWidth);
  cell("library.filter_sort.clear_mark.label", kClearWidth);
  cell("menu.selector.best.label", kBestWidth);
  cell("", kFavoriteWidth);
  return header;
}

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
  contentCard->setPadding(Edge::Left, 12)->setPadding(Edge::Right, 12)->setGap(12);
  addView(contentCard);
  auto *rule = new View();
  rule->setHeight(1)->setFlexShrink(0)->setThemedBackgroundColor(main_menu_theme::rule);
  addView(rule);
  selectedRail = new View();
  selectedRail->setPositionType(YGPositionTypeAbsolute)->setPosition(Edge::Left, 0);
  selectedRail->setPosition(Edge::Top, 0)->setWidth(7)->setHeight(height - 1);
  selectedRail->setThemedBackgroundColor(main_menu_theme::action);
  contentCard->addView(selectedRail);

  rowNumber = rowText("chartListNumber", 18, 30);
  rowNumber->setWidth(kRowNumberWidth);
  contentCard->addView(rowNumber);
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
  artistView->setWidth(kArtistWidth);
  contentCard->addView(artistView);

  detailsLayout = new View();
  detailsLayout->setWidth(kDifficultyColumnWidth)->setFlexShrink(0);
  detailsLayout->setAlignItems(YGAlignCenter)->setGap(3);
  levelView = rowText("chartListDifficulty", 27, 44, TextView::FontWeight::Bold);
  levelView->setWidth(kDifficultyColumnWidth - 12);
  levelView->setAlign(TextView::CENTER);
  levelView->setThemedBackgroundColor(main_menu_theme::action);
  levelView->setThemedColor(main_menu_theme::onAction);
  detailsLayout->addView(levelView);
  keyModeView = rowText("chartListKeyMode", 17, 24, TextView::FontWeight::Bold);
  keyModeView->setWidth(kDifficultyColumnWidth);
  keyModeView->setAlign(TextView::CENTER);
  detailsLayout->addView(keyModeView);
  contentCard->addView(detailsLayout);

  clearColumn = new View();
  clearColumn->setWidth(kClearWidth)->setFlexShrink(0)->setGap(6);
  clearLabel = rowText("chartListClearLabel", 16, 28);
  clearLabel->setAutoFitText(true);
  clearColumn->addView(clearLabel);
  clearLamp = new View();
  clearLamp->setName("chartListClearLamp");
  clearLamp->setWidth(48)->setHeight(7)->setFlexShrink(0);
  clearLamp->setBorderColor(Color(76, 80, 89, 255));
  clearColumn->addView(clearLamp);
  contentCard->addView(clearColumn);

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
  favoriteButton->setWidth(kFavoriteWidth)->setHeight(56)->setFlexShrink(0);
  favoriteButton->setThemedBackgroundColors(
      [] { return Color(0, 0, 0, 0); }, main_menu_theme::hover, main_menu_theme::pressed);
  favoriteButton->setStyledBorderWidth(0);
  favoriteIconView = new TextView(ui_icons::kFontAwesomeSolidPath, 25);
  favoriteIconView->setText(ui_icons::textForCodepoint(0xf005));
  favoriteIconView->setAlign(TextView::CENTER);
  favoriteIconView->setVAlign(TextView::MIDDLE);
  favoriteButton->setContentView(favoriteIconView);
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

void ChartListItemView::setDisplayIndex(int index) {
  std::ostringstream number;
  number << std::setw(2) << std::setfill('0') << index + 1;
  rowNumber->setText(number.str());
}

void ChartListItemView::onLayout() {
  View::onLayout();
  const bool compact = getWidth() < 840;
  if (compact == compact_) return;
  compact_ = compact;
  View::LayoutBatchScope batch;
  rowNumber->setDisplay(compact ? YGDisplayNone : YGDisplayFlex);
  rowNumber->setVisible(!compact);
  artistView->moveTo(compact ? *textLayout : *contentCard);
  if (!compact) {
    // Reappend trailing columns so Artist returns immediately after Title.
    for (auto *view : {detailsLayout, clearColumn, scoreRankColumn, static_cast<View *>(favoriteButton)}) {
      view->moveTo(*textLayout);
      view->moveTo(*contentCard);
    }
  }
  artistView->setWidth(compact ? YGUndefined : kArtistWidth);
  clearColumn->setWidth(compact ? 18 : kClearWidth);
  clearLabel->setDisplay(compact ? YGDisplayNone : YGDisplayFlex);
  clearLabel->setVisible(!compact);
  clearLamp->setWidth(compact ? 8 : 48)->setHeight(compact ? 36 : 7);
  scoreRankColumn->setWidth(compact ? 76 : kBestWidth);
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
  clearLabel->setText(clearRank == kNoClearTypeRank ? "—" : clearTypeRankToLabel(clearRank));
  if (!solidArchive && hasClearLampColor(clearRank)) {
    clearLamp->setBorderWidth(1);
    clearLamp->setBackgroundColor(clearLampColorForRank(clearRank));
  } else {
    clearLamp->setBorderWidth(0);
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
  selectedRail->setVisible(true);
  applyTextColors(true);
  refreshFavoriteButton();
}

void ChartListItemView::onUnselected() {
  selected = false;
  contentCard->setThemedBackgroundColor(main_menu_theme::canvas);
  selectedRail->setVisible(false);
  applyTextColors(false);
  refreshFavoriteButton();
}

void ChartListItemView::applyTextColors(bool selected) {
  titleView->setOverflow(selected ? TextView::TextOverflow::Marquee : TextView::TextOverflow::Hidden);
  artistView->setOverflow(selected ? TextView::TextOverflow::Marquee : TextView::TextOverflow::Hidden);
  titleView->setThemedColor(selected ? main_menu_theme::onSelected : main_menu_theme::ink);
  artistView->setThemedColor(selected ? main_menu_theme::selectedSecondary : main_menu_theme::secondary);
  rowNumber->setThemedColor(selected ? main_menu_theme::selectedSecondary : main_menu_theme::secondary);
  clearLabel->setThemedColor(selected ? main_menu_theme::onSelected : main_menu_theme::ink);
  keyModeView->setThemedColor(selected ? main_menu_theme::selectedSecondary : main_menu_theme::muted);
  if (!selected && unavailable) titleView->setThemedColor(main_menu_theme::error);
  else if (!selected && solidArchive) titleView->setThemedColor(main_menu_theme::warning);
  refreshScoreRankColor();
}
