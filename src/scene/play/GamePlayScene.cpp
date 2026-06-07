//
// Created by XF on 8/25/2024.
//

#include "GamePlayScene.h"
#include "../../PlayOptionUtils.h"
#include "../../view/TextView.h"
#include "BMSRenderer.h"
#include "../../input/RhythmInputHandler.h"
#include "../../targets.h"
#include "../../view/Button.h"
#include "../../scene/MainMenuScene.h"
#include "../ResultScene.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <optional>
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

struct PressLaneCandidate {
  int lane = 0;
  bms_parser::Note *note = nullptr;
  JudgeResult judge = JudgeResult(None, 0);
};

long long noteTimingMicros(const bms_parser::Note *note) {
  return note != nullptr && note->Timeline != nullptr ? note->Timeline->Timing
                                                      : 0;
}

long long absoluteTimeDistance(long long a, long long b) {
  return a > b ? a - b : b - a;
}

void markPracticeSkippedNote(bms_parser::Note *note, long long startTime) {
  if (note == nullptr) {
    return;
  }
  note->IsPlayed = true;
  note->IsDead = true;
  note->PlayedTime = startTime;
  if (auto *longNote = dynamic_cast<bms_parser::LongNote *>(note);
      longNote != nullptr) {
    longNote->IsHolding = false;
    if (!longNote->IsTail() && longNote->Tail != nullptr) {
      longNote->Tail->IsPlayed = true;
      longNote->Tail->IsDead = true;
      longNote->Tail->PlayedTime = startTime;
      longNote->Tail->IsHolding = false;
    }
    if (longNote->IsTail() && longNote->Head != nullptr) {
      longNote->Head->IsPlayed = true;
      longNote->Head->IsDead = true;
      longNote->Head->PlayedTime = startTime;
      longNote->Head->IsHolding = false;
    }
  }
}

long long latestHittableNoteTiming(const Judge &judge, long long inputTime) {
  bool hasWindow = false;
  long long earliestWindow = 0;
  for (const auto &entry : judge.timingWindows) {
    if (!hasWindow || entry.second.first < earliestWindow) {
      earliestWindow = entry.second.first;
      hasWindow = true;
    }
  }
  return hasWindow ? inputTime - earliestWindow : inputTime;
}

bool preferByTimingWindow(const PressLaneCandidate &current,
                          const PressLaneCandidate &next,
                          long long inputTime, const Judge &judge,
                          Judgement threshold) {
  if (next.note == nullptr || next.note->IsPlayed) {
    return false;
  }
  const auto windowIt = judge.timingWindows.find(threshold);
  if (windowIt == judge.timingWindows.end()) {
    return false;
  }

  const auto &window = windowIt->second;
  const long long currentTiming = noteTimingMicros(current.note);
  const long long nextTiming = noteTimingMicros(next.note);
  return currentTiming < inputTime - window.second &&
         nextTiming <= inputTime - window.first;
}

bool shouldPreferCandidate(const PressLaneCandidate &current,
                           const PressLaneCandidate &next,
                           long long inputTime, const Judge &judge,
                           AppSettings::NotePriorityMode mode) {
  switch (mode) {
  case AppSettings::NotePriorityMode::Combo:
    return preferByTimingWindow(current, next, inputTime, judge, Good);
  case AppSettings::NotePriorityMode::Duration:
    return next.note != nullptr && !next.note->IsPlayed &&
           absoluteTimeDistance(noteTimingMicros(current.note), inputTime) >
               absoluteTimeDistance(noteTimingMicros(next.note), inputTime);
  case AppSettings::NotePriorityMode::Score:
    return preferByTimingWindow(current, next, inputTime, judge, Great);
  case AppSettings::NotePriorityMode::Lowest:
    return false;
  }
  return false;
}

bool laneIsPressed(const std::unordered_map<int, bool> &lanePressed,
                   int lane) {
  const auto it = lanePressed.find(lane);
  return it != lanePressed.end() && it->second;
}

std::string gameplayPlayOptionLabel(const StartOptions &options) {
  std::optional<std::string> option = options.playOption;
  std::optional<long long> seed = options.playOptionSeed;
  std::optional<std::string> option2 = options.playOption2;
  std::optional<long long> seed2 = options.playOption2Seed;

  if (options.replayData != nullptr) {
    if (!option.has_value()) {
      option = options.replayData->playOption;
    }
    if (!seed.has_value()) {
      seed = options.replayData->playOptionSeed;
    }
    if (!option2.has_value()) {
      option2 = options.replayData->playOption2;
    }
    if (!seed2.has_value()) {
      seed2 = options.replayData->playOption2Seed;
    }
  }

  const std::string label =
      play_options::formatPlayOptionLabel(option, seed, option2, seed2);
  return label.empty() ? "" : "Option: " + label;
}

bool prepareRetryChart(const bms_parser::ChartMeta &meta,
                       const StartOptions &sourceOptions,
                       std::unique_ptr<bms_parser::Chart> &retryChart,
                       StartOptions &retryOptions,
                       std::atomic_bool &cancelled) {
  retryChart = play_options::parseChart(meta.BmsPath, cancelled, "retry");
  if (retryChart == nullptr || cancelled) {
    return false;
  }

  retryOptions = sourceOptions;
  retryOptions.startPosition = 0;
  retryOptions.autoPlay = false;
  retryOptions.replayData = nullptr;
  retryOptions.playOption.reset();
  retryOptions.playOptionSeed.reset();
  retryOptions.playOption2.reset();
  retryOptions.playOption2Seed.reset();
  retryOptions.ownsChart = true;

  std::optional<std::string> playOption = sourceOptions.playOption;
  std::optional<std::string> playOption2 = sourceOptions.playOption2;
  if (sourceOptions.replayData != nullptr) {
    if (!playOption.has_value()) {
      playOption = sourceOptions.replayData->playOption;
    }
    if (!playOption2.has_value()) {
      playOption2 = sourceOptions.replayData->playOption2;
    }
  }

  if (playOption.has_value() &&
      !play_options::applyPlayOptionModifier(
          *retryChart, *playOption, std::nullopt, 0, retryOptions.playOption,
          retryOptions.playOptionSeed, "retry")) {
    return false;
  }

  if (retryChart->Meta.IsDP && playOption2.has_value() &&
      !play_options::applyPlayOptionModifier(
          *retryChart, *playOption2, std::nullopt, 1, retryOptions.playOption2,
          retryOptions.playOption2Seed, "retry")) {
    return false;
  }

  return true;
}

#if defined(DEBUG) || defined(_DEBUG)
constexpr bool kShowLaneStateOverlay = true;
#else
constexpr bool kShowLaneStateOverlay = false;
#endif
} // namespace

void GamePlayScene::init() {
  renderer = new BMSRenderer(chart, judge.timingWindows,
                             context.settings.visibleTimeGreenNumber);
  renderer->setJudgementIndicatorConfig(
      context.settings.judgementIndicatorEnabled,
      context.settings.judgementIndicatorY,
      context.settings.judgementIndicatorWidthScale,
      context.settings.judgementIndicatorRenderMode ==
          AppSettings::JudgementIndicatorRenderMode::Hud2D);
  renderer->setReplayData(options.replayData.get());
  renderer->setShowInvisibleNotes(context.settings.showInvisibleNotes);
  renderer->setPlayOptionStatus(gameplayPlayOptionLabel(options));
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
      auto makePauseButton = [](const std::string &label, int width, int height,
                                auto onClick) {
        auto button = new Button();
        auto text = new TextView("assets/fonts/notosanscjkjp.ttf", 32);
        text->setText(label);
        text->setAlign(TextView::CENTER);
        button->setContentView(text);
        button->setOnClickListener(onClick);
        button->setSize(width, height);
        return button;
      };

      auto pauseText = new TextView("assets/fonts/notosanscjkjp.ttf", 32);
      pauseText->setSize(200, 100);
      pauseText->setText("Paused");
      pauseText->setAlign(TextView::CENTER);
      pauseScreen->addView(pauseText);
      pauseScreen->addView(makePauseButton("Resume", 200, 100, [this]() {
        context.jukebox.resume();
        pauseLayout->setVisible(false);
      }));
      pauseScreen->addView(makePauseButton(
          isReplayPlayback() ? "Replay" : "Retry", 260, 90, [this]() {
            if (isReplayPlayback() || options.practiceMode) {
              restartCurrentPattern();
            } else {
              retryWithNewPattern();
            }
          }));
      if (!isReplayPlayback() && !options.practiceMode) {
        pauseScreen->addView(makePauseButton(
            "Retry Same", 260, 90, [this]() { restartCurrentPattern(); }));
      }
      pauseScreen->addView(makePauseButton("Exit", 200, 100, [this]() {
        finishReplayRecording();
        publishPracticeGhost();
        context.jukebox.stop();
        defer(
            [this]() {
              if (options.practiceMode && options.returnScene != nullptr) {
                context.sceneManager->changeScene(options.returnScene, false);
              } else {
                context.sceneManager->changeScene("MainMenu");
              }
              return false;
            },
            0, true);
      }));
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
      for (const auto &note : timeline->InvisibleNotes) {
        if (note == nullptr) {
          continue;
        }
        note->Reset();
      }
      for (const auto &note : timeline->LandmineNotes) {
        if (note == nullptr) {
          continue;
        }
        note->Reset();
      }
    }
  }
  context.jukebox.stop();
  const long long startPositionMicros = getStartPositionMicros();
  const std::optional<long long> practiceKeySoundCutoff =
      options.practiceLeadInMicros > 0 ? std::optional<long long>(
                                             startPositionMicros)
                                       : std::nullopt;
  context.jukebox.schedule(*chart, options.autoKeySound && !isReplayPlayback(),
                           isCancelled, practiceKeySoundCutoff);
  context.jukebox.play();
  const long long audioSeekPosition = getAudioSeekPositionMicros();
  if (audioSeekPosition > 0) {
    context.jukebox.seek(audioSeekPosition);
  }
  state = new RhythmState(chart, false);
  const GaugeType initialGaugeType = isReplayPlayback()
                                         ? options.replayData->initialGaugeType
                                         : options.gaugeType;
  const bool gaugeAutoShift = isReplayPlayback()
                                  ? options.replayData->gaugeAutoShift
                                  : options.gaugeAutoShift;
  state->configureGauge(initialGaugeType, gaugeAutoShift);
  initializeStartPositionState();
  state->isPlaying = true;
  replayKeySoundCursor = 0;
  replayEventCursor = 0;
  buildReplayNoteLookup();
  beginReplayRecording();
  updateGaugeStatusText();
}

void GamePlayScene::restartCurrentPattern() {
  pauseLayout->setVisible(false);
  context.jukebox.stop();
  defer(
      [this]() {
        reset();
        return true;
      },
      0, true);
}

void GamePlayScene::retryWithNewPattern() {
  if (isReplayPlayback()) {
    restartCurrentPattern();
    return;
  }

  pauseLayout->setVisible(false);
  context.jukebox.stop();

  defer(
      [this]() {
        std::atomic_bool parseCancelled = false;
        std::unique_ptr<bms_parser::Chart> retryChart;
        StartOptions retryOptions;
        if (!prepareRetryChart(chart->Meta, options, retryChart, retryOptions,
                               parseCancelled)) {
          SDL_Log("Failed to prepare retry chart for: %s",
                  chart->Meta.Title.c_str());
          reset();
          return true;
        }

        context.jukebox.stop();
        context.jukebox.loadChart(*retryChart, true, parseCancelled);
        if (parseCancelled) {
          reset();
          return true;
        }

        auto *loadedChart = retryChart.release();
        context.sceneManager->changeScene(
            new GamePlayScene(context, loadedChart, retryOptions), false);
        return false;
      },
      0, true);
}

bool GamePlayScene::isReplayPlayback() const {
  return options.replayData != nullptr;
}

bool GamePlayScene::shouldRecordReplay() const {
  return !options.autoPlay && !isReplayPlayback();
}

bool GamePlayScene::shouldPersistRecordedReplay() const {
  return shouldRecordReplay() && !options.practiceMode;
}

void GamePlayScene::beginReplayRecording() {
  practiceGhostPublished = false;
  if (!shouldRecordReplay()) {
    recordedReplay = {};
    return;
  }

  recordedReplay = {};
  recordedReplay.chartMeta = chart->Meta;
  recordedReplay.randomSeed = chart->Meta.RandomSeed;
  recordedReplay.randomPrng = chart->Meta.RandomPrng;
  recordedReplay.randomValues = chart->Meta.RandomValues;
  recordedReplay.playOption = options.playOption;
  recordedReplay.playOptionSeed = options.playOptionSeed;
  recordedReplay.playOption2 = options.playOption2;
  recordedReplay.playOption2Seed = options.playOption2Seed;
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

void GamePlayScene::publishPracticeGhost() {
  if (!options.practiceMode || practiceGhostPublished ||
      !options.practiceGhostCallback || recordedReplay.events.empty()) {
    return;
  }

  finishReplayRecording();
  practiceGhostPublished = true;
  options.practiceGhostCallback(recordedReplay);
}

long long GamePlayScene::getAudioOffsetMicros() const {
  return static_cast<long long>(context.settings.audioOffsetMs) * 1000LL;
}

long long GamePlayScene::getStartPositionMicros() const {
  const long long requested =
      static_cast<long long>(std::min<unsigned long long>(
          options.startPosition,
          static_cast<unsigned long long>(
              std::max(0LL, chart != nullptr ? chart->Meta.TotalLength : 0LL))));
  return std::max(0LL, requested);
}

long long GamePlayScene::getAudioSeekPositionMicros() const {
  const long long startPosition = getStartPositionMicros();
  const long long leadIn =
      static_cast<long long>(std::min<unsigned long long>(
          options.practiceLeadInMicros,
          static_cast<unsigned long long>(std::max(0LL, startPosition))));
  return std::max(0LL, startPosition - leadIn);
}

void GamePlayScene::initializeStartPositionState() {
  if (state == nullptr || chart == nullptr) {
    return;
  }

  const long long startPosition = getStartPositionMicros();
  if (startPosition <= 0) {
    return;
  }

  bool foundStartTimeline = false;
  for (size_t measureIndex = 0; measureIndex < chart->Measures.size();
       ++measureIndex) {
    const auto *measure = chart->Measures[measureIndex];
    if (measure == nullptr) {
      continue;
    }

    for (size_t timelineIndex = 0; timelineIndex < measure->TimeLines.size();
         ++timelineIndex) {
      auto *timeline = measure->TimeLines[timelineIndex];
      if (timeline == nullptr) {
        continue;
      }

      if (timeline->Timing >= startPosition) {
        state->passedMeasureCount = measureIndex;
        state->passedTimelineCount = timelineIndex;
        foundStartTimeline = true;
        break;
      }

      for (auto *note : timeline->Notes) {
        markPracticeSkippedNote(note, startPosition);
      }
      for (auto *note : timeline->LandmineNotes) {
        markPracticeSkippedNote(note, startPosition);
      }
    }

    if (foundStartTimeline) {
      break;
    }
  }

  if (!foundStartTimeline) {
    state->passedMeasureCount = chart->Measures.size();
    state->passedTimelineCount = 0;
  }
}

long long
GamePlayScene::getGameplayTimeMicros(long long rawSongTimeMicros) const {
  return rawSongTimeMicros + getAudioOffsetMicros();
}

long long GamePlayScene::getInputSongTimeMicros(long long songTimeMicros,
                                                double inputDelay) const {
  return songTimeMicros - static_cast<long long>(inputDelay * 1000000);
}

long long GamePlayScene::getJudgementTimeMicros(long long songTimeMicros,
                                                double inputDelay) const {
  return getInputSongTimeMicros(songTimeMicros, inputDelay);
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

  const long long rawSongTimeMicros = context.jukebox.getTimeMicros();
  const long long gameplayTimeMicros = getGameplayTimeMicros(rawSongTimeMicros);
  if (isReplayPlayback()) {
    processReplayKeySounds(rawSongTimeMicros);
    processReplayEvents(gameplayTimeMicros);
  }
  checkPassedTimeline(gameplayTimeMicros);
  if (state->passedMeasureCount != chart->Measures.size()) {
    return;
  }

  SDL_Log("All measures passed");
  state->isEnding = true;
  finishReplayRecording();
  publishPracticeGhost();
  defer(
      [this]() {
        const ReplayData *replayToSave =
            shouldPersistRecordedReplay() ? &recordedReplay : nullptr;
        const ReplayData *retrySource =
            replayToSave != nullptr
                ? replayToSave
                : (options.replayData != nullptr ? options.replayData.get()
                                                 : nullptr);
        ResultPracticeOptions practiceResultOptions;
        if (options.practiceMode) {
          practiceResultOptions.enabled = true;
          practiceResultOptions.startPosition =
              static_cast<unsigned long long>(getStartPositionMicros());
          practiceResultOptions.autoKeySound = options.autoKeySound;
          practiceResultOptions.gaugeType = options.gaugeType;
          practiceResultOptions.gaugeAutoShift = options.gaugeAutoShift;
          practiceResultOptions.playOption = options.playOption;
          practiceResultOptions.playOptionSeed = options.playOptionSeed;
          practiceResultOptions.playOption2 = options.playOption2;
          practiceResultOptions.playOption2Seed = options.playOption2Seed;
          practiceResultOptions.leadInMicros = options.practiceLeadInMicros;
          practiceResultOptions.returnScene = options.returnScene;
          practiceResultOptions.practiceGhostCallback =
              options.practiceGhostCallback;
        }
        context.sceneManager->changeScene(
            new ResultScene(context, chart->Meta, *state, replayToSave,
                            !options.practiceMode && !isReplayPlayback(),
                            retrySource, practiceResultOptions),
            false);
        return false;
      },
      2000, true);
}

void GamePlayScene::renderScene() {
  RenderContext renderContext;
  pauseLayout->setSize(rendering::window_width, rendering::window_height);
  // pauseButton->setPosition(rendering::window_width - 40, 10);
  renderer->render(renderContext, getVisualTimeMicros(getGameplayTimeMicros(
                                      context.jukebox.getTimeMicros())));
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
  delete state;
  state = nullptr;
  delete laneStateText;
  laneStateText = nullptr;
  if (options.ownsChart) {
    delete chart;
    chart = nullptr;
  }
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
  const long long gameplayTime = getGameplayTimeMicros(rawSongTime);
  const long long inputSongTime =
      getInputSongTimeMicros(gameplayTime, inputDelay);
  const long long pressedTime = inputSongTime;
  const long long futureCutoff = latestHittableNoteTiming(judge, pressedTime);
  const AppSettings::NotePriorityMode priorityMode =
      context.settings.notePriorityMode;
  bool hasSelectedCandidate = false;
  bool stopScanning = false;
  PressLaneCandidate selectedCandidate;

  for (size_t i = state->passedMeasureCount;
       i < measures.size() && !stopScanning; i++) {
    const bool isFirstMeasure = i == state->passedMeasureCount;
    const auto &measure = measures[i];

    for (size_t j = isFirstMeasure ? state->passedTimelineCount : 0;
         j < measure->TimeLines.size() && !stopScanning; j++) {
      const auto &timeline = measure->TimeLines[j];
      if (timeline->Timing < pressedTime - latePoorTiming) {
        continue;
      }
      if (timeline->Timing > futureCutoff) {
        stopScanning = true;
        break;
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
        const PressLaneCandidate candidate{lane, note, noteJudge};
        if (!hasSelectedCandidate ||
            shouldPreferCandidate(selectedCandidate, candidate, pressedTime,
                                  judge, priorityMode)) {
          selectedCandidate = candidate;
          hasSelectedCandidate = true;
        }
        if (priorityMode == AppSettings::NotePriorityMode::Lowest) {
          stopScanning = true;
          break;
        }
      }
    }
  }
  if (hasSelectedCandidate) {
    const JudgeResult judgement =
        pressNote(selectedCandidate.note, pressedTime,
                  &selectedCandidate.judge, inputSongTime);
    if (const auto pressedIt = lanePressed.find(selectedCandidate.lane);
        pressedIt != lanePressed.end()) {
      pressedIt->second = true;
    }
    updateLaneStateText();
    renderer->onLanePressed(selectedCandidate.lane, judgement, nowMicros());
    return selectedCandidate.note;
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
  const long long gameplayTime = getGameplayTimeMicros(rawSongTime);
  const long long inputSongTime =
      getInputSongTimeMicros(gameplayTime, inputDelay);
  const long long releasedTime = inputSongTime;

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
          for (const auto &note : timeline->Notes) {
            if (note != nullptr && note->IsLandmineNote()) {
              expireGimmickNote(note, judgedTime);
            }
          }
          for (const auto &note : timeline->LandmineNotes) {
            expireGimmickNote(note, judgedTime);
          }
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
            expireGimmickNote(note, judgedTime);
            continue;
          }
          if (note->IsLongNote()) {
            const auto &longNote = static_cast<bms_parser::LongNote *>(note);
            if (!longNote->IsTail()) {
              longNote->MissPress(judgedTime);
            }
          }
          const auto poorResult =
              JudgeResult(Poor, judgedTime - timeline->Timing);
          onJudge(poorResult, false);
          appendReplayEvent(ReplayEventAction::Miss, note->Lane, note, time,
                            judgedTime, poorResult);
        }
        for (const auto &note : timeline->LandmineNotes) {
          if (note == nullptr || note->IsDead) {
            continue;
          }
          expireGimmickNote(note, judgedTime);
        }
      } else if (timeline->Timing <= judgedTime) {
        // auto-release long notes
        for (const auto &note : timeline->Notes) {
          if (note == nullptr) {
            continue;
          }
          if (note->IsPlayed) {
            continue;
          }
          if (note->IsLandmineNote()) {
            auto *landmine = static_cast<bms_parser::LandmineNote *>(note);
            if (!replayPlayback && laneIsPressed(lanePressed, note->Lane)) {
              detonateLandmine(landmine, time, judgedTime);
            } else {
              expireGimmickNote(landmine, judgedTime);
            }
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
              onJudge(judgeResult, false);
              appendReplayEvent(ReplayEventAction::Release, note->Lane, note,
                                time, judgedTime, judgeResult);
              if (options.autoPlay) {
                renderer->onLaneReleased(note->Lane, visualNow);
              }
              continue;
            }
          }
          if (replayPlayback) {
            continue;
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
        for (const auto &note : timeline->LandmineNotes) {
          if (note == nullptr || note->IsDead) {
            continue;
          }
          if (!replayPlayback && laneIsPressed(lanePressed, note->Lane)) {
            detonateLandmine(note, time, judgedTime);
          } else {
            expireGimmickNote(note, judgedTime);
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
      for (const auto &note : timeline->LandmineNotes) {
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

void GamePlayScene::processReplayKeySounds(long long rawSongTimeMicros) {
  if (!isReplayPlayback() || options.replayData == nullptr ||
      options.autoKeySound) {
    return;
  }

  const auto &events = options.replayData->events;
  while (replayKeySoundCursor < events.size()) {
    const auto &event = events[replayKeySoundCursor];
    if (event.songTimeMicros > rawSongTimeMicros) {
      break;
    }

    if (event.action == ReplayEventAction::Press) {
      if (auto *note = findReplayNote(event);
          note != nullptr && note->Wav != bms_parser::Parser::NoWav) {
        context.jukebox.playKeySound(note->Wav);
      }
    }
    replayKeySoundCursor++;
  }
}

void GamePlayScene::processReplayEvents(long long gameplayTimeMicros) {
  if (!isReplayPlayback() || options.replayData == nullptr) {
    return;
  }

  const auto &events = options.replayData->events;
  const long long visualNow = nowMicros();
  while (replayEventCursor < events.size() &&
         events[replayEventCursor].songTimeMicros <= gameplayTimeMicros) {
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
      onJudge(recordedJudge, false);
      applyReplayGauge(event);
    }
    break;
  case ReplayEventAction::Mine:
    if (auto *note = findReplayNote(event); note != nullptr) {
      note->IsPlayed = true;
      expireGimmickNote(note, event.judgeTimeMicros);
    }
    applyReplayGauge(event);
    break;
  }
}

void GamePlayScene::applyReplayGauge(const ReplayEvent &event) {
  if (!isReplayPlayback() || state == nullptr) {
    return;
  }

  state->gaugeType = event.gaugeType;
  state->currentGauge = event.gauge;
  const int gaugeIndex = gaugeTypeIndex(event.gaugeType);
  if (gaugeIndex >= 0 &&
      gaugeIndex < static_cast<int>(state->gaugeValues.size())) {
    state->gaugeValues[gaugeIndex] = event.gauge;
  }
  if (!state->gaugeHistory.empty()) {
    state->gaugeHistory.back() = event.gauge;
  }
  updateGaugeStatusText();
}

void GamePlayScene::detonateLandmine(bms_parser::LandmineNote *note,
                                     long long songTimeMicros,
                                     long long judgeTimeMicros) {
  if (note == nullptr || note->IsDead) {
    return;
  }

  note->IsPlayed = true;
  note->IsDead = true;
  note->PlayedTime = judgeTimeMicros;

  if (state != nullptr) {
    state->applyGaugeDelta(-note->Damage);
    updateGaugeStatusText();
  }
  appendReplayEvent(ReplayEventAction::Mine, note->Lane, note, songTimeMicros,
                    judgeTimeMicros, JudgeResult(None, 0));
}

void GamePlayScene::expireGimmickNote(bms_parser::Note *note,
                                      long long judgeTimeMicros) {
  if (note == nullptr || note->IsDead) {
    return;
  }

  note->IsDead = true;
  note->PlayedTime = judgeTimeMicros;
}

void GamePlayScene::onJudge(const JudgeResult &judgeResult,
                            bool recordTimingSample) {
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
  renderer->onJudge(judgeResult, state->combo, state->getScore(),
                    getVisualTimeMicros(
                        getGameplayTimeMicros(context.jukebox.getTimeMicros())),
                    recordTimingSample);
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
  if (note->Wav != bms_parser::Parser::NoWav && !options.autoKeySound &&
      !isReplayPlayback()) {
    context.jukebox.playKeySound(note->Wav);
  }
  const JudgeResult judgeResult = precomputedJudge != nullptr
                                      ? *precomputedJudge
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
    onJudge(judgeResult, !options.autoPlay || isReplayPlayback());
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
  const auto judgeResult = precomputedJudge != nullptr
                               ? *precomputedJudge
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
  onJudge(appliedJudge, !options.autoPlay || isReplayPlayback());
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
  if (renderer == nullptr || state == nullptr) {
    return;
  }

  renderer->setGaugeStatus(state->gaugeType, state->gaugeAutoShift,
                           state->currentGauge);
}
