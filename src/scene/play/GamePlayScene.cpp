//
// Created by XF on 8/25/2024.
//

#include "GamePlayScene.h"
#include "../../view/TextView.h"
#include "BMSRenderer.h"
#include "../../input/RhythmInputHandler.h"
#include "../../view/Button.h"
#include "../../view/Button.h"
#include "../../scene/MainMenuScene.h"
#include "../ResultScene.h"
void GamePlayScene::init() {
  auto chartNameText = new TextView("assets/fonts/notosanscjkjp.ttf", 32);
  chartNameText->setText(chart->Meta.Title);
  chartNameText->setPosition(10, 10);
  addView(chartNameText);
  renderer = new BMSRenderer(chart, judge.timingWindows[Bad].second);
  context.jukebox.stop();
  // NOTE: should be set before "reset" call to avoid race condition with onTick
  // callback call
  context.jukebox.onTick([this](long long time) {
    if (state != nullptr && state->isPlaying) {
      checkPassedTimeline(time);
      if (state->passedMeasureCount == chart->Measures.size()) {
        SDL_Log("All measures passed");
        defer(
            [this]() {
              context.sceneManager->changeScene(
                  new ResultScene(context, chart->Meta, *state));
              return false;
            },
            2000, true);
      }
    }
  });
  reset();
  inputHandler = new RhythmInputHandler(this, chart->Meta);
  inputHandler->startListenSDL();
  inputHandler->startListenTouch();
  laneStateText = new TextView("assets/fonts/notosanscjkjp.ttf", 32);
  laneStateText->setPosition(100, 100);
  for (const auto &lane : chart->Meta.GetTotalLaneIndices()) {
    SDL_Log("Setting lane %d to false", lane);
    lanePressed[lane] = false;
  }

  /* pause screen */
  pauseLayout =
      new View(0, 0, rendering::window_width, rendering::window_height);
  pauseLayout->setFlexDirection(FlexDirection::Column);
  pauseLayout->setAlignItems(YGAlignCenter);
  {
    auto pauseScreen = new View();
    pauseScreen->setFlex(1);
    pauseScreen->setFlexDirection(FlexDirection::Column);
    pauseScreen->setAlignItems(YGAlignCenter);
    pauseScreen->setJustifyContent(YGJustifyCenter);
    {
      auto pauseText = new TextView("assets/fonts/notosanscjkjp.ttf", 32);
      pauseText->setSize(200, 100);
      pauseText->setText("Paused");
      pauseText->setAlign(TextView::CENTER);
      pauseScreen->addView(pauseText);
      auto resumeButton = new Button();
      auto resumeText = new TextView("assets/fonts/notosanscjkjp.ttf", 32);
      resumeText->setText("Resume");
      resumeText->setAlign(TextView::CENTER);
      resumeButton->setContentView(resumeText);
      resumeButton->setOnClickListener([this]() {
        context.jukebox.resume();
        pauseLayout->setVisible(false);
      });
      resumeButton->setSize(200, 100);
      pauseScreen->addView(resumeButton);
      auto restartButton = new Button();
      auto restartText = new TextView("assets/fonts/notosanscjkjp.ttf", 32);
      restartText->setText("Restart");
      restartText->setAlign(TextView::CENTER);
      restartButton->setContentView(restartText);
      restartButton->setOnClickListener([this]() {
        pauseLayout->setVisible(false);
        defer(
            [this]() {
              reset();
              return true;
            },
            0, true);
      });
      restartButton->setSize(200, 100);
      pauseScreen->addView(restartButton);
      auto exitButton = new Button();
      auto exitText = new TextView("assets/fonts/notosanscjkjp.ttf", 32);
      exitText->setText("Exit");
      exitText->setAlign(TextView::CENTER);
      exitButton->setContentView(exitText);
      exitButton->setOnClickListener([this]() {
        context.jukebox.stop();
        defer(
            [this]() {
              context.sceneManager->changeScene("MainMenu");
              return false;
            },
            0, true);
      });
      exitButton->setSize(200, 100);
      pauseScreen->addView(exitButton);
    }

    pauseLayout->addView(pauseScreen);
  }
  pauseLayout->setVisible(false);
  addView(pauseLayout);

  /* pause button */
  pauseButton = new Button(rendering::window_width - 70, 50, 40, 40);
  auto pauseText = new TextView("assets/fonts/notosanscjkjp.ttf", 32);
  pauseText->setText("| |");
  pauseText->setAlign(TextView::CENTER);
  pauseButton->setContentView(pauseText);
  pauseButton->setOnClickListener([this]() {
    context.jukebox.pause();
    pauseLayout->setVisible(true);
  });
  addView(pauseButton);
}

void GamePlayScene::reset() {
  if (state != nullptr) {
    delete state;
    state = nullptr;
  }
  renderer->reset();
  // reset all notes
  for (const auto &measure : chart->Measures) {
    for (const auto &timeline : measure->TimeLines) {
      for (const auto &note : timeline->Notes) {
        if (note == nullptr) {
          continue;
        }
        note->Reset();
      }
    }
  }
  context.jukebox.stop();
  context.jukebox.schedule(*chart, false, isCancelled);
  context.jukebox.play();
  state = new RhythmState(chart, false);
  state->isPlaying = true;
}
void GamePlayScene::update(float dt) {}

void GamePlayScene::renderScene() {
  RenderContext renderContext;
  pauseLayout->setSize(rendering::window_width, rendering::window_height);
  // pauseButton->setPosition(rendering::window_width - 40, 10);
  renderer->render(renderContext, context.jukebox.getTimeMicros());
  context.jukebox.render();
  std::string str;
  for (auto &[lane, pressed] : lanePressed) {
    str += std::to_string(pressed) + "\n";
  }
  laneStateText->setText(str);
  laneStateText->render(renderContext);
}
void GamePlayScene::cleanupScene() {
  SDL_Log("Cleaning up GamePlayScene");
  context.jukebox.removeOnTick();
  SDL_Log("Stopping input handler");
  inputHandler->stopListen();
  delete inputHandler;
  inputHandler = nullptr;
  delete renderer;
  renderer = nullptr;
  delete laneStateText;
  laneStateText = nullptr;
  SDL_Log("Cleaned up GamePlayScene");
}
bms_parser::Note *GamePlayScene::pressLane(int lane, double inputDelay) {
  return pressLane(lane, lane, inputDelay);
}
bms_parser::Note *GamePlayScene::pressLane(int mainLane, int compensateLane,
                                           double inputDelay) {
  if (context.jukebox.isPaused()) {
    return nullptr;
  }
  std::vector<int> candidates;
  if (lanePressed.contains(mainLane) && !lanePressed[mainLane]) {
    candidates.push_back(mainLane);
  }
  if (lanePressed.contains(compensateLane) && !lanePressed[compensateLane]) {
    candidates.push_back(compensateLane);
  }
  if (candidates.empty()) {
    return nullptr;
  }

  if (isGamePaused) {
    return nullptr;
  }

  if (state == nullptr) {
    lanePressed[mainLane] = true;
    renderer->onLanePressed(
        mainLane, JudgeResult(None, 0),
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    return nullptr;
  }
  if (!state->isPlaying) {
    lanePressed[mainLane] = true;
    return nullptr;
  }

  const auto &measures = chart->Measures;
  const auto pressedTime = context.jukebox.getTimeMicros() -
                           static_cast<long long>(inputDelay * 1000000);
  for (size_t i = state->passedMeasureCount; i < measures.size(); i++) {
    const bool isFirstMeasure = i == state->passedMeasureCount;
    const auto &measure = measures[i];

    for (size_t j = isFirstMeasure ? state->passedTimelineCount : 0;
         j < measure->TimeLines.size(); j++) {
      const auto &timeline = measure->TimeLines[j];
      if (timeline->Timing < pressedTime - latePoorTiming) {
        continue;
      }
      for (auto lane : candidates) {
        const auto &note = timeline->Notes[lane];
        if (note == nullptr) {
          continue;
        }
        if (note->IsPlayed) {
          continue;
        }
        if (note->IsLandmineNote()) {
          continue;
        }
        if (judge.judgeNow(note, pressedTime).judgement == None) {
          continue;
        }
        const JudgeResult judgement = pressNote(note, pressedTime);
        lanePressed[lane] = true;
        renderer->onLanePressed(
            lane, judgement,
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
        return note;
      }
    }
  }
  lanePressed[mainLane] = true;
  renderer->onLanePressed(
      mainLane, JudgeResult(None, 0),
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  return nullptr;
}
bms_parser::Note *GamePlayScene::releaseLane(int lane, double inputDelay) {
  if (!lanePressed.contains(lane) || !lanePressed[lane]) {
    return nullptr;
  }
  lanePressed[lane] = false;
  renderer->onLaneReleased(
      lane, std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
  const auto releasedTime = context.jukebox.getTimeMicros() -
                            static_cast<long long>(inputDelay * 1000000);

  if (state == nullptr) {
    return nullptr;
  }
  if (!state->isPlaying) {
    return nullptr;
  }

  const auto &Measures = chart->Measures;

  for (size_t i = state->passedMeasureCount; i < Measures.size(); i++) {
    const bool isFirstMeasure = i == state->passedMeasureCount;
    const auto &measure = Measures[i];
    for (size_t j = isFirstMeasure ? state->passedTimelineCount : 0;
         j < measure->TimeLines.size(); j++) {
      const auto &Timeline = measure->TimeLines[j];
      if (Timeline->Timing < releasedTime - latePoorTiming) {
        continue;
      }
      const auto &note = Timeline->Notes[lane];
      if (note == nullptr) {
        continue;
      }
      if (note->IsPlayed) {
        continue;
      }
      releaseNote(note, releasedTime);
      return note;
    }
  }
  return nullptr;
}
void GamePlayScene::checkPassedTimeline(long long time) {
  auto measures = chart->Measures;
  if (state == nullptr) {
    return;
  }
  int totalLoopCount = 0;
  for (size_t i = state->passedMeasureCount; i < measures.size(); i++) {
    const bool isFirstMeasure = i == state->passedMeasureCount;
    const auto &measure = measures[i];
    for (size_t j = isFirstMeasure ? state->passedTimelineCount : 0;
         j < measure->TimeLines.size(); j++) {
      totalLoopCount++;
      const auto &timeline = measure->TimeLines[j];
      if (timeline->Timing < time - latePoorTiming) {
        if (isFirstMeasure) {
          state->passedTimelineCount++;
        }
        // make remaining notes POOR
        for (const auto &note : timeline->Notes) {
          if (note == nullptr) {
            continue;
          }
          if (note->IsPlayed) {
            continue;
          }
          if (note->IsLandmineNote()) {
            continue;
          }
          if (note->IsLongNote()) {
            const auto &longNote = static_cast<bms_parser::LongNote *>(note);
            if (!longNote->IsTail()) {
              longNote->MissPress(time);
            }
          }
          const auto poorResult = JudgeResult(Poor, time - timeline->Timing);
          onJudge(poorResult);
        }
      } else if (timeline->Timing <= time) {
        // auto-release long notes
        for (const auto &note : timeline->Notes) {
          if (note == nullptr) {
            continue;
          }
          if (note->IsPlayed) {
            continue;
          }
          if (note->IsLandmineNote()) {
            // TODO: if lane is being pressed, detonate landmine
            continue;
          }
          if (note->IsLongNote()) {
            const auto &longNote = static_cast<bms_parser::LongNote *>(note);
            if (longNote->IsTail()) {
              if (!longNote->IsHolding) {
                continue;
              }
              longNote->Release(time);
              const auto judgeResult =
                  judge.judgeNow(longNote->Head, longNote->Head->PlayedTime);
              onJudge(judgeResult);
              if (options.autoPlay) {
                renderer->onLaneReleased(
                    note->Lane,
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count());
              }
              continue;
            }
          }
          if (options.autoPlay) // NormalNote or LongNote's head
          {
            const JudgeResult judgeResult = pressNote(note, time);
            renderer->onLanePressed(note->Lane, judgeResult, time);
            if (!note->IsLongNote()) {
              renderer->onLaneReleased(
                  note->Lane,
                  std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count());
            }
          }
        }
      } else {
        i = measures.size();
        break;
      }
    }
    if (state->passedTimelineCount == measure->TimeLines.size() &&
        isFirstMeasure) {
      state->passedMeasureCount++;
      state->passedTimelineCount = 0;
    }
  }
}

void GamePlayScene::onJudge(const JudgeResult &judgeResult) {
  std::lock_guard<std::mutex> lock(judgeMutex);
  state->latestJudgeResult = judgeResult;

  state->judgeCount[judgeResult.judgement]++;
  if (judgeResult.isComboBreak()) {
    state->combo = 0;
    state->comboBreak++;
  } else if (judgeResult.judgement != Kpoor) {
    state->combo++;
    if (state->combo > state->maxCombo) {
      state->maxCombo = state->combo;
    }
  }
  renderer->onJudge(judgeResult, state->combo, state->getScore());
  // CurrentRhythmHUD->OnJudge(state);
  // UE_LOG(LogTemp, Warning, TEXT("Judge: %s, Combo: %d, Diff: %lld"),
  // *JudgeResult.ToString(), state->Combo, JudgeResult.Diff);

  if (judgeResult.judgement != None && judgeResult.judgement != Kpoor) {
    if (judgeResult.Diff < 0)
      state->fastCount++;
    else if (judgeResult.Diff > 0)
      state->slowCount++;
  }

  // TODO: implement standard groove gauge system

  if (judgeResult.judgement == PGreat || judgeResult.judgement == Great) {
    state->currentGauge = std::min(100.0f, state->currentGauge + 0.1f);
  } else if (judgeResult.judgement == Good) {
    state->currentGauge = std::max(0.0f, state->currentGauge - 0.5f);
  } else {
    state->currentGauge = std::max(0.0f, state->currentGauge - 2.0f);
  }
  state->gaugeHistory.push_back(state->currentGauge);
}

JudgeResult GamePlayScene::pressNote(bms_parser::Note *note,
                                     long long pressedTime) {
  if (note->Wav != bms_parser::Parser::NoWav && !options.autoKeySound) {
    context.jukebox.playKeySound(note->Wav);
  }
  const auto judgeResult = judge.judgeNow(note, pressedTime);
  if (judgeResult.judgement != None) {
    if (judgeResult.isNotePlayed()) {
      // TODO: play keybomb
      if (note->IsLongNote()) {
        if (const auto &longNote = static_cast<bms_parser::LongNote *>(note);
            !longNote->IsTail()) {
          longNote->Press(pressedTime);
        }
        return judgeResult;
      }
      note->Press(pressedTime);
    }
    onJudge(judgeResult);
  }
  return judgeResult;
}

void GamePlayScene::releaseNote(bms_parser::Note *Note,
                                long long ReleasedTime) {
  if (!Note->IsLongNote()) {
    return;
  }
  const auto &LongNote = static_cast<bms_parser::LongNote *>(Note);
  if (!LongNote->IsTail()) {
    return;
  }
  if (!LongNote->IsHolding) {
    return;
  }
  LongNote->Release(ReleasedTime);
  const auto judgeResult = judge.judgeNow(LongNote, ReleasedTime);
  // if tail judgement is not good/great/pgreat, make it bad
  if (judgeResult.judgement == None || judgeResult.judgement == Kpoor ||
      judgeResult.judgement == Poor) {
    onJudge(JudgeResult(Bad, judgeResult.Diff));
    return;
  }
  // otherwise, follow the head's judgement
  const auto HeadJudgeResult =
      judge.judgeNow(LongNote->Head, LongNote->Head->PlayedTime);
  onJudge(HeadJudgeResult);
}

EventHandleResult GamePlayScene::handleEvents(SDL_Event &event) {
  Scene::handleEvents(event);
  if (event.type == SDL_KEYDOWN) {
    if (event.key.keysym.sym == SDLK_ESCAPE) {
      if (context.jukebox.isPaused()) {
        context.jukebox.resume();
        pauseLayout->setVisible(false);
      } else {
        context.jukebox.pause();
        pauseLayout->setVisible(true);
      }
    }
  }
  return {};
}
