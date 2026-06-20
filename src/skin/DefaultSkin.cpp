#include "DefaultSkin.h"
#include <sstream>
#include <algorithm>
#include "../view/TextView.h"
#include "../view/Button.h"
#include "../view/UiTheme.h"

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
  rootLayout->setPadding(Edge::All, 48);
  rootLayout->setGap(18);
  rootLayout->setBackgroundColor(ui_theme::backdrop());

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
    panel->setShadow(ui_theme::shadow(), 0, 10, 16);
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

  auto *header = new View();
  header->setFlexDirection(FlexDirection::Row);
  header->setAlignItems(YGAlignCenter);
  header->setGap(20);
  header->setMinHeight(94);

  auto *titleStack = new View();
  titleStack->setFlexDirection(FlexDirection::Column);
  titleStack->setFlex(1);
  titleStack->setGap(4);

  auto titleText = new TextView("assets/fonts/notosanscjkjp.ttf", 42);
  titleText->setText(meta.Title);
  titleText->setColor(ui_theme::sdl(ui_theme::textPrimary()));
  titleText->setOverflow(TextView::TextOverflow::Marquee);
  titleText->setHeight(52);
  titleText->setName("title");
  titleStack->addView(titleText);

  auto artistText = new TextView("assets/fonts/notosanscjkjp.ttf", 24);
  artistText->setText(meta.Artist);
  artistText->setColor(ui_theme::sdl(ui_theme::textSecondary()));
  artistText->setHeight(34);
  artistText->setName("artist");
  titleStack->addView(artistText);
  header->addView(titleStack);

  auto *clearBadge = makePanel(
      Color(ui_theme::amber().r, ui_theme::amber().g, ui_theme::amber().b, 42),
      ui_theme::amber());
  clearBadge->setWidth(260);
  clearBadge->setHeight(70);
  clearBadge->setPadding(Edge::All, 12);
  auto *clearTypeText =
      makeLabel(resultState.getClearTypeLabel(), 24, ui_theme::textPrimary());
  clearTypeText->setAlign(TextView::CENTER);
  clearTypeText->setName("clearType");
  clearBadge->addView(clearTypeText);
  header->addView(clearBadge);
  rootLayout->addView(header);

  auto scoreContainer = new View();
  scoreContainer->setFlexDirection(FlexDirection::Row);
  scoreContainer->setGap(18);
  scoreContainer->setAlignItems(YGAlignStretch);
  scoreContainer->setHeight(230);
  scoreContainer->setName("scoreContainer");

  auto *gradePanel = makePanel(ui_theme::panelStrong(), ui_theme::coral());
  gradePanel->setWidth(360);
  gradePanel->setFlexDirection(FlexDirection::Column);
  gradePanel->setAlignItems(YGAlignCenter);
  gradePanel->setJustifyContent(YGJustifyCenter);
  gradePanel->setGap(8);
  auto gradeLabel = makeLabel("GRADE", 22, ui_theme::textSecondary());
  gradeLabel->setAlign(TextView::CENTER);
  gradePanel->addView(gradeLabel);
  auto gradeText = new TextView("assets/fonts/notosanscjkjp.ttf", 110);
  gradeText->setText(grade);
  gradeText->setColor(ui_theme::sdl(ui_theme::amber()));
  gradeText->setAlign(TextView::CENTER);
  gradeText->setHeight(124);
  gradeText->setName("grade");
  gradePanel->addView(gradeText);
  scoreContainer->addView(gradePanel);

  auto *scoreDetailView = makePanel(ui_theme::panel(), ui_theme::hairline());
  scoreDetailView->setFlex(1);
  scoreDetailView->setFlexDirection(FlexDirection::Column);
  scoreDetailView->setJustifyContent(YGJustifyCenter);
  scoreDetailView->setPadding(Edge::All, 28);
  scoreDetailView->setGap(10);
  auto scoreLabel = makeLabel("SCORE", 22, ui_theme::textSecondary());
  scoreDetailView->addView(scoreLabel);
  auto scoreText = new TextView("assets/fonts/notosanscjkjp.ttf", 68);
  scoreText->setText(std::to_string(currentScore));
  scoreText->setColor(ui_theme::sdl(ui_theme::textPrimary()));
  scoreText->setHeight(82);
  scoreText->setName("score");
  scoreDetailView->addView(scoreText);

  auto comboText =
      makeLabel("MAX COMBO " + std::to_string(resultState.maxCombo), 30,
                ui_theme::lime());
  comboText->setText("Max Combo: " + std::to_string(resultState.maxCombo));
  comboText->setName("maxCombo");
  scoreDetailView->addView(comboText);
  scoreContainer->addView(scoreDetailView);

  rootLayout->addView(scoreContainer);

  auto detailsGrid = new View();
  detailsGrid->setFlexDirection(FlexDirection::Row);
  detailsGrid->setFlexWrap(YGWrapWrap);
  detailsGrid->setGap(12);
  detailsGrid->setName("detailsGrid");

  auto addMetric = [&](const std::string &label, int count, Color accent,
                       const std::string &id) {
    auto *tile = makePanel(ui_theme::panelSubtle(),
                           Color(accent.r, accent.g, accent.b, 190));
    tile->setWidth(190);
    tile->setHeight(82);
    tile->setPadding(Edge::All, 10);
    tile->setFlexDirection(FlexDirection::Column);
    tile->setJustifyContent(YGJustifyCenter);
    auto *labelView = makeLabel(label, 17, ui_theme::textSecondary());
    labelView->setHeight(24);
    tile->addView(labelView);
    auto *valueView = makeLabel(std::to_string(count), 30, accent);
    valueView->setName(id);
    tile->addView(valueView);
    detailsGrid->addView(tile);
  };

  addMetric("PGREAT", countFor(PGreat), ui_theme::cyan(), "pgreat");
  addMetric("GREAT", countFor(Great), ui_theme::lime(), "great");
  addMetric("GOOD", countFor(Good), ui_theme::amber(), "good");
  addMetric("BAD", countFor(Bad), Color(255, 132, 96, 255), "bad");
  addMetric("POOR", countFor(Poor), ui_theme::coral(), "poor");
  addMetric("KPOOR", countFor(Kpoor), Color(255, 78, 102, 255), "kpoor");
  addMetric("BREAK", resultState.comboBreak, ui_theme::coral(), "break");
  addMetric("FAST", resultState.fastCount, ui_theme::cyan(), "fast");
  addMetric("SLOW", resultState.slowCount, ui_theme::amber(), "slow");

  rootLayout->addView(detailsGrid);

  auto graphPlaceHolder = new View();
  graphPlaceHolder->setHeight(210);
  graphPlaceHolder->setWidthPercent(100);
  graphPlaceHolder->setBackgroundColor(ui_theme::panelSubtle());
  graphPlaceHolder->setCornerRadius(ui_theme::panelRadius());
  graphPlaceHolder->setShadow(ui_theme::shadow(), 0, 10, 16);
  graphPlaceHolder->setBorderColor(ui_theme::hairline());
  graphPlaceHolder->setBorderWidth(1);
  graphPlaceHolder->setName("graph");
  rootLayout->addView(graphPlaceHolder);

  auto btn = new Button(0, 0, 300, 80);
  auto btnText = new TextView("assets/fonts/notosanscjkjp.ttf", 26);
  btnText->setText("Back to Menu");
  btnText->setAlign(TextView::CENTER);
  btnText->setVAlign(TextView::MIDDLE);
  btnText->setColor(ui_theme::sdl(ui_theme::textOn(ui_theme::primaryAction())));
  btn->setContentView(btnText);
  btn->setName("backButton");
  btn->setSize(300, 64);
  btn->setCornerRadius(ui_theme::controlRadius());
  btn->setBackgroundColors(ui_theme::primaryAction(),
                           ui_theme::primaryActionHover(),
                           ui_theme::primaryActionPressed());
  btn->setBorderColors(ui_theme::cyan(), ui_theme::cyan(),
                       Color(255, 255, 255, 255));
  btn->setStyledBorderWidth(1);
  btn->setOnClickListener(
      [&context]() { context.sceneManager->changeScene("MainMenu"); });
  rootLayout->addView(btn);
}

void DefaultSkin::buildGameContext(View *root, void *data) {
  // Future implementation
}
