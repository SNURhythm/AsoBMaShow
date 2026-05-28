//
// Created by XF on 8/25/2024.
//

#pragma once
#include "../../ReplayData.h"
#include "RhythmState.h"
#include "../Scene.h"
#include "../../bms_parser.hpp"
#include "../../input/IRhythmControl.h"
#include "../../view/TextView.h"
#include <memory>
#include <unordered_map>
class Button;
struct StartOptions {
  unsigned long long startPosition = 0;
  bool autoKeySound = false;
  bool autoPlay = false;
  GaugeType gaugeType = GaugeType::Normal;
  bool gaugeAutoShift = false;
  std::shared_ptr<ReplayData> replayData = nullptr;
};
class RhythmInputHandler;
class BMSRenderer;
class GamePlayScene : public Scene, public IRhythmControl {
private:
  bms_parser::Chart *chart;
  bool isGamePaused = false;
  std::atomic_bool isCancelled = false;
  long long latePoorTiming;

public:
  GamePlayScene() = delete;

  explicit GamePlayScene(ApplicationContext &context, bms_parser::Chart *chart,
                         StartOptions options)
      : Scene(context), judge(chart->Meta.Rank), options(options) {
    this->chart = chart;
    latePoorTiming = judge.timingWindows[Bad].second;
  };
  void init() override;
  void update(float dt) override;
  void renderScene() override;
  void cleanupScene() override;
  bms_parser::Note *pressLane(int lane, double inputDelay) override;
  bms_parser::Note *pressLane(int mainLane, int compensateLane,
                              double inputDelay) override;
  bms_parser::Note *releaseLane(int lane, double inputDelay) override;
  EventHandleResult handleEvents(SDL_Event &event) override;

private:
  void reset();
  [[nodiscard]] bool isReplayPlayback() const;
  [[nodiscard]] bool shouldRecordReplay() const;
  void beginReplayRecording();
  void finishReplayRecording();
  [[nodiscard]] long long getJudgementOffsetMicros() const;
  [[nodiscard]] long long getInputSongTimeMicros(long long songTimeMicros,
                                                 double inputDelay = 0.0) const;
  [[nodiscard]] long long getJudgementTimeMicros(long long songTimeMicros,
                                                 double inputDelay = 0.0) const;
  [[nodiscard]] long long getVisualOffsetMicros() const;
  [[nodiscard]] long long getVisualTimeMicros(long long songTimeMicros) const;
  View *pauseLayout = nullptr;
  Button *pauseButton = nullptr;
  Judge judge;
  StartOptions options;
  void checkPassedTimeline(long long time);
  void onJudge(const JudgeResult &judgeResult);
  void appendReplayEvent(ReplayEventAction action, int lane,
                         const bms_parser::Note *note,
                         long long songTimeMicros, long long judgeTimeMicros,
                         const JudgeResult &judgeResult);
  JudgeResult pressNote(bms_parser::Note *note, long long pressedTime,
                        const JudgeResult *precomputedJudge = nullptr,
                        long long songTimeMicros = -1,
                        bool recordEvent = true);
  JudgeResult releaseNote(bms_parser::Note *Note, long long ReleasedTime,
                          const JudgeResult *precomputedJudge = nullptr,
                          long long songTimeMicros = -1,
                          bool recordEvent = true);
  RhythmState *state = nullptr;
  BMSRenderer *renderer = nullptr;
  RhythmInputHandler *inputHandler = nullptr;
  std::unordered_map<int, bool> lanePressed;
  ReplayData recordedReplay;
  TextView *gaugeStatusText = nullptr;
  TextView *laneStateText = nullptr;
  void updateGaugeStatusText();
  void updateLaneStateText();
  std::mutex judgeMutex;
};
