//
// Created by XF on 8/25/2024.
//

#include "GamePlayScene.h"
#include "../../view/TextView.h"
#include "../../view/ClearLampColors.h"
#include "BMSRenderer.h"
#include "../../input/RhythmInputHandler.h"
#include "../../targets.h"
#include "../../view/Button.h"
#include "../../scene/MainMenuScene.h"
#include "../ResultScene.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <string>

namespace {
long long nowMicros() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::string replayNoteKey(int lane, long long noteTimeMicros) {
  return std::to_string(lane) + ":" + std::to_string(noteTimeMicros);
}

#if defined(DEBUG) || defined(_DEBUG)
constexpr bool kShowLaneStateOverlay = true;
#else
constexpr bool kShowLaneStateOverlay = false;
#endif
} // namespace

void GamePlayScene::init() {
  auto chartNameText = new TextView("assets/fonts/notosanscjkjp.ttf", 32);
  chartNameText->setText(chart->Meta.Title);
  chartNameText->setPosition(10, 10);
  addView(chartNameText);
  gaugeStatusText = new TextView("assets/fonts/notosanscjkjp.ttf", 24);
  gaugeStatusText->setPosition(10, 50);
  gaugeStatusText->setText("Gauge: NORMAL 20.0%");
  renderer = new BMSRenderer(chart, judge.timingWindows[Bad].second,
                             context.settings.visibleTimeGreenNumber);
  context.jukebox.stop();
  reset();
  if (!isReplayPlayback()) {
    inputHandler = new RhythmInputHandler(this, chart->Meta);
    inputHandler->discardPendingTouchEvents();
    inputHandler->startListenSDL();
#if !(TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR)
    inputHandler->startListenTouch();
#endif
  }

  for (const auto &lane : chart->Meta.GetTotalLaneIndices()) {
    lanePressed[lane] = false;
  }

  if constexpr (kShowLaneStateOverlay) {
    laneStateText = new TextView("assets/fonts/notosanscjkjp.ttf", 32);
    laneStateText->setPosition(100, 100);
    updateLaneStateText();
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
  context.jukebox.schedule(*chart, options.autoKeySound && !isReplayPlayback(),
                           isCancelled);
  context.jukebox.play();
  state = new RhythmState(chart, false);
  const GaugeType initialGaugeType =
      isReplayPlayback() ? options.replayData->initialGaugeType
                         : options.gaugeType;
  const bool gaugeAutoShift =
      isReplayPlayback() ? options.replayData->gaugeAutoShift
                         : options.gaugeAutoShift;
  state->configureGauge(initialGaugeType, gaugeAutoShift);
  state->isPlaying = true;
  replayEventCursor = 0;
  buildReplayNoteLookup();
  beginReplayRecording();
  updateGaugeStatusText();
}

bool GamePlayScene::isReplayPlayback() const {
  return options.replayData != nullptr;
}

bool GamePlayScene::shouldRecordReplay() const {
  return !options.autoPlay && !isReplayPlayback();
}

void GamePlayScene::beginReplayRecording() {
  if (!shouldRecordReplay()) {
    recordedReplay = {};
    return;
  }

  recordedReplay = {};
  recordedReplay.chartMeta = chart->Meta;
  recordedReplay.initialGaugeType = options.gaugeType;
  recordedReplay.gaugeAutoShift = options.gaugeAutoShift;
  recordedReplay.finalScore = 0;
  recordedReplay.finalGauge = state != nullptr ? state->currentGauge : 0.0f;
  recordedReplay.clearType = kClearTypeFailedRank;
  recordedReplay.events.reserve(
      static_cast<size_t>(std::max(0, chart->Meta.TotalNotes)) * 2);
}

void GamePlayScene::finishReplayRecording() {
  if (!shouldRecordReplay() || state == nullptr) {
    return;
  }

  recordedReplay.finalScore = state->getScore();
  recordedReplay.finalGauge = state->currentGauge;
  recordedReplay.clearType = state->getClearTypeRank();
}

long long GamePlayScene::getJudgementOffsetMicros() const {
  if (isReplayPlayback()) {
    return 0;
  }
  return static_cast<long long>(context.settings.inputOffsetMs) * 1000LL;
}

long long GamePlayScene::getInputSongTimeMicros(long long songTimeMicros,
                                                double inputDelay) const {
  return songTimeMicros - static_cast<long long>(inputDelay * 1000000);
}

long long GamePlayScene::getJudgementTimeMicros(long long songTimeMicros,
                                                double inputDelay) const {
  return getInputSongTimeMicros(songTimeMicros, inputDelay) +
         getJudgementOffsetMicros();
}

long long GamePlayScene::getVisualOffsetMicros() const {
  return static_cast<long long>(context.settings.visualOffsetMs) * 1000LL;
}

long long GamePlayScene::getVisualTimeMicros(long long songTimeMicros) const {
  return std::max(0LL, songTimeMicros - getVisualOffsetMicros());
}

void GamePlayScene::update(float dt) {
  (void)dt;
  if (inputHandler != nullptr) {
    inputHandler->pumpPendingTouchEvents();
  }
  if (state == nullptr || !state->isPlaying || state->isEnding) {
    return;
  }

  const long long songTimeMicros = context.jukebox.getTimeMicros();
  if (isReplayPlayback()) {
    processReplayEvents(songTimeMicros);
  }
  checkPassedTimeline(songTimeMicros);
  if (state->passedMeasureCount != chart->Measures.size()) {
    return;
  }

  SDL_Log("All measures passed");
  state->isEnding = true;
  finishReplayRecording();
  defer(
      [this]() {
        context.sceneManager->changeScene(new ResultScene(
            context, chart->Meta, *state,
            shouldRecordReplay() ? &recordedReplay : nullptr,
            !isReplayPlayback()));
        return false;
      },
      2000, true);
}

void GamePlayScene::renderScene() {
  RenderContext renderContext;
  pauseLayout->setSize(rendering::window_width, rendering::window_height);
  // pauseButton->setPosition(rendering::window_width - 40, 10);
  renderer->render(renderContext,
                   getVisualTimeMicros(context.jukebox.getTimeMicros()));
  if (gaugeStatusText != nullptr) {
    gaugeStatusText->render(renderContext);
  }
  if (laneStateText != nullptr) {
    laneStateText->render(renderContext);
  }
}
void GamePlayScene::cleanupScene() {
  SDL_Log("Cleaning up GamePlayScene");
  context.jukebox.removeOnTick();
  SDL_Log("Stopping input handler");
  if (inputHandler != nullptr) {
    inputHandler->stopListen();
    delete inputHandler;
    inputHandler = nullptr;
  }
  delete renderer;
  renderer = nullptr;
  delete gaugeStatusText;
  gaugeStatusText = nullptr;
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
  if (isGamePaused || state == nullptr || !state->isPlaying ||
      state->isEnding) {
    return nullptr;
  }
  auto mainLaneIt = lanePressed.find(mainLane);
  std::array<int, 2> candidates{};
  size_t candidateCount = 0;
  if (mainLaneIt != lanePressed.end() && !mainLaneIt->second) {
    candidates[candidateCount++] = mainLane;
  }
  auto compensateLaneIt = lanePressed.find(compensateLane);
  if (compensateLane != mainLane && compensateLaneIt != lanePressed.end() &&
      !compensateLaneIt->second) {
    candidates[candidateCount++] = compensateLane;
  }
  if (candidateCount == 0) {
    return nullptr;
  }

  const auto &measures = chart->Measures;
  const long long rawSongTime = context.jukebox.getTimeMicros();
  const long long inputSongTime =
      getInputSongTimeMicros(rawSongTime, inputDelay);
  const long long pressedTime = inputSongTime + getJudgementOffsetMicros();
  for (size_t i = state->passedMeasureCount; i < measures.size(); i++) {
    const bool isFirstMeasure = i == state->passedMeasureCount;
    const auto &measure = measures[i];

    for (size_t j = isFirstMeasure ? state->passedTimelineCount : 0;
         j < measure->TimeLines.size(); j++) {
      const auto &timeline = measure->TimeLines[j];
      if (timeline->Timing < pressedTime - latePoorTiming) {
        continue;
      }
      for (size_t candidateIdx = 0; candidateIdx < candidateCount;
           ++candidateIdx) {
        const int lane = candidates[candidateIdx];
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
        const JudgeResult noteJudge = judge.judgeNow(note, pressedTime);
        if (noteJudge.judgement == None) {
          continue;
        }
        const JudgeResult judgement =
            pressNote(note, pressedTime, &noteJudge, inputSongTime);
        if (const auto pressedIt = lanePressed.find(lane);
            pressedIt != lanePressed.end()) {
          pressedIt->second = true;
        }
        updateLaneStateText();
        renderer->onLanePressed(lane, judgement, nowMicros());
        return note;
      }
    }
  }
  if (mainLaneIt != lanePressed.end()) {
    mainLaneIt->second = true;
  }
  updateLaneStateText();
  renderer->onLanePressed(mainLane, JudgeResult(None, 0), nowMicros());
  appendReplayEvent(ReplayEventAction::Press, mainLane, nullptr, inputSongTime,
                    pressedTime, JudgeResult(None, 0));
  return nullptr;
}
bms_parser::Note *GamePlayScene::releaseLane(int lane, double inputDelay) {
  if (isGamePaused || state == nullptr || !state->isPlaying ||
      state->isEnding) {
    return nullptr;
  }
  auto laneIt = lanePressed.find(lane);
  if (laneIt == lanePressed.end() || !laneIt->second) {
    return nullptr;
  }
  laneIt->second = false;
  updateLaneStateText();
  renderer->onLaneReleased(lane, nowMicros());
  const long long rawSongTime = context.jukebox.getTimeMicros();
  const long long inputSongTime =
      getInputSongTimeMicros(rawSongTime, inputDelay);
  const long long releasedTime = inputSongTime + getJudgementOffsetMicros();

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
      const JudgeResult releaseJudge =
          releaseNote(note, releasedTime, nullptr, inputSongTime);
      if (releaseJudge.judgement == None) {
        appendReplayEvent(ReplayEventAction::Release, lane, nullptr,
                          inputSongTime, releasedTime, releaseJudge);
      }
      return note;
    }
  }
  appendReplayEvent(ReplayEventAction::Release, lane, nullptr, inputSongTime,
                    releasedTime, JudgeResult(None, 0));
  return nullptr;
}
void GamePlayScene::checkPassedTimeline(long long time) {
  const auto &measures = chart->Measures;
  if (state == nullptr) {
    return;
  }
  const long long visualNow = nowMicros();
  const long long judgedTime = getJudgementTimeMicros(time);
  const long long poorCutoff = judgedTime - latePoorTiming;
  const bool replayPlayback = isReplayPlayback();
  for (size_t i = state->passedMeasureCount; i < measures.size(); i++) {
    const bool isFirstMeasure = i == state->passedMeasureCount;
    const auto &measure = measures[i];
    for (size_t j = isFirstMeasure ? state->passedTimelineCount : 0;
         j < measure->TimeLines.size(); j++) {
      const auto &timeline = measure->TimeLines[j];
      if (timeline->Timing < poorCutoff) {
        if (isFirstMeasure) {
          state->passedTimelineCount++;
        }
        if (replayPlayback) {
          continue;
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
              longNote->MissPress(judgedTime);
            }
          }
          const auto poorResult = JudgeResult(Poor, judgedTime - timeline->Timing);
          onJudge(poorResult);
          appendReplayEvent(ReplayEventAction::Miss, note->Lane, note, time,
                            judgedTime, poorResult);
        }
      } else if (timeline->Timing <= judgedTime) {
        if (replayPlayback) {
          continue;
        }
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
              longNote->Release(judgedTime);
              const auto judgeResult =
                  judge.judgeNow(longNote->Head, longNote->Head->PlayedTime);
              onJudge(judgeResult);
              if (options.autoPlay) {
                renderer->onLaneReleased(note->Lane, visualNow);
              }
              continue;
            }
          }
          if (options.autoPlay) // NormalNote or LongNote's head
          {
            const JudgeResult judgeResult =
                pressNote(note, judgedTime, nullptr, time);
            renderer->onLanePressed(note->Lane, judgeResult, visualNow);
            if (!note->IsLongNote()) {
              renderer->onLaneReleased(note->Lane, visualNow);
            }
          }
        }
      } else {
        return;
      }
    }
    if (state->passedTimelineCount == measure->TimeLines.size() &&
        isFirstMeasure) {
      state->passedMeasureCount++;
      state->passedTimelineCount = 0;
    }
  }
}

void GamePlayScene::buildReplayNoteLookup() {
  replayNoteLookup.clear();
  if (!isReplayPlayback()) {
    return;
  }

  for (const auto &measure : chart->Measures) {
    for (const auto &timeline : measure->TimeLines) {
      for (const auto &note : timeline->Notes) {
        if (note == nullptr) {
          continue;
        }
        replayNoteLookup[replayNoteKey(note->Lane, timeline->Timing)] = note;
      }
    }
  }
}

bms_parser::Note *
GamePlayScene::findReplayNote(const ReplayEvent &event) const {
  if (event.noteTimeMicros < 0) {
    return nullptr;
  }
  const auto it =
      replayNoteLookup.find(replayNoteKey(event.lane, event.noteTimeMicros));
  return it == replayNoteLookup.end() ? nullptr : it->second;
}

void GamePlayScene::processReplayEvents(long long songTimeMicros) {
  if (!isReplayPlayback() || options.replayData == nullptr) {
    return;
  }

  const auto &events = options.replayData->events;
  const long long visualNow = nowMicros();
  while (replayEventCursor < events.size() &&
         events[replayEventCursor].songTimeMicros <= songTimeMicros) {
    applyReplayEvent(events[replayEventCursor], visualNow);
    replayEventCursor++;
  }
}

void GamePlayScene::applyReplayEvent(const ReplayEvent &event,
                                     long long visualTimeMicros) {
  if (state == nullptr || !state->isPlaying || state->isEnding) {
    return;
  }

  const JudgeResult recordedJudge(event.judgement, event.diffMicros);
  switch (event.action) {
  case ReplayEventAction::Press: {
    if (auto pressedIt = lanePressed.find(event.lane);
        pressedIt != lanePressed.end()) {
      pressedIt->second = true;
    }
    updateLaneStateText();

    if (auto *note = findReplayNote(event);
        note != nullptr && event.judgement != None) {
      pressNote(note, event.judgeTimeMicros, &recordedJudge,
                event.songTimeMicros, false);
      applyReplayGauge(event);
    }
    renderer->onLanePressed(event.lane, recordedJudge, visualTimeMicros);
    break;
  }
  case ReplayEventAction::Release: {
    if (auto pressedIt = lanePressed.find(event.lane);
        pressedIt != lanePressed.end()) {
      pressedIt->second = false;
    }
    updateLaneStateText();
    renderer->onLaneReleased(event.lane, visualTimeMicros);

    if (auto *note = findReplayNote(event);
        note != nullptr && event.judgement != None) {
      releaseNote(note, event.judgeTimeMicros, &recordedJudge,
                  event.songTimeMicros, false);
      applyReplayGauge(event);
    }
    break;
  }
  case ReplayEventAction::Miss:
    if (event.judgement != None) {
      onJudge(recordedJudge);
      applyReplayGauge(event);
    }
    break;
  }
}

void GamePlayScene::applyReplayGauge(const ReplayEvent &event) {
  if (!isReplayPlayback() || state == nullptr || event.judgement == None) {
    return;
  }

  state->gaugeType = event.gaugeType;
  state->currentGauge = event.gauge;
  const int gaugeIndex = gaugeTypeIndex(event.gaugeType);
  if (gaugeIndex >= 0 && gaugeIndex < static_cast<int>(state->gaugeValues.size())) {
    state->gaugeValues[gaugeIndex] = event.gauge;
  }
  if (!state->gaugeHistory.empty()) {
    state->gaugeHistory.back() = event.gauge;
  }
  updateGaugeStatusText();
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

  state->applyGaugeJudgement(judgeResult.judgement);
  updateGaugeStatusText();
}

void GamePlayScene::appendReplayEvent(ReplayEventAction action, int lane,
                                      const bms_parser::Note *note,
                                      long long songTimeMicros,
                                      long long judgeTimeMicros,
                                      const JudgeResult &judgeResult) {
  if (!shouldRecordReplay() || state == nullptr) {
    return;
  }

  ReplayEvent event;
  event.action = action;
  event.lane = lane;
  event.noteTimeMicros = note != nullptr && note->Timeline != nullptr
                             ? note->Timeline->Timing
                             : -1;
  event.songTimeMicros = songTimeMicros;
  event.judgeTimeMicros = judgeTimeMicros;
  event.judgement = judgeResult.judgement;
  event.diffMicros = judgeResult.Diff;
  event.gauge = state->currentGauge;
  event.gaugeType = state->gaugeType;
  event.combo = state->combo;
  event.score = state->getScore();
  recordedReplay.events.push_back(event);
}

JudgeResult GamePlayScene::pressNote(bms_parser::Note *note,
                                     long long pressedTime,
                                     const JudgeResult *precomputedJudge,
                                     long long songTimeMicros,
                                     bool recordEvent) {
  if (note->Wav != bms_parser::Parser::NoWav && !options.autoKeySound) {
    context.jukebox.playKeySound(note->Wav);
  }
  const JudgeResult judgeResult =
      precomputedJudge != nullptr ? *precomputedJudge
                                  : judge.judgeNow(note, pressedTime);
  if (judgeResult.judgement != None) {
    if (judgeResult.isNotePlayed()) {
      // TODO: play keybomb
      if (note->IsLongNote()) {
        if (const auto &longNote = static_cast<bms_parser::LongNote *>(note);
            !longNote->IsTail()) {
          longNote->Press(pressedTime);
          if (recordEvent) {
            appendReplayEvent(ReplayEventAction::Press, note->Lane, note,
                              songTimeMicros >= 0 ? songTimeMicros
                                                  : pressedTime,
                              pressedTime, judgeResult);
          }
        }
        return judgeResult;
      }
      note->Press(pressedTime);
    }
    onJudge(judgeResult);
    if (recordEvent) {
      appendReplayEvent(ReplayEventAction::Press, note->Lane, note,
                        songTimeMicros >= 0 ? songTimeMicros : pressedTime,
                        pressedTime, judgeResult);
    }
  }
  return judgeResult;
}

JudgeResult GamePlayScene::releaseNote(bms_parser::Note *Note,
                                       long long ReleasedTime,
                                       const JudgeResult *precomputedJudge,
                                       long long songTimeMicros,
                                       bool recordEvent) {
  if (!Note->IsLongNote()) {
    return JudgeResult(None, 0);
  }
  const auto &LongNote = static_cast<bms_parser::LongNote *>(Note);
  if (!LongNote->IsTail()) {
    return JudgeResult(None, 0);
  }
  if (!LongNote->IsHolding) {
    return JudgeResult(None, 0);
  }
  LongNote->Release(ReleasedTime);
  const auto judgeResult =
      precomputedJudge != nullptr ? *precomputedJudge
                                  : judge.judgeNow(LongNote, ReleasedTime);
  // if tail judgement is not good/great/pgreat, make it bad
  JudgeResult appliedJudge(None, 0);
  if (judgeResult.judgement == None || judgeResult.judgement == Kpoor ||
      judgeResult.judgement == Poor) {
    appliedJudge = JudgeResult(Bad, judgeResult.Diff);
  } else if (precomputedJudge != nullptr) {
    appliedJudge = *precomputedJudge;
  } else {
    // otherwise, follow the head's judgement
    appliedJudge = judge.judgeNow(LongNote->Head, LongNote->Head->PlayedTime);
  }
  onJudge(appliedJudge);
  if (recordEvent) {
    appendReplayEvent(ReplayEventAction::Release, Note->Lane, Note,
                      songTimeMicros >= 0 ? songTimeMicros : ReleasedTime,
                      ReleasedTime, appliedJudge);
  }
  return appliedJudge;
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
void GamePlayScene::updateLaneStateText() {
  if (laneStateText == nullptr) {
    return;
  }
  std::string str;
  for (auto &[lane, pressed] : lanePressed) {
    str += std::to_string(pressed) + "\n";
  }
  laneStateText->setText(str);
}

void GamePlayScene::updateGaugeStatusText() {
  if (gaugeStatusText == nullptr || state == nullptr) {
    return;
  }

  char text[96];
  std::snprintf(text, sizeof(text), "%s: %s %.1f%%",
                state->gaugeAutoShift ? "GAS" : "Gauge",
                gaugeTypeToShortLabel(state->gaugeType), state->currentGauge);
  gaugeStatusText->setText(text);

  const Color color =
      clearLampColorForRank(gaugeTypeToClearRank(state->gaugeType));
  gaugeStatusText->setColor({color.r, color.g, color.b, 255});
}
