#include "DefaultSkin.h"
#include <sstream>
#include "../view/TextView.h"
#include "../view/Button.h"

void DefaultSkin::buildLayout(const std::string& screenName, View* root, void* data) {
    if (screenName == "Result") {
        buildResultLayout(root, static_cast<ResultSkinData*>(data));
    }
}

void DefaultSkin::buildResultLayout(View* rootLayout, ResultSkinData* data) {
  const auto& meta = *data->meta;
  const auto& resultState = *data->state;
  auto& context = *data->context;

  rootLayout->setFlexDirection(FlexDirection::Column);
  rootLayout->setAlignItems(YGAlignCenter);
  rootLayout->setJustifyContent(YGJustifyCenter);
  rootLayout->setPadding(Edge::All, 20);
  rootLayout->setGap(10);

  // Title & Artist
  auto titleText = new TextView("assets/fonts/notosanscjkjp.ttf", 48);
  titleText->setText(meta.Title);
  titleText->setAlign(TextView::CENTER);
  titleText->setName("title");
  rootLayout->addView(titleText);

  auto artistText = new TextView("assets/fonts/notosanscjkjp.ttf", 32);
  artistText->setText(meta.Artist);
  artistText->setAlign(TextView::CENTER);
  artistText->setName("artist");
  rootLayout->addView(artistText);

  // Score Container
  auto scoreContainer = new View();
  scoreContainer->setFlexDirection(FlexDirection::Row);
  scoreContainer->setGap(50);
  scoreContainer->setAlignItems(YGAlignCenter);
  scoreContainer->setName("scoreContainer");

  // Grade
  // Calculate Grade
  int totalNotes = meta.TotalNotes;
  int maxScore = totalNotes * 2;
  int currentScore = resultState.getScore();
  double percentage = maxScore > 0 ? (double)currentScore / maxScore : 0.0;
  std::string grade = "F";
  if (percentage >= 8.0/9.0)
    grade = "AAA";
  else if (percentage >= 7.0/9.0)
    grade = "AA";
  else if (percentage >= 6.0/9.0)
    grade = "A";
  else if (percentage >= 5.0/9.0)
    grade = "B";
  else if (percentage >= 4.0/9.0)
    grade = "C";
  else if (percentage >= 3.0/9.0)
    grade = "D";
  else if (percentage >= 2.0/9.0)
    grade = "E";
  else
    grade = "F";

  auto gradeText = new TextView("assets/fonts/notosanscjkjp.ttf", 96);
  gradeText->setText(grade);
  gradeText->setColor({255, 200, 50, 255}); // Gold-ish
  gradeText->setName("grade");
  scoreContainer->addView(gradeText);

  auto scoreDetailView = new View();
  scoreDetailView->setFlexDirection(FlexDirection::Column);
  auto scoreText = new TextView("assets/fonts/notosanscjkjp.ttf", 48);
  scoreText->setText(std::to_string(currentScore));
  scoreText->setName("score");
  scoreDetailView->addView(scoreText);

  auto comboText = new TextView("assets/fonts/notosanscjkjp.ttf", 32);
  comboText->setText("Max Combo: " + std::to_string(resultState.maxCombo));
  comboText->setName("maxCombo");
  scoreDetailView->addView(comboText);
  scoreContainer->addView(scoreDetailView);

  rootLayout->addView(scoreContainer);

  // Judgements
  auto detailsGrid = new View();
  detailsGrid->setFlexDirection(FlexDirection::Row);
  detailsGrid->setGap(40);
  detailsGrid->setName("detailsGrid");
  
  auto leftCol = new View();
  leftCol->setFlexDirection(FlexDirection::Column);
  
  auto addJudgeText = [&](std::string label, int count, SDL_Color color, std::string id) {
      std::stringstream ss;
      ss << label << ": " << count;
      auto tv = new TextView("assets/fonts/notosanscjkjp.ttf", 24);
      tv->setText(ss.str());
      tv->setColor(color);
      tv->setName(id);
      leftCol->addView(tv);
  };
  
  addJudgeText("PGREAT", resultState.judgeCount.at(PGreat), {200, 255, 255, 255}, "pgreat");
  addJudgeText("GREAT", resultState.judgeCount.at(Great), {200, 255, 200, 255}, "great");
  addJudgeText("GOOD", resultState.judgeCount.at(Good), {200, 200, 255, 255}, "good");
  addJudgeText("BAD", resultState.judgeCount.at(Bad), {255, 200, 200, 255}, "bad");
  addJudgeText("POOR", resultState.judgeCount.at(Poor), {255, 100, 100, 255}, "poor");
  addJudgeText("MISS", resultState.judgeCount.at(Kpoor), {255, 50, 50, 255}, "miss");
  addJudgeText("BREAK", resultState.comboBreak, {255, 50, 50, 255}, "break");
  
  detailsGrid->addView(leftCol);
  
  auto rightCol = new View();
  rightCol->setFlexDirection(FlexDirection::Column);
  
  auto fastSlowText = new TextView("assets/fonts/notosanscjkjp.ttf", 24);
  std::stringstream fss;
  fss << "FAST: " << resultState.fastCount << "\nSLOW: " << resultState.slowCount;
  fastSlowText->setText(fss.str());
  fastSlowText->setName("fastSlow");
  rightCol->addView(fastSlowText);
  
  detailsGrid->addView(rightCol);
  rootLayout->addView(detailsGrid);

  // Graph Placeholder
  auto graphPlaceHolder = new View();
  graphPlaceHolder->setHeight(200);
  graphPlaceHolder->setWidth(rendering::window_width - 100);
  graphPlaceHolder->setName("graph");
  rootLayout->addView(graphPlaceHolder);

  // Buttons
  auto btn = new Button(0, 0, 300, 80);
  auto btnText = new TextView("assets/fonts/notosanscjkjp.ttf", 32);
  btnText->setText("Back to Menu");
  btnText->setAlign(TextView::CENTER);
  btn->setContentView(btnText);
  btn->setName("backButton");
  btn->setOnClickListener([&context]() {
    context.sceneManager->changeScene("MainMenu");
  });
  rootLayout->addView(btn);
}

void DefaultSkin::buildGameContext(View* root, void* data) {
    // Future implementation
}
