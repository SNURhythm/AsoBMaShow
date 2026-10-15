#include "../rendering/ResponsiveUi.h"
#include "../GameplayKeyMode.h"
#include "ChartDetailsView.h"

#include "ClearLampColors.h"
#include "ImageView.h"
#include "IconText.h"
#include "MainMenuTheme.h"
#include "TextView.h"
#include "UiTheme.h"
#include "../ResultContracts.h"
#include "../ScoreRankUtils.h"

#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>

namespace {
constexpr const char *kFont = main_menu_theme::font;

void updateClearLamp(TextView &badge, int rank) {
  if (!hasClearLampColor(rank)) {
    badge.clearBackgroundColor();
    badge.clearShadow();
    badge.setThemedColor(main_menu_theme::ink);
    return;
  }
  auto color = clearLampColorForRank(rank);
  color.a = 255;
  badge.setBackgroundColor(color);
  const auto linear = [](uint8_t channel) {
    const double value = channel / 255.0;
    return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
  };
  const double luminance = 0.2126 * linear(color.r) + 0.7152 * linear(color.g) +
                           0.0722 * linear(color.b);
  badge.setColor(luminance > 0.179 ? SDL_Color{0, 0, 0, 255} : SDL_Color{255, 255, 255, 255});
  if (rank == kClearTypeHardClearRank) color = Color(140, 185, 255, 255);
  color.a = 160;
  badge.setShadow(color, 0, 0, 10);
}

class ArtworkPlaceholder final : public TextView {
public:
  explicit ArtworkPlaceholder(ImageView &artwork)
      : TextView(ui_icons::kFontAwesomeSolidPath, 56), artwork_(artwork) {}

protected:
  void renderImpl(RenderContext &context) override {
    // The image renders first, so an async decode can replace the placeholder
    // in the same frame. Missing and failed images keep the glyph visible.
    if (!artwork_.getVisible() || artwork_.imageWidth() <= 0 ||
        artwork_.imageHeight() <= 0) {
      TextView::renderImpl(context);
    }
  }

private:
  ImageView &artwork_;
};

TextView *label(const char *name, int size, int height,
                View::ThemeColorProvider color = main_menu_theme::ink,
                TextView::FontWeight weight = TextView::FontWeight::Regular) {
  auto *text = new TextView(kFont, size, weight);
  text->setName(name);
  text->setHeight(height)->setMinWidth(0)->setFlexShrink(0);
  text->setVAlign(TextView::MIDDLE);
  text->setOverflow(TextView::TextOverflow::Hidden);
  text->setThemedColor(std::move(color));
  return text;
}

void visible(View *view, bool show) {
  view->setVisible(show);
  view->setDisplay(show ? YGDisplayFlex : YGDisplayNone);
}

std::string number(double value) {
  if (!std::isfinite(value) || value < 0) return "—";
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::fixed << std::setprecision(2) << value;
  std::string text = out.str();
  while (text.back() == '0') text.pop_back();
  if (text.back() == '.') text.pop_back();
  return text;
}

std::string bpmRange(const bms_parser::ChartMeta &meta) {
  if (std::isfinite(meta.MinBpm) && std::isfinite(meta.MaxBpm) &&
      meta.MinBpm > 0 && meta.MaxBpm >= meta.MinBpm) {
    const auto min = number(meta.MinBpm);
    const auto max = number(meta.MaxBpm);
    return min == max ? min : min + "–" + max;
  }
  return meta.Bpm > 0 ? number(meta.Bpm) : "—";
}

std::string judgeRank(const bms_parser::ChartMeta &meta) {
  if (meta.RankType == bms_parser::JudgeRankType::DefExRank) {
    return meta.Rank > 0 ? std::to_string(meta.Rank) + "%" : "—";
  }
  constexpr const char *names[] = {"VERY HARD", "HARD", "NORMAL", "EASY",
                                  "VERY EASY"};
  return meta.Rank >= 0 && meta.Rank <= 4 ? names[meta.Rank] : "—";
}

std::string duration(long long micros) {
  if (micros <= 0) return "—";
  const auto seconds = micros / 1000000;
  std::ostringstream out;
  out << seconds / 60 << ':' << std::setw(2) << std::setfill('0') << seconds % 60;
  return out.str();
}

std::string nextGrade(int score, int maxScore) {
  // MAX- shares AAA's displayed grade; do not offer another AAA as the target.
  constexpr const char *grades[] = {"E", "D", "C", "B", "A", "AA", "AAA", "MAX"};
  for (int i = 0; i < 8; ++i) {
    const int target = score_rank::targetScoreForFraction(maxScore, i + 2, 9);
    if (score < target) {
      return std::string(grades[i]) + " −" + std::to_string(target - score);
    }
  }
  return "MAX";
}
} // namespace

ChartDetailsView::ChartDetailsView(ImageView *artwork) {
  setName("chartDetails");
  setFlexDirection(FlexDirection::Column)->setAlignItems(YGAlignStretch);
  setFlexShrink(0)->setMinWidth(0)->setGap(kSectionGap);

  auto *identity = new View();
  identity_ = identity;
  artwork_ = artwork;
  identity->setFlexDirection(FlexDirection::Row)->setGap(14)->setHeight(kIdentityHeight);
  identity->setAlignItems(YGAlignCenter)->setFlexShrink(0);
  artworkFrame_ = new View();
  artworkFrame_->setWidth(112)->setHeight(112)->setFlexShrink(0);
  artworkFrame_->setName("chartDetailsArtwork");
  artworkFrame_->setPadding(Edge::All, 0);
  artworkFrame_->setCornerRadius(0);
  artworkFrame_->setThemedBackgroundColor(main_menu_theme::surface);
  artwork->setWidthPercent(100)->setHeight(112);
  artwork->setPositionType(YGPositionTypeAbsolute);
  artwork->setPosition(Edge::Left, 0)->setPosition(Edge::Top, 0);
  artwork->setCornerRadius(0);
  artwork->setContentMode(ImageView::ContentMode::Contain);
  artworkFrame_->addView(artwork);
  artworkPlaceholder_ = new ArtworkPlaceholder(*artwork);
  artworkPlaceholder_->setName("chartDetailsArtworkPlaceholder");
  artworkPlaceholder_->setText(ui_icons::textForCodepoint(ui_icons::kMusic));
  artworkPlaceholder_->setWidthPercent(100)->setHeight(112);
  artworkPlaceholder_->setAlign(TextView::CENTER);
  artworkPlaceholder_->setVAlign(TextView::MIDDLE);
  artworkPlaceholder_->setThemedColor(main_menu_theme::muted);
  artworkFrame_->addView(artworkPlaceholder_);
  identity->addView(artworkFrame_);

  auto *titles = new View();
  titles_ = titles;
  titles->setFlex(1)->setMinWidth(0)->setGap(2);
  title_ = label("chartDetailsTitle", 48, 88, main_menu_theme::ink,
                 TextView::FontWeight::Bold);
  title_->setWrap(false);
  artist_ = label("chartDetailsArtist", 28, 32, main_menu_theme::secondary);
  artist_->setOverflow(TextView::TextOverflow::Marquee);
  genre_ = label("chartDetailsGenre", 21, 26, main_menu_theme::muted);
  genre_->setOverflow(TextView::TextOverflow::Marquee);
  titleRow_ = new View();
  titleRow_->setFlexDirection(FlexDirection::Row)->setAlignItems(YGAlignCenter)->setGap(10);
  title_->setFlex(1)->setMinWidth(0);
  titleRow_->addView(title_);
  titles->addView(titleRow_);
  titles->addView(artist_);
  titles->addView(genre_);
  identity->addView(titles);
  addView(identity);

  difficulty_ = label("chartDetailsDifficulty", 28, kDifficultyHeight,
                      main_menu_theme::accent, TextView::FontWeight::Bold);
  difficulty_->setOverflow(TextView::TextOverflow::Marquee);
  addView(difficulty_);
  chartLevel_ = label("chartDetailsChartLevel", 20, 28, main_menu_theme::secondary);
  chartLevel_->setOverflow(TextView::TextOverflow::Marquee);
  addView(chartLevel_);

  facts_ = new View();
  facts_->setName("chartDetailsFacts");
  facts_->setFlexDirection(FlexDirection::Column)->setGap(kFactGap)->setFlexShrink(0);
  facts_->setFlexGrow(1);
  facts_->setPadding(Edge::All, kFactPadding);
  facts_->clearBackgroundColor();

  size_t rowIndex = 0;
  auto row = [this, &rowIndex]() {
    auto *view = new View();
    view->setFlexDirection(FlexDirection::Row)->setGap(14);
    view->setHeight(kFactRowHeight)->setMinHeight(kFactRowHeight)->setFlexShrink(0);
    view->setFlexGrow(0);
    facts_->addView(view);
    factRows_[rowIndex++] = view;
    return view;
  };
  size_t cellIndex = 0;
  auto metric = [this, &cellIndex](View *row, const char *key, const char *name,
                   TextView **caption = nullptr) {
    auto *cell = new View();
    factCells_[cellIndex++] = cell;
    cell->setFlex(1)->setMinWidth(0)->setJustifyContent(YGJustifyFlexStart);
    auto *heading = label("", 20, 26, main_menu_theme::secondary);
    heading->setLocalizedText(i18n::message(key));
    auto *value = label(name, 28, 34);
    value->setOverflow(TextView::TextOverflow::Marquee);
    cell->addView(heading);
    cell->addView(value);
    row->addView(cell);
    if (caption) *caption = heading;
    return value;
  };
  auto *tempoRow = row();
  bpm_ = metric(tempoRow, "menu.details.bpm.label", "chartDetailsBpm");
  judge_ = metric(tempoRow, "menu.details.judge.label", "chartDetailsJudge");
  auto *lengthRow = row();
  length_ = metric(lengthRow, "menu.details.length.label", "chartDetailsLength");
  notes_ = metric(lengthRow, "menu.details.notes.label", "chartDetailsNotes");
  auto *totalRow = row();
  total_ = metric(totalRow, "menu.details.total.label", "chartDetailsTotal", &totalLabel_);
  noteTypes_ = metric(totalRow, "menu.details.note_types.label", "chartDetailsNoteTypes");
  addView(facts_);

  personalBest_ = new View();
  personalBest_->setName("chartDetailsPersonalBest");
  personalBest_->setFlexDirection(FlexDirection::Column)->setGap(4)->setFlexShrink(0);
  personalBest_->setPadding(Edge::All, 14);
  personalBest_->clearBackgroundColor();
  auto *bestHeading = new View();
  bestHeading->setFlexDirection(FlexDirection::Row)->setGap(8)->setHeight(28);
  bestLabel_ = label("", 20, 26, main_menu_theme::secondary);
  bestLabel_->setFlex(1);
  bestHeading->addView(bestLabel_);
  next_ = label("chartDetailsNext", 22, 28, main_menu_theme::secondary);
  next_->setFlex(1)->setMinWidth(0);
  next_->setAlign(TextView::RIGHT);
  next_->setAutoFitText(true);
  bestHeading->addView(next_);
  personalBest_->addView(bestHeading);
  auto *scoreRow = new View();
  scoreRow->setFlexDirection(FlexDirection::Row)->setAlignItems(YGAlignCenter)->setGap(12);
  scoreRow->setHeight(52)->setFlexShrink(0);
  score_ = label("chartDetailsScore", 34, 52, main_menu_theme::ink,
                 TextView::FontWeight::Bold);
  score_->setFlex(1)->setMinWidth(0);
  score_->setAutoFitText(true);
  scoreRow->addView(score_);
  emptyScore_ = label("chartDetailsEmptyScore", 26, 52, main_menu_theme::secondary);
  emptyScore_->setFlex(1)->setMinWidth(0);
  emptyScore_->setLocalizedText(i18n::message("menu.details.unplayed.label"));
  scoreRow->addView(emptyScore_);
  clear_ = label("chartDetailsClear", 32, 52, main_menu_theme::ink,
                 TextView::FontWeight::Bold);
  clear_->setFlex(1)->setMinWidth(0)->setHeight(52);
  clear_->setAlign(TextView::RIGHT);
  clear_->setPadding(Edge::Left, 12)->setPadding(Edge::Right, 12);
  clear_->setVAlign(TextView::MIDDLE);
  clear_->setAutoFitText(true);
  scoreRow->addView(clear_);
  personalBest_->addView(scoreRow);
  rate_ = label("chartDetailsRate", 24, 32);
  personalBest_->addView(rate_);
  addView(personalBest_);
  summary_ = new View();
  summary_->setName("chartDetailsSummary");
  summary_->setFlexDirection(FlexDirection::Row)->setAlignItems(YGAlignCenter)->setGap(16);
  summary_->setHeight(96)->setFlexShrink(0)->setPadding(Edge::Top, 10);
  auto *summaryRule = new View();
  summaryRule->setPositionType(YGPositionTypeAbsolute)->setPosition(Edge::Top, 0);
  summaryRule->setWidthPercent(100)->setHeight(1)->setThemedBackgroundColor(main_menu_theme::secondary);
  summary_->addView(summaryRule);
  levelGroup_ = new View();
  levelGroup_->setFlex(1)->setMinWidth(0)->setGap(8);
  levelLine_ = new View();
  levelLine_->setFlexDirection(FlexDirection::Row)->setAlignItems(YGAlignCenter)->setGap(14)->setHeight(46);
  levelGroup_->addView(levelLine_);
  tempo_ = label("chartDetailsTempo", 23, 32, main_menu_theme::ink, TextView::FontWeight::Bold);
  tempo_->setOverflow(TextView::TextOverflow::Marquee);
  levelGroup_->addView(tempo_);
  summary_->addView(levelGroup_);
  auto *bestOverview = new View();
  bestOverview->setWidth(112)->setFlexShrink(0)->setGap(8);
  grade_ = label("chartDetailsGrade", 48, 46, main_menu_theme::ink, TextView::FontWeight::Bold);
  grade_->setAlign(TextView::RIGHT);
  grade_->setAutoFitText(true);
  bestOverview->addView(grade_);
  desktopRate_ = label("chartDetailsBestRate", 25, 32, main_menu_theme::ink, TextView::FontWeight::Bold);
  desktopRate_->setAlign(TextView::RIGHT);
  bestOverview->addView(desktopRate_);
  summary_->addView(bestOverview);
  addView(summary_);
  visible(summary_, false);
  scoreSlot_ = new View();
  scoreSlot_->setName("chartDetailsScoreSlot");
  scoreSlot_->setFlexDirection(FlexDirection::Column)->setFlexShrink(0);
  addView(scoreSlot_);
  visible(scoreSlot_, false);

  setChart(nullptr, std::nullopt, kNoClearTypeRank, "");
}

void ChartDetailsView::setScoreContainer(View *container) {
  scoreContainer_ = container;
  personalBest_->moveTo(container ? *container : compact_ ? *this : *scoreSlot_);
}

void ChartDetailsView::onLayout() {
  View::onLayout();
  refreshArtworkLayout();
}

void ChartDetailsView::renderImpl(RenderContext &) {
  // Keep the square frame stable as image data arrives and panel size changes.
  refreshArtworkLayout();
  const bool hasImage = artwork_->getVisible() && artwork_->imageWidth() > 0;
  if (hasImage != artworkHasImage_) {
    artworkHasImage_ = hasImage;
    if (hasImage) artworkFrame_->clearBackgroundColor();
    else artworkFrame_->setThemedBackgroundColor(main_menu_theme::surface);
  }
}

bool ChartDetailsView::canShowPreviewGlow() const {
  if (!artwork_->ambientPalette()) return false;
  for (auto node = artwork_->getNode(); node; node = YGNodeGetOwner(node)) {
    const auto *view = static_cast<const View *>(YGNodeGetContext(node));
    if (view && !view->getVisible()) return false;
  }
  return true;
}

void ChartDetailsView::updatePreviewGlow(float level, const audio::Spectrum &spectrum, float dt) {
  if (!canShowPreviewGlow()) {
    previewGlow_.reset();
    sampledSpectrum_.fill(0);
    spectrumCooldown_ = 0;
    artwork_->clearAmbientGlow();
    return;
  }
  const float intensity = previewGlow_.update(level, spectrum, dt);
  if (intensity == 0) {
    artwork_->clearAmbientGlow();
    return;
  }
  artwork_->setAmbientGlow({.intensity = intensity, .spread = compact_ ? 28.0f : 80.0f,
                             .spectrum = previewGlow_.spectrum(), .bass = previewGlow_.bass()});
}

void ChartDetailsView::refreshArtworkLayout() {
  if (!artworkFrame_ || !artworkPlaceholder_ || !identity_) return;
  const int width = rendering::compactMenuCanvasActive ? 0 : compact_ ? 112 : std::min(472, getWidth() > 0 ? getWidth() : 472);
  const int height = width;
  if (width == artworkLayoutWidth_ && height == artworkLayoutHeight_ &&
      compact_ == artworkLayoutCompact_) return;
  artworkLayoutWidth_ = width;
  artworkLayoutHeight_ = height;
  artworkLayoutCompact_ = compact_;
  View::LayoutBatchScope batch;
  artworkFrame_->setWidth(width)->setHeight(height);
  artwork_->setWidthPercent(100)->setHeight(height);
  artworkPlaceholder_->setHeight(height);
  identity_->setHeight(compact_ ? kIdentityHeight : height + 124);
}

void ChartDetailsView::setCompact(bool compact) {
  View::LayoutBatchScope batch;
  compact_ = compact;
  // The visualizer overflows its image bounds without reserving layout space.
  setPadding(Edge::Top, 0);
  identity_->setFlexDirection(compact ? FlexDirection::Row : FlexDirection::Column);
  identity_->setAlignItems(compact ? YGAlignCenter : YGAlignStretch);
  identity_->setGap(compact ? 14 : 8);
  artworkFrame_->setAlignSelf(YGAlignCenter);
  refreshArtworkLayout();
  visible(artworkFrame_, !rendering::compactMenuCanvasActive);
  titleRow_->setHeight(compact ? 74 : 56);
  title_->setHeight(compact ? 74 : 56);
  title_->setWrap(false);
  title_->setOverflow(TextView::TextOverflow::Marquee);
  artist_->moveTo(*titles_);
  genre_->moveTo(*titles_);
  artist_->setFlex(0)->setMinWidth(0)->setHeight(compact ? 28 : 30);
  genre_->setFlex(0)->setMinWidth(0);
  setGap(compact ? kSectionGap : 6);
  visible(summary_, !compact && !rendering::compactMenuCanvasActive);
  visible(scoreSlot_, !compact);
  visible(rate_, compact);
  personalBest_->setPadding(Edge::All, compact ? 14 : 0);
  difficulty_->moveTo(*levelLine_);
  chartLevel_->moveTo(*levelLine_);
  difficulty_->moveTo(compact ? *this : *levelLine_);
  chartLevel_->moveTo(compact ? *this : *levelLine_);
  difficulty_->setWidth(YGUndefined)->setHeight(compact ? kDifficultyHeight : 46);
  difficulty_->setFlexShrink(compact ? 0 : 1);
  difficulty_->setPadding(Edge::Left, compact ? 0 : 12)->setPadding(Edge::Right, compact ? 0 : 12);
  chartLevel_->setFlex(compact ? 0 : 1)->setMinWidth(compact ? 0 : 96)->setHeight(compact ? 28 : 36);
  chartLevel_->setAutoFitText(!compact);
  difficulty_->setThemedBackgroundColor(compact ? [] { return Color(0, 0, 0, 0); } : main_menu_theme::action);
  difficulty_->setThemedColor(compact ? main_menu_theme::accent : main_menu_theme::onAction);
  difficulty_->setAlign(compact ? TextView::LEFT : TextView::CENTER);

  // Reappend sections and metric cells in a canonical order on every rotation.
  facts_->moveTo(*levelGroup_);
  if (!compact) {
    summary_->moveTo(*identity_);
    summary_->moveTo(*this);
    scoreSlot_->moveTo(*identity_);
    scoreSlot_->moveTo(*this);
  }
  facts_->moveTo(*this);
  facts_->setPadding(Edge::All, compact ? kFactPadding : 0);
  facts_->setGap(compact ? kFactGap : 0)->setFlexGrow(compact ? 1 : 0);
  facts_->setJustifyContent(compact ? YGJustifySpaceBetween : YGJustifyFlexStart);
  for (size_t i = 0; i < factCells_.size(); ++i) {
    factCells_[i]->moveTo(*levelGroup_);
    visible(factCells_[i], compact || (i != 0 && i != 2));
  }
  for (auto *row : factRows_) row->moveTo(*levelGroup_);
  for (auto *row : factRows_) row->moveTo(*facts_);
  if (compact) {
    for (size_t i = 0; i < factCells_.size(); ++i) factCells_[i]->moveTo(*factRows_[i / 2]);
  } else {
    // Keep only facts not already shown in the tempo summary.
    for (size_t i : {4U, 3U, 5U, 1U}) factCells_[i]->moveTo(*factRows_[0]);
    factCells_[0]->moveTo(*factRows_[1]);
    factCells_[2]->moveTo(*factRows_[1]);
  }
  for (size_t i = 0; i < factRows_.size(); ++i) {
    auto *row = factRows_[i];
    visible(row, compact || i == 0);
    row->setHeight(compact ? kFactRowHeight : 48)->setMinHeight(compact ? kFactRowHeight : 48);
    row->setFlexGrow(0)->setGap(compact ? 14 : 10);
    for (auto *cell : row->getChildren()) {
      cell->setFlexDirection(FlexDirection::Column)->setAlignItems(YGAlignStretch)->setGap(0);
      for (size_t j = 0; j < cell->getChildren().size(); ++j) {
        auto *text = static_cast<TextView *>(cell->getChildren()[j]);
        text->setFlex(0)->setMinWidth(0)->setHeight(compact ? (j ? 34 : 26) : (j ? 28 : 20));
        text->setAutoFitText(!compact);
      }
    }
  }
  if (!scoreContainer_) {
    personalBest_->moveTo(*levelGroup_);
    setScoreContainer(nullptr);
  }
}

void ChartDetailsView::setChart(const ChartMetaRecord *record,
                               const std::optional<ScoreBestSnapshot> &best,
                               int clearRank, const std::string &total) {
  const bool chart = record && !record->courseStart && !record->solidArchive &&
                     !record->unavailable && !record->meta.BmsPath.empty();
  const std::string chartPath = chart ? record->meta.BmsPath : "";
  if (glowChartPath_ != chartPath) {
    glowChartPath_ = chartPath;
    previewGlow_.reset();
    sampledSpectrum_.fill(0);
    spectrumCooldown_ = 0;
    artwork_->clearAmbientGlow();
  }
  artwork_->setVisible(chart && !record->meta.StageFile.empty());
  setCompact(compact_);
  title_->setLocalizedText(record ? i18n::Text(record->meta.Title +
      (record->meta.SubTitle.empty() ? "" : " " + record->meta.SubTitle))
      : i18n::message("menu.details.select_chart.label"));
  artist_->setText(record ? record->meta.Artist +
      (record->meta.SubArtist.empty() ? "" : " / " + record->meta.SubArtist) : "");
  genre_->setText(record ? record->meta.Genre : "");
  grade_->setText("—");
  desktopRate_->setText("—");
  tempo_->setText("BPM —   |   —");
  if (!chart) {
    for (auto *value : {difficulty_, chartLevel_, bpm_, judge_, length_, notes_, total_,
                       noteTypes_, score_, rate_, next_, clear_}) {
      value->setText("—");
    }
    visible(emptyScore_, false);
    visible(score_, true);
    bestLabel_->setLocalizedText(i18n::message("menu.details.best.label"));
    totalLabel_->setLocalizedText(i18n::message("menu.details.total.label"));
    updateClearLamp(*clear_, kNoClearTypeRank);
    rate_->setThemedColor(main_menu_theme::secondary);
    if (record) {
      difficulty_->setLocalizedText(i18n::message(record->courseStart
          ? "library.chart.course.label" : record->solidArchive
          ? "library.chart.unzip_required.label" : "library.chart.missing.badge"));
    }
    return;
  }

  const auto &meta = record->meta;
  const std::string chartLevel = "Lv. " + number(meta.PlayLevel);
  const bool hasTableLevel = !record->difficultyTableLabels.empty();
  difficulty_->setText(hasTableLevel ? record->difficultyTableLabels : chartLevel);
  chartLevel_->setText(gameplay::keyModeLabel(meta.KeyMode) +
                      (hasTableLevel ? " · " + chartLevel : ""));
  bpm_->setText(bpmRange(meta));
  judge_->setText(judgeRank(meta));
  length_->setText(duration(meta.PlayLength));
  tempo_->setText("BPM " + bpmRange(meta) + "   |   " + duration(meta.PlayLength));
  notes_->setText(meta.TotalNotes >= 0 ? std::to_string(meta.TotalNotes) : "—");
  total_->setText(total.empty() ? "—" : total);
  totalLabel_->setLocalizedText(i18n::message(meta.HasTotal && meta.Total > 0
      ? "menu.details.total.label" : "menu.details.total_auto.label"));
  noteTypes_->setText(std::to_string(meta.TotalLongNotes + meta.TotalBackSpinNotes) +
      " / " + std::to_string(meta.TotalScratchNotes + meta.TotalBackSpinNotes));

  visible(emptyScore_, !best.has_value());
  visible(score_, best.has_value());
  bestLabel_->setLocalizedText(i18n::message(best && best->source == ScoreBestSource::ImportedIr
      ? "menu.details.best_ir.label" : "menu.details.best.label"));
  clear_->setText(clearRank == kNoClearTypeRank ? "—" : clearTypeRankToLabel(clearRank));
  updateClearLamp(*clear_, clearRank);
  if (!best) {
    score_->setText("");
    rate_->setText("");
    next_->setText("");
    return;
  }

  const int maxScore = best->maxScore > 0 ? best->maxScore
      : result_contract::maximumScoreForNotes(meta.TotalNotes).value_or(0);
  score_->setText(std::to_string(best->score) + " / " +
                  (maxScore > 0 ? std::to_string(maxScore) : "—"));
  if (maxScore <= 0) {
    rate_->setText("");
    next_->setText("");
    return;
  }
  const std::string rank = score_rank::displayLabelForScore(best->score, maxScore);
  grade_->setText(rank);
  std::ostringstream rate;
  rate.imbue(std::locale::classic());
  rate << rank << " · " << std::fixed << std::setprecision(2)
       << 100.0 * static_cast<double>(best->score) / maxScore << '%';
  rate_->setText(rate.str());
  desktopRate_->setText(rate.str().substr(rank.size() + std::string(" · ").size()));
  rate_->setThemedColor([rank] { return ui_theme::scoreRankColor(rank); });
  next_->setText(nextGrade(best->score, maxScore));
}
