#include "DefaultSkin.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include "../view/TextView.h"
#include "../view/Button.h"
#include "../view/ClearLampColors.h"
#include "../view/UiTheme.h"

namespace {
std::string formatNumber(double value, int decimals = 0) {
  std::ostringstream output;
  output << std::fixed << std::setprecision(decimals) << value;
  return output.str();
}

std::string formatSignedDelta(int delta) {
  return (delta >= 0 ? "+" : "") + std::to_string(delta);
}

std::string formatScoreRate(int score, int maxScore) {
  if (maxScore <= 0) {
    return "0.00%";
  }
  return formatNumber(static_cast<double>(score) * 100.0 /
                          static_cast<double>(maxScore),
                      2) +
         "%";
}

std::string gradeForScore(int score, int maxScore) {
  const double percentage =
      maxScore > 0 ? static_cast<double>(score) / maxScore : 0.0;
  if (percentage >= 8.0 / 9.0) {
    return "AAA";
  }
  if (percentage >= 7.0 / 9.0) {
    return "AA";
  }
  if (percentage >= 6.0 / 9.0) {
    return "A";
  }
  if (percentage >= 5.0 / 9.0) {
    return "B";
  }
  if (percentage >= 4.0 / 9.0) {
    return "C";
  }
  if (percentage >= 3.0 / 9.0) {
    return "D";
  }
  if (percentage >= 2.0 / 9.0) {
    return "E";
  }
  return "F";
}

std::string formatBpm(const bms_parser::ChartMeta &meta) {
  auto formatBpmValue = [](double value) {
    if (std::abs(value - std::round(value)) < 0.01) {
      return std::to_string(static_cast<int>(std::round(value)));
    }
    return formatNumber(value, 2);
  };

  const double minBpm = meta.MinBpm > 0.0 ? meta.MinBpm : meta.Bpm;
  const double maxBpm = meta.MaxBpm > 0.0 ? meta.MaxBpm : meta.Bpm;
  if (minBpm > 0.0 && maxBpm > 0.0 && std::abs(maxBpm - minBpm) > 0.01) {
    return formatBpmValue(minBpm) + "-" + formatBpmValue(maxBpm);
  }
  return formatBpmValue(meta.Bpm);
}

std::string formatDuration(long long micros) {
  if (micros <= 0) {
    return "0:00";
  }
  const long long seconds = micros / 1000000LL;
  const long long minutes = seconds / 60LL;
  const long long remaining = seconds % 60LL;
  std::ostringstream output;
  output << minutes << ":" << std::setw(2) << std::setfill('0') << remaining;
  return output.str();
}

std::string formatGauge(float gauge) {
  return formatNumber(static_cast<double>(gauge), 1) + "%";
}

std::string clearTypeLabelForRank(int rank) {
  if (rank >= kClearTypeExHardClearRank) {
    return "EX-HARD CLEAR";
  }
  if (rank >= kClearTypeHardClearRank) {
    return "HARD CLEAR";
  }
  if (rank >= kClearTypeNormalClearRank) {
    return "NORMAL CLEAR";
  }
  if (rank >= kClearTypeEasyClearRank) {
    return "EASY CLEAR";
  }
  if (rank >= kClearTypeAssistedEasyClearRank) {
    return "ASSISTED EASY CLEAR";
  }
  if (rank == kNoClearTypeRank) {
    return "NO PLAY";
  }
  return "FAILED";
}

std::pair<std::string, int> nextRankTarget(int score, int maxScore) {
  if (maxScore <= 0) {
    return {"-", 0};
  }

  struct Threshold {
    const char *label;
    int numerator;
  };
  constexpr Threshold thresholds[] = {{"E", 2},   {"D", 3},  {"C", 4},
                                      {"B", 5},   {"A", 6},  {"AA", 7},
                                      {"AAA", 8}, {"MAX", 9}};
  for (const auto &threshold : thresholds) {
    const int target =
        static_cast<int>(std::ceil(maxScore * threshold.numerator / 9.0));
    if (score < target) {
      return {threshold.label, score - target};
    }
  }
  return {"MAX", 0};
}
} // namespace

void DefaultSkin::buildLayout(const std::string &screenName, View *root,
                              void *data) {
  if (screenName == "Result") {
    buildResultLayout(root, static_cast<ResultSkinData *>(data));
  }
}

void DefaultSkin::buildResultLayout(View *rootLayout, ResultSkinData *data) {
  const auto &meta = *data->meta;
  const auto &resultState = *data->state;
  auto &context = *data->context;

  rootLayout->setFlexDirection(FlexDirection::Column);
  rootLayout->setAlignItems(YGAlignStretch);
  rootLayout->setJustifyContent(YGJustifyCenter);
  rootLayout->setPadding(Edge::All, 32);
  rootLayout->setGap(12);
  rootLayout->setBackgroundColor(ui_theme::resultBackdrop());

  auto makeLabel = [](const std::string &text, int size, Color color) {
    auto *label = new TextView("assets/fonts/notosanscjkjp.ttf", size);
    label->setText(text);
    label->setColor(ui_theme::sdl(color));
    label->setVAlign(TextView::MIDDLE);
    return label;
  };

  auto makePanel = [](Color fill, Color border) {
    auto *panel = new View();
    panel->setBackgroundColor(fill);
    panel->setCornerRadius(ui_theme::panelRadius());
    panel->setShadow(ui_theme::cardShadow(), ui_theme::kCardShadow);
    panel->setBorderColor(border);
    panel->setBorderWidth(1);
    return panel;
  };

  const int totalNotes = meta.TotalNotes;
  const int maxScore = totalNotes * 2;
  const int currentScore = resultState.getScore();
  const std::string grade = gradeForScore(currentScore, maxScore);

  auto countFor = [&resultState](Judgement judgement) {
    const auto it = resultState.judgeCount.find(judgement);
    return it == resultState.judgeCount.end() ? 0 : it->second;
  };
  auto fastSlowFor = [&resultState](Judgement judgement) {
    const auto it = resultState.judgementFastSlowCount.find(judgement);
    return it == resultState.judgementFastSlowCount.end()
               ? JudgementFastSlowCount{}
               : it->second;
  };

  auto *header = new View();
  header->setFlexDirection(FlexDirection::Row);
  header->setAlignItems(YGAlignCenter);
  header->setGap(20);
  header->setMinHeight(data->playModeLabel.empty() ? 78 : 100);

  auto *titleStack = new View();
  titleStack->setFlexDirection(FlexDirection::Column);
  titleStack->setFlex(1);
  titleStack->setGap(4);

  auto titleText = new TextView("assets/fonts/notosanscjkjp.ttf", 36);
  titleText->setText(meta.Title);
  titleText->setColor(ui_theme::sdl(ui_theme::textPrimary()));
  titleText->setOverflow(TextView::TextOverflow::Marquee);
  titleText->setHeight(46);
  titleText->setName("title");
  titleStack->addView(titleText);

  auto artistText = new TextView("assets/fonts/notosanscjkjp.ttf", 21);
  artistText->setText(meta.Artist);
  artistText->setColor(ui_theme::sdl(ui_theme::textSecondary()));
  artistText->setHeight(30);
  artistText->setName("artist");
  titleStack->addView(artistText);

  if (!data->playModeLabel.empty()) {
    auto playModeText = new TextView("assets/fonts/notosanscjkjp.ttf", 18);
    playModeText->setText("PLAY MODE: " + data->playModeLabel);
    playModeText->setColor(ui_theme::sdl(ui_theme::textSecondary()));
    playModeText->setHeight(28);
    playModeText->setOverflow(TextView::TextOverflow::Hidden);
    playModeText->setName("playMode");
    titleStack->addView(playModeText);
  }
  header->addView(titleStack);
  rootLayout->addView(header);

  const auto nextRank = nextRankTarget(currentScore, maxScore);
  const auto hasPreviousBest =
      data != nullptr && data->previousBest.has_value();
  const int previousScore = hasPreviousBest ? data->previousBest->score : 0;
  const int previousMaxScore =
      hasPreviousBest && data->previousBest->maxScore > 0
          ? data->previousBest->maxScore
          : maxScore;
  const int previousClearRank =
      hasPreviousBest ? data->previousBest->clearType : kNoClearTypeRank;
  const int scoreDelta = hasPreviousBest ? currentScore - previousScore : 0;
  const int comboDelta =
      hasPreviousBest ? resultState.maxCombo - data->previousBest->maxCombo : 0;
  const int breakDelta =
      hasPreviousBest ? resultState.comboBreak - data->previousBest->comboBreak
                      : 0;
  const Color currentClearAccent =
      clearLampColorForRank(resultState.getClearTypeRank());
  const Color previousClearAccent =
      hasPreviousBest ? clearLampColorForRank(previousClearRank)
                      : ui_theme::textMuted();
  const Color positiveDelta =
      scoreDelta >= 0 ? ui_theme::lime() : ui_theme::coral();
  const std::string longNoteSummary =
      meta.TotalLongNotes > 0 ? std::to_string(meta.TotalLongNotes) + " LN"
                              : "";
  const int playLevelDecimals =
      std::abs(meta.PlayLevel - std::round(meta.PlayLevel)) < 0.01 ? 0 : 1;
  const std::string playLevelLabel =
      "LV " + formatNumber(meta.PlayLevel, playLevelDecimals);

  auto makeDivider = []() {
    auto *divider = new View();
    divider->setWidth(1);
    divider->setAlignSelf(YGAlignStretch);
    divider->setBackgroundColor(ui_theme::hairlineSubtle());
    divider->setFlexShrink(0);
    return divider;
  };

  auto makeComparisonColumn = [&](const std::string &label,
                                  const std::string &value,
                                  const std::string &detail, Color accent,
                                  int valueSize = 28) {
    auto *column = new View();
    column->setFlexDirection(FlexDirection::Column);
    column->setJustifyContent(YGJustifyCenter);
    column->setFlexGrow(1);
    column->setFlexBasis(0);
    column->setMinWidth(0);
    column->setGap(2);

    auto *labelView = makeLabel(label, 13, ui_theme::textSecondary());
    labelView->setHeight(18);
    labelView->setAlign(TextView::CENTER);
    labelView->setOverflow(TextView::TextOverflow::Hidden);
    column->addView(labelView);

    auto *valueView = makeLabel(value, valueSize, accent);
    valueView->setHeight(valueSize + 8);
    valueView->setAlign(TextView::CENTER);
    valueView->setOverflow(TextView::TextOverflow::Hidden);
    column->addView(valueView);

    auto *detailView = makeLabel(detail, 14, ui_theme::textSecondary());
    detailView->setHeight(18);
    detailView->setAlign(TextView::CENTER);
    detailView->setOverflow(TextView::TextOverflow::Hidden);
    column->addView(detailView);
    return column;
  };

  auto makeLampColumn = [&](const std::string &label,
                            const std::string &lampLabel,
                            const std::string &detail, Color accent) {
    auto *column = new View();
    column->setFlexDirection(FlexDirection::Column);
    column->setJustifyContent(YGJustifyCenter);
    column->setAlignItems(YGAlignStretch);
    column->setFlexGrow(1);
    column->setFlexBasis(0);
    column->setMinWidth(0);
    column->setGap(4);

    auto *labelView = makeLabel(label, 13, ui_theme::textSecondary());
    labelView->setHeight(18);
    labelView->setAlign(TextView::CENTER);
    labelView->setOverflow(TextView::TextOverflow::Hidden);
    column->addView(labelView);

    auto *lampRow = new View();
    lampRow->setFlexDirection(FlexDirection::Row);
    lampRow->setAlignItems(YGAlignCenter);
    lampRow->setJustifyContent(YGJustifyCenter);
    lampRow->setGap(8);
    lampRow->setHeight(38);

    auto *lampSwatch = new View();
    lampSwatch->setWidth(8);
    lampSwatch->setHeight(30);
    lampSwatch->setFlexShrink(0);
    lampSwatch->setBackgroundColor(accent);
    lampSwatch->setCornerRadius(4.0f);
    lampRow->addView(lampSwatch);

    auto *lampText = makeLabel(lampLabel, 18, ui_theme::textPrimary());
    lampText->setHeight(30);
    lampText->setFlexShrink(1);
    lampText->setMinWidth(0);
    lampText->setOverflow(TextView::TextOverflow::Hidden);
    lampRow->addView(lampText);
    column->addView(lampRow);

    auto *detailView = makeLabel(detail, 14, ui_theme::textSecondary());
    detailView->setHeight(18);
    detailView->setAlign(TextView::CENTER);
    detailView->setOverflow(TextView::TextOverflow::Hidden);
    column->addView(detailView);
    return column;
  };

  auto addPanelTitle = [&](View *panel, const std::string &title) {
    auto *titleView = makeLabel(title, 14, ui_theme::textSecondary());
    titleView->setHeight(20);
    titleView->setOverflow(TextView::TextOverflow::Hidden);
    panel->addView(titleView);
  };

  auto *summaryRow = new View();
  summaryRow->setFlexDirection(FlexDirection::Row);
  summaryRow->setAlignItems(YGAlignStretch);
  summaryRow->setGap(12);
  summaryRow->setHeight(154);
  summaryRow->setName("resultSummary");

  auto *gradePanel = makePanel(
      ui_theme::resultPanelStrong(), ui_theme::withAlpha(ui_theme::amber(), 138));
  gradePanel->setWidth(150);
  gradePanel->setFlexShrink(0);
  gradePanel->setFlexDirection(FlexDirection::Column);
  gradePanel->setAlignItems(YGAlignCenter);
  gradePanel->setJustifyContent(YGJustifyCenter);
  gradePanel->setPadding(Edge::All, 12);
  gradePanel->setGap(2);
  auto *gradeLabel = makeLabel("GRADE", 14, ui_theme::textSecondary());
  gradeLabel->setHeight(18);
  gradeLabel->setAlign(TextView::CENTER);
  gradePanel->addView(gradeLabel);
  auto *gradeText = new TextView("assets/fonts/notosanscjkjp.ttf", 68);
  gradeText->setText(grade);
  gradeText->setColor(ui_theme::sdl(ui_theme::amber()));
  gradeText->setAlign(TextView::CENTER);
  gradeText->setHeight(76);
  gradeText->setName("grade");
  gradePanel->addView(gradeText);
  auto *rateText = makeLabel(formatScoreRate(currentScore, maxScore), 16,
                             ui_theme::cyan());
  rateText->setHeight(22);
  rateText->setAlign(TextView::CENTER);
  gradePanel->addView(rateText);
  summaryRow->addView(gradePanel);

  auto *scorePanel =
      makePanel(ui_theme::resultPanel(), ui_theme::hairlineSubtle());
  scorePanel->setFlexGrow(1.45f);
  scorePanel->setFlexBasis(0);
  scorePanel->setMinWidth(0);
  scorePanel->setFlexDirection(FlexDirection::Column);
  scorePanel->setPadding(Edge::All, 14);
  scorePanel->setGap(6);
  addPanelTitle(scorePanel, "SCORE COMPARISON");
  auto *scoreCompareRow = new View();
  scoreCompareRow->setFlexDirection(FlexDirection::Row);
  scoreCompareRow->setAlignItems(YGAlignStretch);
  scoreCompareRow->setGap(10);
  scoreCompareRow->setFlexGrow(1);
  scoreCompareRow->addView(makeComparisonColumn(
      "CURRENT", std::to_string(currentScore),
      "MAX " + std::to_string(maxScore), ui_theme::textPrimary(), 31));
  scoreCompareRow->addView(makeDivider());
  scoreCompareRow->addView(makeComparisonColumn(
      "BEST",
      hasPreviousBest ? std::to_string(previousScore) : "NO PLAY",
      hasPreviousBest ? gradeForScore(previousScore, previousMaxScore) : "",
      hasPreviousBest ? ui_theme::textPrimary() : ui_theme::textMuted(), 31));
  scorePanel->addView(scoreCompareRow);
  auto *scoreDeltaText =
      makeLabel(hasPreviousBest ? "DELTA " + formatSignedDelta(scoreDelta)
                                : "DELTA --",
                17, hasPreviousBest ? positiveDelta : ui_theme::textMuted());
  scoreDeltaText->setHeight(22);
  scoreDeltaText->setAlign(TextView::CENTER);
  scoreDeltaText->setOverflow(TextView::TextOverflow::Hidden);
  scorePanel->addView(scoreDeltaText);
  summaryRow->addView(scorePanel);

  auto *lampPanel = makePanel(
      ui_theme::resultPanel(),
      ui_theme::withAlpha(currentClearAccent, hasPreviousBest ? 120 : 96));
  lampPanel->setFlexGrow(1.15f);
  lampPanel->setFlexBasis(0);
  lampPanel->setMinWidth(0);
  lampPanel->setFlexDirection(FlexDirection::Column);
  lampPanel->setPadding(Edge::All, 14);
  lampPanel->setGap(6);
  addPanelTitle(lampPanel, "CLEAR LAMP COMPARISON");
  auto *lampCompareRow = new View();
  lampCompareRow->setFlexDirection(FlexDirection::Row);
  lampCompareRow->setAlignItems(YGAlignStretch);
  lampCompareRow->setGap(10);
  lampCompareRow->setFlexGrow(1);
  lampCompareRow->addView(makeLampColumn(
      "CURRENT", resultState.getClearTypeLabel(),
      "GAUGE " + formatGauge(resultState.currentGauge), currentClearAccent));
  lampCompareRow->addView(makeDivider());
  lampCompareRow->addView(makeLampColumn(
      "BEST", clearTypeLabelForRank(previousClearRank),
      hasPreviousBest ? "GAUGE " + formatGauge(data->previousBest->finalGauge)
                      : "",
      previousClearAccent));
  lampPanel->addView(lampCompareRow);
  summaryRow->addView(lampPanel);

  auto *comboPanel =
      makePanel(ui_theme::resultPanel(), ui_theme::hairlineSubtle());
  comboPanel->setFlexGrow(1.0f);
  comboPanel->setFlexBasis(0);
  comboPanel->setMinWidth(0);
  comboPanel->setFlexDirection(FlexDirection::Column);
  comboPanel->setPadding(Edge::All, 14);
  comboPanel->setGap(6);
  addPanelTitle(comboPanel, "COMBO / BREAK COMPARISON");
  auto *comboCompareRow = new View();
  comboCompareRow->setFlexDirection(FlexDirection::Row);
  comboCompareRow->setAlignItems(YGAlignStretch);
  comboCompareRow->setGap(10);
  comboCompareRow->setFlexGrow(1);
  comboCompareRow->addView(makeComparisonColumn(
      "CURRENT", std::to_string(resultState.maxCombo),
      "BREAK " + std::to_string(resultState.comboBreak), ui_theme::lime(), 27));
  comboCompareRow->addView(makeDivider());
  comboCompareRow->addView(makeComparisonColumn(
      "BEST",
      hasPreviousBest ? std::to_string(data->previousBest->maxCombo)
                      : "NO PLAY",
      hasPreviousBest ? "BREAK " + std::to_string(data->previousBest->comboBreak)
                      : "",
      hasPreviousBest ? ui_theme::lime() : ui_theme::textMuted(), 27));
  comboPanel->addView(comboCompareRow);
  auto *comboDeltaText = makeLabel(
      hasPreviousBest ? "COMBO " + formatSignedDelta(comboDelta) +
                            " / BREAK " + formatSignedDelta(breakDelta)
                      : "COMBO -- / BREAK --",
      15,
      hasPreviousBest
          ? (comboDelta >= 0 && breakDelta <= 0 ? ui_theme::lime()
                                                : ui_theme::amber())
          : ui_theme::textMuted());
  comboDeltaText->setHeight(20);
  comboDeltaText->setAlign(TextView::CENTER);
  comboDeltaText->setOverflow(TextView::TextOverflow::Hidden);
  comboPanel->addView(comboDeltaText);
  summaryRow->addView(comboPanel);

  rootLayout->addView(summaryRow);

  auto *infoGrid = new View();
  infoGrid->setFlexDirection(FlexDirection::Row);
  infoGrid->setFlexWrap(YGWrapWrap);
  infoGrid->setJustifyContent(YGJustifyCenter);
  infoGrid->setAlignItems(YGAlignCenter);
  infoGrid->setGap(10);
  infoGrid->setName("resultInfoGrid");

  auto addInfoTile = [&](const std::string &label, const std::string &value,
                         const std::string &subValue, Color accent,
                         Color valueColor = ui_theme::textPrimary()) {
    auto *tile = makePanel(ui_theme::resultPanelSubtle(),
                           Color(accent.r, accent.g, accent.b, 98));
    tile->setWidth(150);
    tile->setHeight(62);
    tile->setPadding(Edge::All, 8);
    tile->setFlexDirection(FlexDirection::Column);
    tile->setJustifyContent(YGJustifyCenter);

    auto *labelView = makeLabel(label, 12, ui_theme::textSecondary());
    labelView->setHeight(16);
    labelView->setOverflow(TextView::TextOverflow::Hidden);
    tile->addView(labelView);

    auto *valueView = makeLabel(value, 20, valueColor);
    valueView->setHeight(24);
    valueView->setOverflow(TextView::TextOverflow::Hidden);
    valueView->setName(label);
    tile->addView(valueView);

    auto *subView = makeLabel(subValue, 13, accent);
    subView->setHeight(16);
    subView->setOverflow(TextView::TextOverflow::Hidden);
    tile->addView(subView);
    infoGrid->addView(tile);
  };

  addInfoTile("NEXT RANK", nextRank.first, formatSignedDelta(nextRank.second),
              ui_theme::amber());
  addInfoTile("TOTAL NOTES", std::to_string(totalNotes), longNoteSummary,
              ui_theme::lime());
  addInfoTile("BPM", formatBpm(meta), "", ui_theme::amber());
  addInfoTile("JUDGE RANK", Judge::getRankDescription(meta.Rank), "",
              ui_theme::cyan());
  addInfoTile("DURATION", formatDuration(meta.PlayLength),
              meta.TotalLength > meta.PlayLength
                  ? "BGA " + formatDuration(meta.TotalLength)
                  : "",
              ui_theme::violetActionHover());
  addInfoTile("KEY MODE", std::to_string(meta.GetTotalLaneCount()) + " KEYS",
              playLevelLabel, ui_theme::coral());
  rootLayout->addView(infoGrid);

  auto detailsGrid = new View();
  detailsGrid->setFlexDirection(FlexDirection::Row);
  detailsGrid->setFlexWrap(YGWrapWrap);
  detailsGrid->setJustifyContent(YGJustifyCenter);
  detailsGrid->setAlignItems(YGAlignCenter);
  detailsGrid->setGap(12);
  detailsGrid->setName("detailsGrid");

  auto addMetric = [&](const std::string &label, int count, Color accent,
                       const std::string &id) {
    auto *tile = makePanel(ui_theme::resultPanelSubtle(),
                           Color(accent.r, accent.g, accent.b, 132));
    tile->setWidth(132);
    tile->setHeight(66);
    tile->setPadding(Edge::All, 8);
    tile->setFlexDirection(FlexDirection::Column);
    tile->setJustifyContent(YGJustifyCenter);
    auto *labelView = makeLabel(label, 14, ui_theme::textSecondary());
    labelView->setHeight(20);
    tile->addView(labelView);
    auto *valueView = makeLabel(std::to_string(count), 25, accent);
    valueView->setName(id);
    tile->addView(valueView);
    detailsGrid->addView(tile);
  };

  auto addJudgementMetric = [&](const std::string &label, Judgement judgement,
                                Color accent, const std::string &id) {
    auto *tile = makePanel(ui_theme::resultPanelSubtle(),
                           Color(accent.r, accent.g, accent.b, 132));
    tile->setWidth(132);
    tile->setHeight(66);
    tile->setPadding(Edge::All, 7);
    tile->setFlexDirection(FlexDirection::Column);
    tile->setJustifyContent(YGJustifyCenter);

    auto *labelView = makeLabel(label, 13, ui_theme::textSecondary());
    labelView->setHeight(16);
    tile->addView(labelView);

    auto *valueView = makeLabel(std::to_string(countFor(judgement)), 23,
                                accent);
    valueView->setHeight(26);
    valueView->setName(id);
    tile->addView(valueView);

    const JudgementFastSlowCount timing = fastSlowFor(judgement);
    auto *timingRow = new View();
    timingRow->setFlexDirection(FlexDirection::Row);
    timingRow->setAlignItems(YGAlignCenter);
    timingRow->setJustifyContent(YGJustifyCenter);
    timingRow->setHeight(18);

    auto *fastText =
        makeLabel(std::to_string(timing.fast), 15, ui_theme::cyan());
    fastText->setWidth(36);
    fastText->setHeight(18);
    fastText->setAlign(TextView::RIGHT);
    fastText->setName(id + "Fast");
    timingRow->addView(fastText);

    auto *slashText = makeLabel("/", 15, ui_theme::textSecondary());
    slashText->setWidth(10);
    slashText->setHeight(18);
    slashText->setAlign(TextView::CENTER);
    timingRow->addView(slashText);

    auto *slowText =
        makeLabel(std::to_string(timing.slow), 15, ui_theme::amber());
    slowText->setWidth(36);
    slowText->setHeight(18);
    slowText->setAlign(TextView::LEFT);
    slowText->setName(id + "Slow");
    timingRow->addView(slowText);

    tile->addView(timingRow);
    detailsGrid->addView(tile);
  };

  addJudgementMetric("PGREAT", PGreat, ui_theme::cyan(), "pgreat");
  addJudgementMetric("GREAT", Great, ui_theme::lime(), "great");
  addJudgementMetric("GOOD", Good, ui_theme::amber(), "good");
  addJudgementMetric("BAD", Bad, Color(255, 132, 96, 255), "bad");
  addJudgementMetric("POOR", Poor, ui_theme::coral(), "poor");
  addJudgementMetric("KPOOR", Kpoor, Color(255, 78, 102, 255), "kpoor");
  addMetric("BREAK", resultState.comboBreak, ui_theme::coral(), "break");
  addMetric("FAST", resultState.fastCount, ui_theme::cyan(), "fast");
  addMetric("SLOW", resultState.slowCount, ui_theme::amber(), "slow");

  rootLayout->addView(detailsGrid);

  auto graphPlaceHolder = new View();
  graphPlaceHolder->setHeight(136);
  graphPlaceHolder->setWidthPercent(100);
  graphPlaceHolder->setBackgroundColor(ui_theme::resultPanelSubtle());
  graphPlaceHolder->setCornerRadius(ui_theme::panelRadius());
  graphPlaceHolder->setShadow(ui_theme::cardShadow(), ui_theme::kCardShadow);
  graphPlaceHolder->setBorderColor(ui_theme::hairlineSubtle());
  graphPlaceHolder->setBorderWidth(1);
  graphPlaceHolder->setName("graph");
  rootLayout->addView(graphPlaceHolder);

  if (data != nullptr && data->outGraphPlaceholder != nullptr) {
    *data->outGraphPlaceholder = graphPlaceHolder;
  }

  if (data != nullptr && !data->showControls) {
    return;
  }

  auto *actionsRow = new View();
  actionsRow->setFlexDirection(FlexDirection::Row);
  actionsRow->setAlignItems(YGAlignCenter);
  actionsRow->setJustifyContent(YGJustifyCenter);
  actionsRow->setFlexWrap(YGWrapWrap);
  actionsRow->setGap(14);
  actionsRow->setName("resultActions");

  auto btn = new Button(0, 0, 300, 80);
  auto btnText = new TextView("assets/fonts/notosanscjkjp.ttf", 26);
  btnText->setText("Back to Menu");
  btnText->setAlign(TextView::CENTER);
  btnText->setVAlign(TextView::MIDDLE);
  btnText->setColor(ui_theme::sdl(ui_theme::textOn(ui_theme::primaryAction())));
  btn->setContentView(btnText);
  btn->setName("backButton");
  btn->setSize(232, 64);
  btn->setCornerRadius(ui_theme::controlRadius());
  btn->setBackgroundColors(
      ui_theme::withAlpha(ui_theme::primaryAction(), 182),
      ui_theme::withAlpha(ui_theme::primaryActionHover(), 210),
      ui_theme::withAlpha(ui_theme::primaryActionPressed(), 226));
  btn->setBorderColors(ui_theme::withAlpha(ui_theme::cyan(), 170),
                       ui_theme::withAlpha(ui_theme::cyan(), 210),
                       Color(255, 255, 255, 216));
  btn->setStyledBorderWidth(1);
  btn->setOnClickListener(
      [&context]() { context.sceneManager->changeScene("MainMenu"); });
  actionsRow->addView(btn);
  rootLayout->addView(actionsRow);
}

void DefaultSkin::buildGameContext(View *root, void *data) {
  // Future implementation
}
