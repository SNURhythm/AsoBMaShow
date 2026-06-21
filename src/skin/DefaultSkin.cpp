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
      return {threshold.label, target - score};
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
  const double percentage =
      maxScore > 0 ? static_cast<double>(currentScore) / maxScore : 0.0;
  std::string grade = "F";
  if (percentage >= 8.0 / 9.0) {
    grade = "AAA";
  } else if (percentage >= 7.0 / 9.0) {
    grade = "AA";
  } else if (percentage >= 6.0 / 9.0) {
    grade = "A";
  } else if (percentage >= 5.0 / 9.0) {
    grade = "B";
  } else if (percentage >= 4.0 / 9.0) {
    grade = "C";
  } else if (percentage >= 3.0 / 9.0) {
    grade = "D";
  } else if (percentage >= 2.0 / 9.0) {
    grade = "E";
  }

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

  const Color clearAccent = clearLampColorForRank(resultState.getClearTypeRank());
  auto *clearBadge = makePanel(Color(clearAccent.r, clearAccent.g,
                                     clearAccent.b, 30),
                               Color(clearAccent.r, clearAccent.g,
                                     clearAccent.b, 136));
  clearBadge->setWidth(260);
  clearBadge->setHeight(64);
  clearBadge->setFlexDirection(FlexDirection::Row);
  clearBadge->setAlignItems(YGAlignCenter);
  clearBadge->setPadding(Edge::All, 12);
  clearBadge->setGap(12);
  auto *lampSwatch = new View();
  lampSwatch->setWidth(10);
  lampSwatch->setHeight(44);
  lampSwatch->setFlexShrink(0);
  lampSwatch->setBackgroundColor(clearAccent);
  lampSwatch->setCornerRadius(5.0f);
  clearBadge->addView(lampSwatch);
  auto *clearTextStack = new View();
  clearTextStack->setFlexDirection(FlexDirection::Column);
  clearTextStack->setJustifyContent(YGJustifyCenter);
  clearTextStack->setFlex(1);
  clearTextStack->setMinWidth(0);
  auto *clearLampLabel = makeLabel("CLEAR LAMP", 15, ui_theme::textSecondary());
  clearLampLabel->setHeight(20);
  clearTextStack->addView(clearLampLabel);
  auto *clearTypeText =
      makeLabel(resultState.getClearTypeLabel(), 21, ui_theme::textPrimary());
  clearTypeText->setHeight(30);
  clearTypeText->setName("clearType");
  clearTextStack->addView(clearTypeText);
  clearBadge->addView(clearTextStack);
  header->addView(clearBadge);
  rootLayout->addView(header);

  auto scoreContainer = new View();
  scoreContainer->setFlexDirection(FlexDirection::Row);
  scoreContainer->setGap(12);
  scoreContainer->setAlignItems(YGAlignStretch);
  scoreContainer->setHeight(160);
  scoreContainer->setName("scoreContainer");

  auto *gradePanel = makePanel(
      ui_theme::resultPanelStrong(), ui_theme::withAlpha(ui_theme::coral(), 150));
  gradePanel->setWidth(300);
  gradePanel->setFlexDirection(FlexDirection::Column);
  gradePanel->setAlignItems(YGAlignCenter);
  gradePanel->setJustifyContent(YGJustifyCenter);
  gradePanel->setGap(8);
  auto gradeLabel = makeLabel("GRADE", 19, ui_theme::textSecondary());
  gradeLabel->setAlign(TextView::CENTER);
  gradePanel->addView(gradeLabel);
  auto gradeText = new TextView("assets/fonts/notosanscjkjp.ttf", 86);
  gradeText->setText(grade);
  gradeText->setColor(ui_theme::sdl(ui_theme::amber()));
  gradeText->setAlign(TextView::CENTER);
  gradeText->setHeight(96);
  gradeText->setName("grade");
  gradePanel->addView(gradeText);
  scoreContainer->addView(gradePanel);

  auto *scoreDetailView =
      makePanel(ui_theme::resultPanel(), ui_theme::hairlineSubtle());
  scoreDetailView->setFlex(1);
  scoreDetailView->setFlexDirection(FlexDirection::Column);
  scoreDetailView->setJustifyContent(YGJustifyCenter);
  scoreDetailView->setPadding(Edge::All, 22);
  scoreDetailView->setGap(6);
  auto scoreLabel = makeLabel("SCORE", 19, ui_theme::textSecondary());
  scoreDetailView->addView(scoreLabel);
  auto scoreText = new TextView("assets/fonts/notosanscjkjp.ttf", 56);
  scoreText->setText(std::to_string(currentScore));
  scoreText->setColor(ui_theme::sdl(ui_theme::textPrimary()));
  scoreText->setHeight(64);
  scoreText->setName("score");
  scoreDetailView->addView(scoreText);

  auto comboText =
      makeLabel("MAX COMBO " + std::to_string(resultState.maxCombo), 25,
                ui_theme::lime());
  comboText->setText("Max Combo: " + std::to_string(resultState.maxCombo));
  comboText->setName("maxCombo");
  scoreDetailView->addView(comboText);
  scoreContainer->addView(scoreDetailView);

  rootLayout->addView(scoreContainer);

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
                           Color(accent.r, accent.g, accent.b, 112));
    tile->setWidth(150);
    tile->setHeight(68);
    tile->setPadding(Edge::All, 8);
    tile->setFlexDirection(FlexDirection::Column);
    tile->setJustifyContent(YGJustifyCenter);

    auto *labelView = makeLabel(label, 13, ui_theme::textSecondary());
    labelView->setHeight(18);
    labelView->setOverflow(TextView::TextOverflow::Hidden);
    tile->addView(labelView);

    auto *valueView = makeLabel(value, 23, valueColor);
    valueView->setHeight(28);
    valueView->setOverflow(TextView::TextOverflow::Hidden);
    valueView->setName(label);
    tile->addView(valueView);

    auto *subView = makeLabel(subValue, 14, accent);
    subView->setHeight(18);
    subView->setOverflow(TextView::TextOverflow::Hidden);
    tile->addView(subView);
    infoGrid->addView(tile);
  };

  const auto nextRank = nextRankTarget(currentScore, maxScore);
  const auto hasPreviousBest =
      data != nullptr && data->previousBest.has_value();
  const std::string longNoteSummary =
      meta.TotalLongNotes > 0 ? std::to_string(meta.TotalLongNotes) + " LN"
                              : "";
  const int playLevelDecimals =
      std::abs(meta.PlayLevel - std::round(meta.PlayLevel)) < 0.01 ? 0 : 1;
  const std::string playLevelLabel =
      "LV " + formatNumber(meta.PlayLevel, playLevelDecimals);
  if (hasPreviousBest) {
    const int delta = currentScore - data->previousBest->score;
    addInfoTile("BEST SCORE", std::to_string(data->previousBest->score),
                formatSignedDelta(delta),
                delta >= 0 ? ui_theme::lime() : ui_theme::coral());
  } else {
    addInfoTile("BEST SCORE", "NO PLAY", "", ui_theme::textSecondary(),
                ui_theme::textSecondary());
  }
  addInfoTile("NEXT RANK", nextRank.first, formatSignedDelta(nextRank.second),
              ui_theme::amber());
  addInfoTile("SCORE RATE", formatScoreRate(currentScore, maxScore), "",
              ui_theme::cyan());
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
