//
// Created by XF on 8/25/2024.
//

#pragma once
#include "../../CoursePlaySession.h"
#include "../../PreparationPlan.h"
#include "../../ReplayData.h"
#include "../../ThreadCompat.h"
#include "../../audio/PlaybackRate.h"
#include "../../math/Vector3.h"
#include "GamePlayStartOptions.h"
#include "BeatorajaHiSpeed.h"
#include "NoteTimeRange.h"
#include "Pacemaker.h"
#include "PlayfieldChartVisualModel.h"
#include "PlayfieldProjection.h"
#include "PlayfieldVisualState.h"
#include "StartSelectControl.h"
#include "RhythmState.h"
#include "../Scene.h"
#include "../../bms_parser.hpp"
#include "../../input/IRhythmControl.h"
#include "../../input/InputTypes.h"
#include "../../practice/PracticeResultFlow.h"
#include "../../replay/ChartReplayCapture.h"
#include "../../replay/ReplayInputRecorder.h"
#include "../../skin/SkinPresentationTypes.h"
#include "../../view/TextView.h"
#include "../ResultScene.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
class Button;
class RhythmLaneInputController;
class RhythmInputHandler;
class BuiltInPlayfieldPresentation;
class PlayfieldPresentation;
class GamePlayScene : public Scene, public IRhythmControl {
private:
  std::unique_ptr<bms_parser::Chart> ownedChart;
  bms_parser::Chart *chart = nullptr;
  bool isGamePaused = false;
  bool escapeHandledByInputPipeline = false;
  bool profileGameplayBlockerActive = false;
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
  bool pausesBackgroundTasksForPerformance() const override { return true; }
  void init() override;
  void update(float dt) override;
  bool renderViewBeforeScene(const View *view) const override;
  void renderScene() override;
  void cleanupScene() override;
  bms_parser::Note *pressLane(int lane, double inputDelay) override;
  bms_parser::Note *pressLane(int mainLane, int compensateLane,
                              double inputDelay) override;
  bms_parser::Note *releaseLane(int lane, double inputDelay,
                                bool isBackSpin = false) override;
  EventHandleResult handleEvents(SDL_Event &event) override;

private:
  struct RealtimeGameplaySession;
  struct CompletedModernReplayCapture {
    std::optional<std::vector<replay::InputTransition>> acceptedInput;
    std::vector<replay::ReplayTouchSample> touchSamples;
    std::vector<replay::ReplayLaneCoverEvent> laneCoverEvents;
    replay::ReplayTimeBounds timeBounds;
  };
  bool reset();
  bool startRealtimeGameplayAuthority();
  void stopRealtimeGameplayAuthority(bool transferReplay);
  void setRealtimeGameplayIngressEnabled(bool enabled);
  void drainRealtimeTouchSamples(
      std::optional<long long> cancelPresentationAtSteadyMicros =
          std::nullopt);
  void drainRealtimeInputCommands();
  void drainRealtimeStartSelectInputs();
  void refreshRealtimeTouchLayout();
  void refreshGameplayPresentationGeometry();
  void updateSkinResetLayoutVisibility();
  void acquireGameplaySkinForAttempt();
  [[nodiscard]] bool publishRealtimeTouchHitSnapshot();
  void updateRealtimeVisualTimeline(long long gameplayTimeMicros);
  void syncRealtimeGameplaySnapshot();
  [[nodiscard]] bool realtimeGameplayAuthorityActive() const noexcept;
  void showPlaybackInitializationFailure(const std::string &message);
  void initializeStartPositionState();
  void applyTimelineBpm(const bms_parser::TimeLine *timeline);
  void initializePlayfieldVisualNoteSources();
  void capturePlayfieldVisualState(long long gameplayTimeMicros,
                                   long long visualTimeMicros,
                                   bool startLaneIndicatorsVisible,
                                   bool practiceCountInActive,
                                   bool selectedSkinActive);
  void showPauseMenu(bool pausePlayback);
  void closePauseMenu();
  void togglePauseMenuFromInput();
  void
  handleLogicalInputCommand(const input::LogicalInputTransition &transition);
  void consumeStartSelectInput(const gameplay::StartSelectControlInput &input);
  void applyStartSelectControlActions(
      const std::vector<gameplay::StartSelectControlAction> &actions);
  void refreshRuntimePresentationConfiguration();
  void abortPlayFromStartSelectControl();
  void adjustLaneCoverFromInput(int deltaPercent);
  void restartCurrentPattern();
  bool restartCourseFromBeginning();
  void retryWithNewPattern();
  void finishPractice();
  void exitPracticeWithoutSummary();
  void completePracticeAttempt();
  void completePracticeSection(bool realtimeRangeFinalized);
  void finalizePracticeRangeMisses();
  void scheduleResultTransition(std::uint64_t delayMillis);
  [[nodiscard]] std::uint64_t
  selectedSkinResultTransitionDelayMillis(long long gameplayTimeMicros) const;
  void updatePracticeHud(long long chartTimeMicros);
  [[nodiscard]] bool isReplayPlayback() const;
  [[nodiscard]] bool isCoursePlayback() const;
  [[nodiscard]] bool courseNoSpeed() const;
  [[nodiscard]] int effectiveVisibleTimeDurationMilliseconds() const;
  [[nodiscard]] int effectiveNoteStartPositionPercent() const;
  [[nodiscard]] bool shouldRecordReplay() const;
  [[nodiscard]] bool shouldPersistRecordedReplay() const;
  [[nodiscard]] bool usesModernCourseContinuation() const;
  [[nodiscard]] practice::ResultCapturePolicy resultCapturePolicy() const;
  [[nodiscard]] std::optional<NoteTimeRange> practiceNoteRange() const;
  [[nodiscard]] bool practiceInputAllowed(long long chartTimeMicros) const;
  [[nodiscard]] bool practiceReplayEventAllowed(const ReplayEvent &event) const;
  [[nodiscard]] bool
  preparationIndicatorActive(long long rawSongTimeMicros) const;
  [[nodiscard]] bool practiceCountInActive(long long rawSongTimeMicros) const;
  bool startCourseReplayChartAtCurrentIndex();
  bool startCourseChartAtCurrentIndex();
  bool startNextCourseChart();
  bool handleCoursePauseButtonEvent(SDL_Event &event);
  void beginCoursePauseHold(bool touch, SDL_FingerID fingerId);
  void cancelCoursePauseHold();
  void resetCoursePauseHold();
  void updateCoursePauseHoldProgress(long long currentMicros);
  void renderCoursePauseHoldRing();
  void beginReplayRecording();
  void finishReplayRecording();
  [[nodiscard]] CompletedModernReplayCapture completeModernReplayCapture();
  void recordModernCourseStage(const CompletedModernReplayCapture &capture);
  void captureModernReplayInput(int physicalLane,
                                replay::LogicalControl control,
                                bool hasReplayControl, bool pressed,
                                bool replayOnly);
  void publishPracticeGhost();
  void buildReplayNoteLookup();
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
  View *playbackFailureLayout = nullptr;
  Button *pauseButton = nullptr;
  Button *practiceRestartButton = nullptr;
  Button *skinResetLayoutButton = nullptr;
  TextView *practiceHudText = nullptr;
  bool coursePauseHoldActive = false;
  bool coursePauseHoldRewinding = false;
  bool coursePauseHoldTouch = false;
  SDL_FingerID coursePauseHoldFinger = -1;
  long long coursePauseHoldStartMicros = 0;
  long long coursePauseHoldRewindStartMicros = 0;
  float coursePauseHoldProgress = 0.0f;
  float coursePauseHoldRewindStartProgress = 0.0f;
  StartOptions options;
  gameplay::GameplayPolicyBuildOutcome rulesetPolicyBuild;
  Judge judge;
  ScoreProvenance attemptProvenance;
  void checkPassedTimeline(long long time);
  void detonateLandmine(bms_parser::LandmineNote *note,
                        long long songTimeMicros, long long judgeTimeMicros);
  void expireGimmickNote(bms_parser::Note *note, long long judgeTimeMicros);
  void onJudge(const JudgeResult &judgeResult, PlayfieldJudgeEventClock clock,
               bool recordTimingSample = true);
  [[nodiscard]] PlayfieldJudgeEventClock
  judgeEventClock(long long songTimeMicros) const;
  void appendReplayEvent(ReplayEventAction action, int lane,
                         const bms_parser::Note *note, long long songTimeMicros,
                         long long judgeTimeMicros,
                         const JudgeResult &judgeResult);
  void recordPreparationLaneEvent(ReplayEventAction action, int lane,
                                  long long songTimeMicros);
  void appendReplayLaneCoverEvent(int noteStartPositionPercent,
                                  long long songTimeMicros,
                                  bool resetVisibleTimeReference,
                                  ReplayLaneCoverChangeKind changeKind);
  bool handleTouchInput(SDL_FingerID fingerIndex, ReplayTouchAction action,
                        Vector3 normalizedLocation);
  bool handleTouchInputAtGameplayTime(
      SDL_FingerID fingerIndex, ReplayTouchAction action,
      Vector3 normalizedLocation, long long gameplayTimeMicros,
      std::optional<long long> visualGameplayTimeMicros = std::nullopt,
      bool allowBuiltInControl = true);
  bool handleFloatingLaneCoverInput(SDL_FingerID fingerIndex,
                                    ReplayTouchAction action,
                                    Vector3 normalizedLocation,
                                    long long songTimeMicros);
  void cancelLegacyFloatingLaneCoverTouch();
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
  pacemaker::Target activePacemakerTarget;
  pacemaker::Target activeBestScoreTarget;
  std::optional<ScoreBestSnapshot> activePacemakerBest;
  std::optional<ChartScoreHistorySnapshot> activePersistedScore;
  std::optional<PlayfieldRivalScoreState> activeRivalScore;
  std::optional<int> activeTargetPlayOption;
  PlayerScoreHistorySnapshot activePlayerScoreHistory;
  int playfieldFavoriteChartState = 0;
  bool playfieldChartHasDocument = false;
  bool playfieldStageFileAvailable = false;
  bool playfieldBackBmpAvailable = false;
  std::optional<ResultPreviousBestData> activeReplayPacemakerPreviousBest;
  std::jthread bestReplayLoadThread;
  std::shared_ptr<std::atomic_bool> bestReplayLoadCancelled =
      std::make_shared<std::atomic_bool>(false);
  std::mutex bestReplayLoadMutex;
  std::shared_ptr<ReplayData> pendingBestReplay;
  PlayfieldChartVisualModel playfieldChartVisualModel;
  PlayfieldProjection playfieldProjection;
  std::unique_ptr<PlayfieldVisualStateStore> ownedPlayfieldVisualStateStore;
  PlayfieldVisualStateStore *playfieldVisualStateStore = nullptr;
  PlayfieldPresentationConfig playfieldPresentationConfiguration;
  std::unique_ptr<PlayfieldPresentation> ownedPresentation;
  BuiltInPlayfieldPresentation *builtInPresentation = nullptr;
  PlayfieldPresentation *presentation = nullptr;
  std::unique_ptr<PlayfieldPresentationEventFanout>
      ownedPresentationEventFanout;
  PlayfieldPresentationEventFanout *presentationEventFanout = nullptr;
  std::unique_ptr<RhythmLaneInputController> ownedLaneInputController;
  RhythmLaneInputController *laneInputController = nullptr;
  std::unique_ptr<RhythmInputHandler> ownedInputHandler;
  RhythmInputHandler *inputHandler = nullptr;
  std::unique_ptr<RealtimeGameplaySession> realtimeGameplaySession;
  std::uint64_t realtimeGameplayEpoch = 0;
  bool realtimeGameplayAuthorityWaitingForSkinGeometry = false;
  std::unordered_map<int, bool> lanePressed;
  bool startButtonPressed = false;
  bool selectButtonPressed = false;
  ReplayData recordedReplay;
  ReplayData analyticsReplay;
  std::unique_ptr<replay::ReplayInputRecorder> modernReplayInputRecorder;
  std::optional<std::vector<replay::InputTransition>>
      completedModernReplayInput;
  std::optional<GaugeStateSnapshot> courseStageInitialGauge;
  std::string modernReplayCaptureDiagnostic;
  ResultPersistenceOptions resultPersistenceOptions;
  std::string resultPersistenceAttemptId;
  std::unordered_map<long long, ReplayTouchSample> lastRecordedTouchSamples;
  std::unordered_map<std::string, bms_parser::Note *> replayNoteLookup;
  std::unordered_map<bms_parser::LongNote *, long long>
      hellChargeGaugeBalanceMicros;
  long long lastHellChargeGaugeUpdateMicros = 0;
  size_t replayEventCursor = 0;
  size_t replayLaneCoverCursor = 0;
  bool touchVisualizerLoaded = false;
  bool playbackInitializationFailed = false;
  bool practiceGhostPublished = false;
  bool recordedAttemptCompleted = false;
  bool resultTransitionScheduled = false;
  bool resultPersistenceAttemptCreationTried = false;
  std::optional<gameplay::StartSelectControl> startSelectControl;
  std::optional<gameplay_hispeed::State> playfieldHispeedState;
  bool playfieldLaneCoverEnabled = true;
  bool floatingLaneCoverDragActive = false;
  bool floatingLaneCoverDragChanged = false;
  bool floatingLaneCoverSettingsDirty = false;
  SDL_FingerID floatingLaneCoverFinger = -1;
  float floatingLaneCoverDragOffsetY = 0.0f;
  std::uint64_t playfieldFrameSerial = 0;
  PlayfieldVisualState capturedPlayfieldVisualState;
  PlayfieldProjectionResult capturedPlayfieldProjection;
  std::vector<const bms_parser::Note *> playfieldVisualNoteSources;
  int playfieldLaneCoverPercent = 0;
  float playfieldLaneCoverPercentExact = 0.0F;
  bool playfieldLaneCoverResetPending = false;
  bool gameplaySkinSafeBoundsInitialized = false;
  double gameplaySkinSafeBoundsX = 0.0;
  double gameplaySkinSafeBoundsY = 0.0;
  double gameplaySkinSafeBoundsWidth = 0.0;
  double gameplaySkinSafeBoundsHeight = 0.0;
  double currentGameplayBpm = 0.0;
  double currentGameplayScrollRate = 1.0;
  preparation::Plan preparationPlan;
  std::unique_ptr<TextView> ownedLaneStateText;
  TextView *laneStateText = nullptr;
  void configurePacemakerTarget();
  void startBestReplayLoad(std::string attemptId,
                           std::filesystem::path chartPath);
  void applyPendingBestReplay();
  void stopBestReplayLoad();
  void updatePacemakerStatus();
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  void applySkinAudioVolume(skin::SkinAudioVolumeWriterTarget target,
                            float value);
#endif
  void updateGaugeStatusText();
  bool finishIfGaugeFailed();
  void updateLaneStateText();
};
