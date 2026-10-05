#include "ChartDetailsView.h"

#include "ClearLampColors.h"
#include "ImageView.h"
#include "TextView.h"
#include "UiTheme.h"
#include "../ResultContracts.h"
#include "../ScoreRankUtils.h"

#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>

namespace {
constexpr const char *kFont = "assets/fonts/notosanscjkjp.ttf";

TextView *label(const char *name, int size, int height,
                View::ThemeColorProvider color = ui_theme::textPrimary) {
  auto *text = new TextView(kFont, size);
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
  identity->setFlexDirection(FlexDirection::Row)->setGap(14)->setHeight(kIdentityHeight);
  identity->setAlignItems(YGAlignCenter)->setFlexShrink(0);
  artworkFrame_ = new View();
  artworkFrame_->setWidth(112)->setHeight(112)->setFlexShrink(0);
  artworkFrame_->setPadding(Edge::All, 2);
  artworkFrame_->setCornerRadius(ui_theme::controlRadius());
  artworkFrame_->setThemedBackgroundColor(ui_theme::mainMenuSurface);
  artwork->setWidth(108)->setHeight(108);
  artwork->setCornerRadius(ui_theme::childRadiusForInset(
      ui_theme::controlRadius(), 0, 2));
  artworkFrame_->addView(artwork);
  identity->addView(artworkFrame_);

  auto *titles = new View();
  titles->setFlex(1)->setMinWidth(0)->setGap(2);
  title_ = label("chartDetailsTitle", 32, 76);
  title_->setWrap(true);
  artist_ = label("chartDetailsArtist", 23, 28, ui_theme::textSecondary);
  artist_->setOverflow(TextView::TextOverflow::Marquee);
  genre_ = label("chartDetailsGenre", 21, 26, ui_theme::textMuted);
  genre_->setOverflow(TextView::TextOverflow::Marquee);
  titles->addView(title_);
  titles->addView(artist_);
  titles->addView(genre_);
  identity->addView(titles);
  addView(identity);

  difficulty_ = label("chartDetailsDifficulty", 24, kDifficultyHeight, ui_theme::cyan);
  difficulty_->setOverflow(TextView::TextOverflow::Marquee);
  addView(difficulty_);

  facts_ = new View();
  facts_->setName("chartDetailsFacts");
  facts_->setFlexDirection(FlexDirection::Column)->setGap(kFactGap)->setFlexShrink(0);
  facts_->setFlexGrow(1);
  facts_->setPadding(Edge::All, kFactPadding);
  facts_->setThemedBackgroundColor(ui_theme::mainMenuSurface);
  facts_->setCornerRadius(ui_theme::controlRadius());

  auto row = [this]() {
    auto *view = new View();
    view->setFlexDirection(FlexDirection::Row)->setGap(14);
    view->setHeight(kFactRowHeight)->setMinHeight(kFactRowHeight)->setFlexShrink(0);
    view->setFlexGrow(1);
    facts_->addView(view);
    return view;
  };
  auto metric = [](View *row, const char *key, const char *name,
                   TextView **caption = nullptr) {
    auto *cell = new View();
    cell->setFlex(1)->setMinWidth(0)->setJustifyContent(YGJustifySpaceBetween);
    auto *heading = label("", 20, 26, ui_theme::textSecondary);
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
  personalBest_->setThemedBackgroundColor(ui_theme::mainMenuSurface);
  personalBest_->setCornerRadius(ui_theme::controlRadius());
  auto *bestHeading = new View();
  bestHeading->setFlexDirection(FlexDirection::Row)->setGap(8)->setHeight(28);
  bestLabel_ = label("", 20, 26, ui_theme::textSecondary);
  bestLabel_->setFlex(1);
  bestHeading->addView(bestLabel_);
  auto *clearRow = new View();
  clearRow->setFlexDirection(FlexDirection::Row)->setAlignItems(YGAlignCenter)->setGap(8);
  clearRow->setFlex(2)->setMinWidth(0);
  clearLamp_ = new View();
  clearLamp_->setWidth(10)->setHeight(22)->setCornerRadius(3)->setFlexShrink(0);
  clearRow->addView(clearLamp_);
  clear_ = label("chartDetailsClear", 20, 28);
  clear_->setFlex(1);
  clear_->setOverflow(TextView::TextOverflow::Marquee);
  clearRow->addView(clear_);
  bestHeading->addView(clearRow);
  personalBest_->addView(bestHeading);
  score_ = label("chartDetailsScore", 34, 44);
  personalBest_->addView(score_);
  auto *progress = new View();
  progress->setFlexDirection(FlexDirection::Row)->setGap(8);
  rate_ = label("chartDetailsRate", 24, 32);
  rate_->setFlex(1);
  next_ = label("chartDetailsNext", 22, 32, ui_theme::textSecondary);
  next_->setAlign(TextView::RIGHT);
  progress->addView(rate_);
  progress->addView(next_);
  personalBest_->addView(progress);
  emptyScore_ = label("chartDetailsEmptyScore", 26, 44, ui_theme::textSecondary);
  emptyScore_->setLocalizedText(i18n::message("menu.details.unplayed.label"));
  personalBest_->addView(emptyScore_);
  addView(personalBest_);
  setChart(nullptr, std::nullopt, kNoClearTypeRank, "");
}

void ChartDetailsView::setScoreContainer(View *container) {
  personalBest_->moveTo(container ? *container : *this);
}

void ChartDetailsView::setChart(const ChartMetaRecord *record,
                               const std::optional<ScoreBestSnapshot> &best,
                               int clearRank, const std::string &total) {
  const bool chart = record && !record->courseStart && !record->solidArchive &&
                     !record->unavailable && !record->meta.BmsPath.empty();
  visible(artworkFrame_, chart && !record->meta.StageFile.empty());
  title_->setLocalizedText(record ? i18n::Text(record->meta.Title +
      (record->meta.SubTitle.empty() ? "" : " " + record->meta.SubTitle))
      : i18n::message("menu.details.select_chart.label"));
  artist_->setText(record ? record->meta.Artist +
      (record->meta.SubArtist.empty() ? "" : " / " + record->meta.SubArtist) : "");
  genre_->setText(record ? record->meta.Genre : "");
  if (!chart) {
    for (auto *value : {difficulty_, bpm_, judge_, length_, notes_, total_,
                       noteTypes_, score_, rate_, next_, clear_}) {
      value->setText("—");
    }
    visible(emptyScore_, false);
    visible(score_, true);
    bestLabel_->setLocalizedText(i18n::message("menu.details.best.label"));
    totalLabel_->setLocalizedText(i18n::message("menu.details.total.label"));
    clearLamp_->clearBackgroundColor();
    rate_->setThemedColor(ui_theme::textSecondary);
    if (record) {
      difficulty_->setLocalizedText(i18n::message(record->courseStart
          ? "library.chart.course.label" : record->solidArchive
          ? "library.chart.unzip_required.label" : "library.chart.missing.badge"));
    }
    return;
  }

  const auto &meta = record->meta;
  difficulty_->setText(std::to_string(meta.KeyMode) + "K · Lv. " +
      number(meta.PlayLevel) + (record->difficultyTableLabels.empty()
          ? "" : " · " + record->difficultyTableLabels));
  bpm_->setText(bpmRange(meta));
  judge_->setText(judgeRank(meta));
  length_->setText(duration(meta.PlayLength));
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
  if (hasClearLampColor(clearRank)) {
    clearLamp_->setBackgroundColor(clearLampColorForRank(clearRank));
  } else {
    clearLamp_->clearBackgroundColor();
  }
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
  std::ostringstream rate;
  rate.imbue(std::locale::classic());
  rate << rank << " · " << std::fixed << std::setprecision(2)
       << 100.0 * static_cast<double>(best->score) / maxScore << '%';
  rate_->setText(rate.str());
  rate_->setThemedColor([rank] { return ui_theme::scoreRankColor(rank); });
  next_->setText(nextGrade(best->score, maxScore));
}
