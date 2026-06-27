//
// Created by XF on 8/25/2024.
//

#pragma once
#include "../../CoursePlaySession.h"
#include "../../ReplayData.h"
#include "../../math/Vector3.h"
#include "RhythmState.h"
#include "../Scene.h"
#include "../../bms_parser.hpp"
#include "../../input/IRhythmControl.h"
#include "../../view/TextView.h"
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
class Button;
class RhythmLaneInputController;
struct StartOptions {
  unsigned long long startPosition = 0;
  bool autoKeySound = false;
  bool autoPlay = false;
  GaugeType gaugeType = GaugeType::Normal;
  GaugeProfile gaugeProfile = GaugeProfile::Standard;
  bool gaugeAutoShift = false;
  std::shared_ptr<ReplayData> replayData = nullptr;
  std::optional<std::string> playOption;
  std::optional<long long> playOptionSeed;
  std::optional<std::string> playOption2;
  std::optional<long long> playOption2Seed;
  int longNoteMode = 0;
  std::string assistOption = assist_options::kOff;
  std::shared_ptr<CoursePlaySession> courseSession = nullptr;
  CourseConstraintRules courseConstraints;
  bool ownsChart = false;
  bool practiceMode = false;
  unsigned long long practiceLeadInMicros = 0;
  Scene *returnScene = nullptr;
  std::optional<bool> touchVisualizationEnabled;
  std::optional<bool> replayGhostRenderingEnabled;
  std::function<void(const ReplayData &)> practiceGhostCallback;
};
class RhythmInputHandler;
class BMSRenderer;
class GamePlayScene : public Scene, public IRhythmControl {
private:
  std::unique_ptr<bms_parser::Chart> ownedChart;
  bms_parser::Chart *chart = nullptr;
  bool isGamePaused = false;
  std::atomic_bool isCancelled = false;
  long long latePoorTiming;

public:
  GamePlayScene() = delete;

  explicit GamePlayScene(ApplicationContext &context, bms_parser::Chart *chart,
                         StartOptions options);
  explicit GamePlayScene(ApplicationContext &context,
                         std::unique_ptr<bms_parser::Chart> chart,
                         StartOptions options);
  ~GamePlayScene() override;
  void init() override;
  void update(float dt) override;
  bool renderViewBeforeScene(const View *view) const override;
  void renderScene() override;
  void cleanupScene() override;
  bms_parser::Note *pressLane(int lane, double inputDelay) override;
  bms_parser::Note *pressLane(int mainLane, int compensateLane,
                              double inputDelay) override;
  bms_parser::Note *releaseLane(int lane, double inputDelay) override;
  EventHandleResult handleEvents(SDL_Event &event) override;

private:
  void reset();
  void initializeStartPositionState();
  void applyTimelineBpm(const bms_parser::TimeLine *timeline);
  void restartCurrentPattern();
  void retryWithNewPattern();
  [[nodiscard]] bool isReplayPlayback() const;
  [[nodiscard]] bool isCoursePlayback() const;
  [[nodiscard]] bool courseNoSpeed() const;
  [[nodiscard]] int effectiveVisibleTimeGreenNumber() const;
  [[nodiscard]] int effectiveNoteStartPositionPercent() const;
  [[nodiscard]] bool shouldRecordReplay() const;
  [[nodiscard]] bool shouldPersistRecordedReplay() const;
  bool startNextCourseChart();
  void beginReplayRecording();
  void finishReplayRecording();
  void publishPracticeGhost();
  void buildReplayNoteLookup();
  void processReplayKeySounds(long long rawSongTimeMicros);
  void processReplayEvents(long long gameplayTimeMicros);
  void processReplayLaneCoverEvents(long long gameplayTimeMicros);
  void applyReplayEvent(const ReplayEvent &event, long long visualTimeMicros);
  void applyReplayLaneCoverEvent(const ReplayLaneCoverEvent &event);
  void applyReplayGauge(const ReplayEvent &event);
  bms_parser::Note *findReplayNote(const ReplayEvent &event) const;
  void resetHellChargeGaugeTracking(long long gameplayTimeMicros);
  void updateHellChargeGauge(long long gameplayTimeMicros);
  [[nodiscard]] long long getAudioOffsetMicros() const;
  [[nodiscard]] long long getStartPositionMicros() const;
  [[nodiscard]] long long getAudioSeekPositionMicros() const;
  [[nodiscard]] long long
  getGameplayTimeMicros(long long rawSongTimeMicros) const;
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
  void detonateLandmine(bms_parser::LandmineNote *note, long long songTimeMicros,
                        long long judgeTimeMicros);
  void expireGimmickNote(bms_parser::Note *note, long long judgeTimeMicros);
  void onJudge(const JudgeResult &judgeResult,
               bool recordTimingSample = true);
  void appendReplayEvent(ReplayEventAction action, int lane,
                         const bms_parser::Note *note, long long songTimeMicros,
                         long long judgeTimeMicros,
                         const JudgeResult &judgeResult);
  void appendReplayLaneCoverEvent(int noteStartPositionPercent,
                                  long long songTimeMicros,
                                  bool resetVisibleTimeReference);
  bool handleTouchInput(SDL_FingerID fingerIndex, ReplayTouchAction action,
                        Vector3 normalizedLocation);
  bool handleFloatingLaneCoverInput(SDL_FingerID fingerIndex,
                                    ReplayTouchAction action,
                                    Vector3 normalizedLocation,
                                    long long songTimeMicros);
  void persistFloatingLaneCoverSettings();
  void appendReplayTouchSample(SDL_FingerID fingerIndex,
                               ReplayTouchAction action,
                               Vector3 normalizedLocation,
                               long long songTimeMicros);
  JudgeResult pressNote(bms_parser::Note *note, long long pressedTime,
                        const JudgeResult *precomputedJudge = nullptr,
                        long long songTimeMicros = -1, bool recordEvent = true);
  JudgeResult releaseNote(bms_parser::Note *Note, long long ReleasedTime,
                          const JudgeResult *precomputedJudge = nullptr,
                          long long songTimeMicros = -1,
                          bool recordEvent = true);
  std::unique_ptr<RhythmState> ownedState;
  RhythmState *state = nullptr;
  std::unique_ptr<BMSRenderer> ownedRenderer;
  BMSRenderer *renderer = nullptr;
  std::unique_ptr<RhythmLaneInputController> ownedLaneInputController;
  RhythmLaneInputController *laneInputController = nullptr;
  std::unique_ptr<RhythmInputHandler> ownedInputHandler;
  RhythmInputHandler *inputHandler = nullptr;
  std::unordered_map<int, bool> lanePressed;
  ReplayData recordedReplay;
  std::unordered_map<long long, ReplayTouchSample> lastRecordedTouchSamples;
  std::unordered_map<std::string, bms_parser::Note *> replayNoteLookup;
  std::unordered_map<bms_parser::LongNote *, long long>
      hellChargeGaugeBalanceMicros;
  long long lastHellChargeGaugeUpdateMicros = 0;
  size_t replayKeySoundCursor = 0;
  size_t replayEventCursor = 0;
  size_t replayLaneCoverCursor = 0;
  bool practiceGhostPublished = false;
  bool floatingLaneCoverDragActive = false;
  bool floatingLaneCoverDragChanged = false;
  bool floatingLaneCoverSettingsDirty = false;
  SDL_FingerID floatingLaneCoverFinger = -1;
  float floatingLaneCoverDragOffsetY = 0.0f;
  double currentGameplayBpm = 0.0;
  std::unique_ptr<TextView> ownedLaneStateText;
  TextView *laneStateText = nullptr;
  void updateGaugeStatusText();
  void updateLaneStateText();
};
