#include "i18n/Localization.h"
#include "ir/tachi/TachiEligibility.h"
#include "scene/play/GamePlayStartOptions.h"
#include "scene/play/GamePlayTiming.h"
#include "scene/play/IpadGestureReminder.h"
#include "scene/play/PracticeNoteFinalizer.h"
#include "scene/play/RealtimeGameplayAuthorityPolicy.h"
#include "scene/play/RealtimeGameplayWorker.h"
#include "scene/play/RealtimeGameplayInputRegistration.h"
#include "input/RealtimePhysicalInputRouter.h"
#include "scene/play/PlayfieldPresentationEvents.h"
#include "scene/play/PlayfieldVisualState.h"
#include "scene/play/GameplayNoteJudgeRole.h"
#include "scene/play/StartSelectControl.h"
#include "replay/ReplayInputRecorder.h"
#include "practice/PracticeResultFlow.h"
#include "Uuid.h"
#include "CourseConstraintUtils.h"
#include "CourseIdentity.h"
#include "ResultPersistenceCoordinator.h"
#include "replay/ReplaySetupProvenance.h"
#include "replay/CourseReplayConsumer.h"
#include "skin/beatoraja/GameplaySkinEndAnimation.h"
#include <SDL2/SDL_log.h>
#include "../SDL/include/SDL_uikit_rawtouch.h"
#include <array>
#include <deque>
#include <yoga/Yoga.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_set>

void require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}


struct FixtureJukebox {
  long long time = 0;
  bool paused = false;
  std::function<void()> beforeResume;
  long long getTimeMicros() const { return time; }
  void stop() {}
  void pause() { paused = true; }
  void pauseClock() { paused = true; }
  void resume() { if (beforeResume) beforeResume(); paused = false; }
  bool isPaused() const { return paused; }
  void playKeySound(int) { require(false, "Watch must not request live keysounds"); }
};

std::deque<IOSRawTouchEvent> reminderTouches;
extern "C" size_t IOSPopRawTouchEvents(IOSRawTouchEvent *buffer, size_t capacity) {
  size_t count = 0;
  while (count < capacity && !reminderTouches.empty()) {
    buffer[count++] = reminderTouches.front();
    reminderTouches.pop_front();
  }
  return count;
}

struct FixtureInput {
  void stopListen() {}
  void pumpPendingTouchEvents() {}
  void discardPendingTouchEvents() {}
};

struct FixtureTouchRouter {
  bool advanceSpinScratch(long long) { return true; }
};

struct FixtureWorker {
  gameplay::RealtimeGameplaySnapshot snapshot;
  std::unique_ptr<gameplay::RealtimeGameplayWorker> native;
  gameplay::RealtimeGameplayFault fault() const {
    return native ? native->fault() : gameplay::RealtimeGameplayFault::None;
  }
  std::shared_ptr<gameplay::RealtimeGameplaySnapshot> acquireLatestSnapshot() const {
    if (native) {
      auto lease = native->acquireLatestSnapshot();
      return lease ? std::make_shared<gameplay::RealtimeGameplaySnapshot>(*lease) : nullptr;
    }
    return std::make_shared<gameplay::RealtimeGameplaySnapshot>(snapshot);
  }
  std::function<void()> beforeStop;
  bool suspend() { return native ? native->suspend() : true; }
  bool requestSuspend() { return native ? native->requestSuspend() : true; }
  bool resume() { return native ? native->resume() : true; }
  void stop() {
    if (beforeStop) beforeStop();
    native->stop();
  }
  auto copyGaugeHistoryAfterStop() const { return native->copyGaugeHistoryAfterStop(); }
  auto copyGaugeHistoriesAfterStop() const { return native->copyGaugeHistoriesAfterStop(); }
  auto copyAcceptedReplayInputAfterStop() const { return native->copyAcceptedReplayInputAfterStop(); }
  auto copyReplayEventsAfterStop() const { return native->copyReplayEventsAfterStop(); }
};

struct FixtureRealtimeSession {
  std::mutex inputInterruptionMutex;
  FixtureJukebox *audio = nullptr;
  std::unique_ptr<input::RealtimePhysicalInputRouter> physicalInputRouter;
  std::atomic_bool inputInterrupted{false};
  std::atomic_bool inputFallbackReady{false};
  std::atomic_bool inputInterruptionAcknowledged{false};
  gameplay::BoundedMpscQueue<input::LogicalInputTransition, 16> inputCommands;
  void interruptInput(const input::InputInterruption &interruption);
  std::mutex touchRouterMutex;
  std::unique_ptr<FixtureTouchRouter> touchRouter;
  std::atomic_bool acceptingTouch{false};
  std::atomic_bool touchRoutingRecoveryRequested{false};
  std::unique_ptr<FixtureWorker> worker;
  gameplay::BoundedMpscQueue<gameplay::StartSelectControlInput, 16> startSelectInputs;
  std::atomic_bool startSelectInputOverflow{false};
  std::vector<bms_parser::Note *> notes;
  std::uint64_t appliedSnapshotGeneration = 0;
  std::uint64_t appliedNoteRevision = 0;
  std::uint64_t appliedGraphJudgementRevision = 0;
  std::uint64_t appliedGraphGaugeRevision = 0;
  std::uint64_t appliedTransactionSequence = 0;
  std::atomic_bool acceptingNativeInput{false};
  std::unique_ptr<gameplay::RealtimeGameplayInputRegistration> inputRegistration;
};

struct FixturePresentation {
  std::unordered_map<ChartVisualId, NotePresentationState> noteStates;
  void setNoteState(NotePresentationState state) {
    noteStates.insert_or_assign(state.id, state);
  }
  void onLanePressed(int, JudgeResult, long long) {}
  void onLaneReleased(int, long long) {}
  void onJudge(JudgeResult, int, int, PlayfieldJudgeEventClock, bool) {}
  void applyGameplayGraphState(const SkinGameplayDynamicGraphState &) {}
  void clearLiveTouchPoints() {}
};

struct FixturePauseView {
  bool visible = false;
  YGDisplay display = YGDisplayFlex;
  void setDisplay(YGDisplay value) { display = value; }
  void setVisible(bool value) { visible = value; }
  bool getVisible() const { return visible; }
};

struct ReminderSceneManager {
  std::unordered_set<Scene *> backgroundScenes;
  int returns = 0;
  template <typename Destination> void changeScene(Destination, bool) { ++returns; }
};

class GamePlayScene {
public:
  bms_parser::Chart ownedChart;
  bms_parser::Chart *chart = &ownedChart;
  std::unique_ptr<RhythmState> state;
  StartOptions options;
  struct {
    FixtureJukebox jukebox;
    ReminderSceneManager reminderSceneManager;
    ReminderSceneManager *sceneManager = &reminderSceneManager;
    struct {
      int fallbackCompletions = 0;
      void resetGyroscopeTurntableSession() {}
      void completeRealtimeInputFallback() { ++fallbackCompletions; }
    } inputDeviceRegistry;
  } context;
  FixtureInput *inputHandler = nullptr;
  std::unique_ptr<FixtureRealtimeSession> realtimeGameplaySession;
  std::optional<gameplay::StartSelectControl> startSelectControl;
  bool practiceMenuActive = false;
  long long practiceMenuStartPressedMicros = 0;
  bool touchVisualizerLoaded = false;
  bool recordedAttemptCompleted = false;
  bool resultTransitionScheduled = false;
  bool startButtonPressed = false;
  bool selectButtonPressed = false;
  bool playfieldChangeLiftTarget = false;
  ReplayData recordedReplay;
  ReplayData analyticsReplay;
  struct CompletedModernReplayCapture {
    std::optional<std::vector<replay::InputTransition>> acceptedInput;
    std::vector<replay::ReplayTouchSample> touchSamples;
    std::vector<replay::ReplayLaneCoverEvent> laneCoverEvents;
    replay::ReplayTimeBounds timeBounds;
  };
  std::unique_ptr<replay::ReplayInputRecorder> modernReplayInputRecorder;
  std::optional<std::vector<replay::InputTransition>> completedModernReplayInput;
  std::string modernReplayCaptureDiagnostic;
  std::size_t replayEventCursor = 0;
  std::optional<GaugeStateSnapshot> courseStageInitialGauge;
  bool playfieldLaneCoverEnabled = false;
  ScoreProvenance attemptProvenance = ScoreProvenance::Legacy();
  FixturePauseView *pauseLayout = nullptr;
  FixturePauseView *pausePenaltyText = nullptr;
  FixturePauseView *pauseButton = nullptr;
  FixturePauseView *practiceRestartButton = nullptr;
  bool realtimeGameplayAuthorityWaitingForSkinGeometry = false;
  bool playbackInitializationFailed = false;
  gameplay::IpadGestureReminder ipadGestureReminder;
  bool ipadGestureReminderPending = false;
  bool ipadGestureReminderReady = false;
  bool ipadGestureReminderExiting = false;
  bool ipadGestureReminderBackground = false;
  FixturePauseView reminderLayout;
  FixturePauseView *ipadGestureReminderLayout = &reminderLayout;
  void pumpIpadGestureReminderTouches();
  void showIpadGestureReminder() { reminderLayout.setVisible(true); }
  void onApplicationBackgroundChanged(bool background);
  void returnFromIpadGestureReminder();
  int attemptStarts = 0;
  bool nativeBackground = false;
  bool startedWhileNativeBackground = false;
  bool startPreparedAttempt() {
    ++attemptStarts;
    startedWhileNativeBackground |= nativeBackground;
    state->isPlaying = true;
    return true;
  }
  bool queueDeferred = false;
  unsigned frame = 0;
  std::vector<std::pair<unsigned, std::function<bool()>>> deferred;
  void finishFrame() {
    auto callbacks = std::move(deferred);
    deferred.clear();
    for (auto &entry : callbacks) {
      if (entry.first <= frame) entry.second();
      else deferred.push_back(std::move(entry));
    }
    ++frame;
  }
  bool useProductionResetBoundary = false;
  int transitions = 0;
  int resets = 0;
  std::unique_ptr<RhythmState> stoppedSnapshot;
  long long offset = 0;
  long long clock = 0;
  std::optional<std::int64_t> sourcePlaytime;
  Judge judge{2};
  gameplay::GameplayPolicyBuildOutcome rulesetPolicyBuild;
  std::unordered_map<int, bool> lanePressed;
  std::unordered_map<std::string, bms_parser::Note *> replayNoteLookup;
  PlayfieldVisualState capturedPlayfieldVisualState;
  std::unordered_map<const bms_parser::Note *, ChartVisualId> skinGameplayGraphSourceIds;
  std::unique_ptr<FixturePresentation> presentationEventFanout = std::make_unique<FixturePresentation>();
  std::unique_ptr<FixturePresentation> playfieldVisualStateStore = std::make_unique<FixturePresentation>();

  GamePlayScene() {
    ownedChart.Meta.TotalNotes = 2;
    ownedChart.Meta.KeyMode = 7;
    ownedChart.Meta.MD5 = std::string(32, 'b');
    ownedChart.Meta.SHA256 = std::string(64, 'a');
    ownedChart.Meta.BmsPath = "library/abort.bms";
    ownedChart.Meta.Rank = 2;
    ownedChart.Meta.LnMode = 1;
    ownedChart.Meta.HasTotal = true;
    ownedChart.Meta.Total = 200;
    auto *measure = new bms_parser::Measure();
    for (const long long timing : {2'000'000LL, 3'000'000LL}) {
      auto *timeline = new bms_parser::TimeLine(8, false);
      timeline->Timing = timing;
      timeline->SetNote(0, new bms_parser::Note(1));
      measure->TimeLines.push_back(timeline);
    }
    ownedChart.Measures.push_back(measure);
    state = std::make_unique<RhythmState>(chart, false);
    state->isPlaying = true;
  }

  void update(float dt);
  void showPauseMenu(bool pausePlayback);
  bool inputInterruptionPause = false;
  void closePauseMenu();
  bool drainRealtimeInputInterruption();
  void togglePauseMenuFromInput();
  void restartCurrentPattern();
  void resetAttemptBoundaryForTest();
  void resetCoursePauseHold() {}
  void updateSkinResetLayoutVisibility() {}
  template <typename Callback> void defer(Callback callback, int, bool waitFrame) {
    if (queueDeferred) deferred.emplace_back(frame + (waitFrame ? 1 : 0), callback);
    else callback();
  }
  void completePracticeSection(bool realtimeRangeFinalized);
  void finalizePracticeRangeMisses();
  void completePracticeAttempt();
  void finishReplayRecording();
  void abortPlayFromStartSelectControl();
  void consumeStartSelectInput(const gameplay::StartSelectControlInput &input);
  void drainRealtimeStartSelectInputs();
  CompletedModernReplayCapture completeModernReplayCapture();
  void processReplayEvents(long long gameplayTimeMicros);
  void recordModernCourseStage(const CompletedModernReplayCapture &capture);
  practice::ResultCapturePolicy resultCapturePolicy() const;
  int effectiveNoteStartPositionPercent() const { return 0; }

  bool realtimeGameplayAuthorityActive() const {
    return realtimeGameplaySession != nullptr;
  }
  void applyPendingBestReplay() {}
  void drainRealtimeInputCommands() {}
  std::function<void()> onIngressClosed;
  std::function<void()> onTouchDrain;
  void setRealtimeGameplayIngressEnabled(bool enabled) {
    if (!enabled && onIngressClosed) onIngressClosed();
    if (realtimeGameplaySession) {
      const std::lock_guard lock(realtimeGameplaySession->inputInterruptionMutex);
      if (realtimeGameplaySession->physicalInputRouter &&
          (!enabled || !realtimeGameplaySession->inputInterrupted.load())) {
        realtimeGameplaySession->physicalInputRouter->setGameplayEnabled(enabled, clock);
      }
    }
  }
  void drainRealtimeTouchSamples() { if (onTouchDrain) onTouchDrain(); }
  long long nowMicros() const { return clock; }
  void startPracticeAttemptFromMenu() { require(false, "unexpected practice menu"); }
  bool isReplayPlayback() const { return options.replayData != nullptr; }
  void applyStartSelectControlActions(
      const std::vector<gameplay::StartSelectControlAction> &actions) {
    for (const auto &action : actions) {
      if (action.kind == gameplay::StartSelectControlActionKind::Exit) {
        abortPlayFromStartSelectControl();
      } else {
        require(action.kind == gameplay::StartSelectControlActionKind::ToggleLiftHiddenTarget,
                "fixture permits only the conjunction's preceding lift toggle");
        playfieldChangeLiftTarget = !playfieldChangeLiftTarget;
      }
    }
  }
  void updateCoursePauseHoldProgress(long long) {}
  long long getAudioOffsetMicros() const { return offset; }
  long long getGameplayTimeMicros(long long raw) const { return raw + offset; }
  void updatePracticeHud(long long) {}
  void syncRealtimeGameplaySnapshot() {
    if (realtimeGameplaySession && realtimeGameplaySession->worker &&
        realtimeGameplaySession->worker->native) {
      syncRealtimeGameplaySnapshotFromWorker();
      return;
    }
    require(stoppedSnapshot != nullptr, "realtime fixture must supply a final domain snapshot");
    *state = *stoppedSnapshot;
  }
  void syncRealtimeGameplaySnapshotFromWorker();
  void stopRealtimeGameplayAuthorityFromWorker(bool transferReplay);
  void refreshRealtimeTouchLayout() {}
  long long getVisualOffsetMicros() const { return 0; }
  void updateLaneStateText() {}
  void updateGaugeStatusText() {}
  void updatePacemakerStatus() {}
  void updateRealtimeVisualTimeline(long long) {}
  bool practiceReplayEventAllowed(const ReplayEvent &) const { return true; }
  long long getVisualTimeMicros(long long time) const { return time; }
  void applyReplayEvent(const ReplayEvent &event, long long visualTimeMicros);
  void applyReplayGauge(const ReplayEvent &event);
  void buildReplayNoteLookup();
  bms_parser::Note *findReplayNote(const ReplayEvent &event) const;
  JudgeResult pressNote(bms_parser::Note *, long long, const JudgeResult *, long long, bool);
  JudgeResult releaseNote(bms_parser::Note *, long long, const JudgeResult *, long long, bool);
  void expireGimmickNote(bms_parser::Note *, long long);
  void processReplayLaneCoverEvents(long long) {}
  bool preparationIndicatorActive(long long) const { return false; }
  std::optional<std::int64_t> beatorajaPlaytimeMillis(
      bms_parser::Chart *, const StartOptions &) const { return sourcePlaytime; }
  void stopRealtimeGameplayAuthority(bool transferReplay) {
    if (realtimeGameplaySession && realtimeGameplaySession->worker &&
        realtimeGameplaySession->worker->native) {
      stopRealtimeGameplayAuthorityFromWorker(transferReplay);
      return;
    }
    if (realtimeGameplaySession != nullptr && stoppedSnapshot != nullptr) {
      require(transferReplay, "abort must transfer the final accepted snapshot");
      *state = *stoppedSnapshot;
    }
    realtimeGameplaySession.reset();
  }
  void showPlaybackInitializationFailure(const char *) {
    require(false, "unexpected realtime failure");
  }
  void updateHellChargeGauge(long long) {}
  bool finishIfGaugeFailed();
  void updateSkinGameplayGraph(long long) {}
  void checkPassedTimeline(long long time) {
    require(time < 2'000'000 || options.practiceSession != nullptr,
            "fixture timeline driver only covers the opening gap");
  }
  void publishPracticeGhost() {}
  bool isCoursePlayback() const { return options.courseSession != nullptr; }
  bool usesModernCourseContinuation() const { return false; }
  bool shouldRecordReplay() const { return true; }
  void cancelGameplaySkinPreparation() {}
  void finishPractice();
  void scheduleResultTransition(std::uint64_t) {
    require(state->isEnding, "transition must capture terminal state");
    if (!resultTransitionScheduled) {
      ++transitions;
      resultTransitionScheduled = true;
    }
  }
  std::uint64_t selectedSkinResultTransitionDelayMillis(long long) { return 0; }
  std::optional<NoteTimeRange> practiceNoteRange() const {
    if (!options.practiceSession) {
      return std::nullopt;
    }
    const auto &configuration = options.practiceSession->configuration();
    return NoteTimeRange{configuration.startMicros, configuration.endMicros};
  }
  PlayfieldJudgeEventClock judgeEventClock(long long time) {
    return makePlayfieldJudgeEventClock(time, 0);
  }
  void onJudge(const JudgeResult &judge, PlayfieldJudgeEventClock, bool,
               const bms_parser::Note *) {
    if (!state->isEnding) {
      state->commitJudge(judge);
    }
  }
  void appendReplayEvent(ReplayEventAction action, int lane,
                         const bms_parser::Note *note, long long songTime,
                         long long judgeTime, const JudgeResult &judge,
                         bool checkGaugeFailure = true);
  void reset() {
    if (useProductionResetBoundary) resetAttemptBoundaryForTest();
    ++resets;
    state = std::make_unique<RhythmState>(chart, false);
    state->isPlaying = true;
    context.jukebox.time = -offset;
    for (auto *measure : chart->Measures) {
      for (auto *timeline : measure->TimeLines) {
        for (auto *note : timeline->Notes) {
          if (note != nullptr) {
            note->Reset();
          }
        }
      }
    }
    recordedReplay = {};
    if (options.practiceSession) options.practiceSession->beginAttempt();
  }
};

SCENE_METHODS

struct ResultPersistenceOptions {
  result_persistence::SaveOutcome outcome;
};

RESULT_PERSIST_HELPERS

class ResultScene {
public:
  struct LocalSource {
    struct CourseOptions {
      std::shared_ptr<CoursePlaySession> session;
      bool savedResultBrowsing = false;
    } courseOptions;
    RhythmState resultState{nullptr, false};
    bool courseTransitionStarted = false;
    ResultPersistenceOptions persistenceOptions;
    ScoreProvenance attemptProvenance = ScoreProvenance::Legacy();
    std::int64_t currentScoreDateUnixSeconds = 0;
  } local;
  struct {
    std::function<replay::CourseResultPersistenceOutcome(
        const replay::CapturedCourseReplayAttempt &)> persistModernCourse;
  } context;
  int summaries = 0;
  LocalSource *localSource() { return &local; }
  bool isCourseStageResult() const { return true; }
  bool isCourseFinalResult() const { return true; }
  long long recordCourseStageRestTime() { return 0; }
  void showCourseResult() { ++summaries; }
  void showSavedCourseStage() { require(false, "not browsing saved stages"); }
  void startCourseReplayStage(std::shared_ptr<CoursePlaySession>) {
    require(false, "not a course replay");
  }
  void continueCourse();
  bool persistModernCourseResult();
};

RESULT_CONTINUE_PREFIX
RESULT_PERSIST_METHOD

struct ApplicationContext {
  int resourceStarts = 0;
};

struct ReplayVideoExportOptions {
  bool includeResultScreen = false;
};

struct ReplayVideoExportResult {
  bool success = false;
  std::filesystem::path outputPath;
  std::string message;
};

class ReplayVideoExporter {
public:
  static ReplayVideoExportResult Export(
      ApplicationContext &, bms_parser::Chart *, const ReplayData &,
      const ReplayVideoExportOptions &);
};

using CourseMaterializedStages = std::vector<replay::CourseReplayMaterializedStage>;

EXPORT_PREFIXES

void testAbortExportAdmission(const ReplayData &replay) {
  GamePlayScene scene;
  ApplicationContext context;
  for (const bool resultScreen : {false, true}) {
    const auto outcome = ReplayVideoExporter::Export(
        context, scene.chart, replay, {.includeResultScreen = resultScreen});
    require(!outcome.success && outcome.message.find("unsupported") != std::string::npos &&
                context.resourceStarts == 0,
            "T2-R2: accepted abort export is explicitly unsupported before resource work");
  }
  CourseReplayData course;
  course.stages.resize(2);
  course.stages.back().replay = replay;
  const auto courseOutcome = exportCourseReplayImpl(context, course, nullptr, nullptr, {});
  require(!courseOutcome.success && context.resourceStarts == 0,
          "T2-R2: course export with an aborted stage stops before any stage resources");
  ReplayData ordinary;
  const auto ordinaryOutcome = ReplayVideoExporter::Export(context, scene.chart, ordinary, {});
  require(ordinaryOutcome.success && context.resourceStarts == 1,
          "T2-R2: ordinary export still reaches its existing resource boundary");
  course.stages.back().replay.abortedAtSongTimeMicros.reset();
  const auto ordinaryCourse = exportCourseReplayImpl(context, course, nullptr, nullptr, {});
  require(ordinaryCourse.success && context.resourceStarts == 2,
          "T2-R2: ordinary course export still reaches its existing resource boundary");
}

#include "gameplay_terminal_persistence_fixture.h"

void testPausePenaltyAndFreshAttemptBoundary() {
  for (const auto ruleset : {GameplayRuleset::LR2, GameplayRuleset::Beatoraja}) {
    GamePlayScene scene;
    FixturePauseView warning;
    scene.pausePenaltyText = &warning;
    scene.options.ruleset = ruleset;
    scene.options.gaugeType = GaugeType::Hard;
    const auto *selectedAssist = ruleset == GameplayRuleset::LR2
                                    ? assist_options::kBpmGuide
                                    : assist_options::kOff;
    scene.options.assistOption = selectedAssist;
    scene.chart->Meta.MinBpm = scene.chart->Meta.MaxBpm = 120;
    scene.rulesetPolicyBuild = buildGameplayRulesetPolicyAtPlayStart(
        scene.options, *scene.chart, AppSettings::NotePriorityMode::Lowest);
    require(scene.rulesetPolicyBuild.built(), "pause fixture has a canonical policy");
    scene.attemptProvenance = captureScoreProvenanceAtPlayStart(
        scene.options, scene.chart->Meta, *scene.rulesetPolicyBuild.policy);
    scene.state->configureGauge(GaugeType::Hard, GaugeAutoShiftMode::None);
    require(scene.attemptProvenance.eligibility == ScoreEligibility::Verified,
            "unpaused manual attempt starts verified");
    scene.context.jukebox.time = 2'500'000;
    scene.showPauseMenu(true);
    require(scene.context.jukebox.paused &&
                scene.attemptProvenance.assistOption == assist_options::kAssisted &&
                scene.attemptProvenance.eligibility == ScoreEligibility::Modified &&
                scene.recordedReplay.provenance == scene.attemptProvenance &&
                scene.analyticsReplay.provenance == scene.attemptProvenance &&
                scene.recordedReplay.assistOption == assist_options::kAssisted &&
                scene.analyticsReplay.assistOption == assist_options::kAssisted &&
                scene.options.assistOption == selectedAssist &&
                scene.state->getClearType() == ClearType::LightAssistedEasyClear,
            "actual pause marks the attempt and its replay assisted and unranked");
    require(warning.getVisible(), "assisted pause shows the warning");
    scene.realtimeGameplaySession = std::make_unique<FixtureRealtimeSession>();
    auto &session = *scene.realtimeGameplaySession;
    session.worker = std::make_unique<FixtureWorker>();
    session.worker->snapshot.generation = 1;
    session.worker->snapshot.gaugeState = scene.state->gaugeSnapshot();
    scene.syncRealtimeGameplaySnapshotFromWorker();
    require(scene.state->getClearType() == ClearType::LightAssistedEasyClear,
            "worker snapshot synchronization cannot erase the pause lamp cap");
    scene.realtimeGameplaySession.reset();
    scene.closePauseMenu();
    require(!scene.context.jukebox.paused &&
                scene.attemptProvenance.assistOption == assist_options::kAssisted &&
                scene.state->lightAssistClearMark,
            "resume retains the pause penalty");
    scene.state->stagePassedNotes = scene.chart->Meta.TotalNotes;
    scene.showPauseMenu(true);
    require(scene.attemptProvenance.assistOption == assist_options::kAssisted &&
                warning.getVisible(),
            "a safe later pause retains the earlier penalty and warning");
    scene.closePauseMenu();
    scene.useProductionResetBoundary = true;
    scene.restartCurrentPattern();
    require(scene.resets == 1 &&
                scene.attemptProvenance.assistOption == assist_options::kOff &&
                scene.attemptProvenance.eligibility == ScoreEligibility::Verified &&
                !scene.state->lightAssistClearMark,
            "Retry Same and Retry without randomization reset the penalty at the real attempt boundary");
    scene.context.jukebox.time = 0;
    scene.showPauseMenu(true);
    require(!warning.getVisible() && warning.display == YGDisplayNone,
            "safe pause after retry hides the previous attempt's warning");
    scene.closePauseMenu();
    scene.context.jukebox.time = 2'500'000;
    scene.showPauseMenu(true);
    require(scene.attemptProvenance.assistOption == assist_options::kAssisted &&
                warning.getVisible() && warning.display == YGDisplayFlex,
            "a fresh attempt can acquire its own pause penalty and warning");
  }
  for (const auto *assist : {assist_options::kDrag, assist_options::kBpmGuide}) {
    GamePlayScene scene;
    scene.options.assistOption = assist;
    scene.chart->Meta.MinBpm = 120;
    scene.chart->Meta.MaxBpm = 180;
    scene.rulesetPolicyBuild = buildGameplayRulesetPolicyAtPlayStart(
        scene.options, *scene.chart, AppSettings::NotePriorityMode::Lowest);
    scene.attemptProvenance = captureScoreProvenanceAtPlayStart(
        scene.options, scene.chart->Meta, *scene.rulesetPolicyBuild.policy);
    scene.context.jukebox.time = 2'500'000;
    scene.showPauseMenu(true);
    require(scene.attemptProvenance.assistOption == assist &&
                scene.recordedReplay.assistOption == assist &&
                scene.options.assistOption == assist,
            "pause retains the effective assist option already in use");
  }
  for (const bool replay : {false, true}) {
    GamePlayScene scene;
    if (replay) scene.options.replayData = std::make_shared<ReplayData>();
    else scene.options.courseSession = std::make_shared<CoursePlaySession>();
    const auto before = scene.attemptProvenance;
    scene.togglePauseMenuFromInput();
    require(scene.attemptProvenance == before && !scene.state->lightAssistClearMark,
            "course menus and pausing Watch never penalize a recorded attempt");
    require(scene.context.jukebox.paused == replay,
            "course menu leaves the song running while Watch remains pausable");
  }
}

void testPauseAfterEarlyJudgmentDisqualifiesIr() {
  for (const bool realtime : {false, true}) {
    for (const int handled : {0, 1, 2}) {
      GamePlayScene scene;
      scene.options.ruleset = GameplayRuleset::LR2;
      scene.options.autoKeySound = true;
      scene.rulesetPolicyBuild = buildGameplayRulesetPolicyAtPlayStart(
          scene.options, *scene.chart, AppSettings::NotePriorityMode::Lowest);
      require(scene.rulesetPolicyBuild.built(), "early-pause fixture has a canonical policy");
      scene.attemptProvenance = captureScoreProvenanceAtPlayStart(
          scene.options, scene.chart->Meta, *scene.rulesetPolicyBuild.policy);
      const auto irEligible = [&] {
        return ir::tachi::isReplayEligibleForBokutachi(
            "11111111-1111-4111-8111-111111111111", true,
            scene.chart->Meta, scene.attemptProvenance);
      };
      require(irEligible(), "ordinary manual attempt starts IR eligible");
      auto &timelines = scene.chart->Measures.front()->TimeLines;
      timelines.back()->Timing = 2'000'000;
      scene.context.jukebox.time = 1'990'000;
      for (int i = 0; i < handled; ++i) {
        const auto result = scene.pressNote(timelines[i]->Notes[0],
                                           1'990'000, nullptr, 1'990'000, false);
        require(result.isNotePlayed(), "first chord can be judged before nominal note time");
      }
      require(scene.state->stagePassedNotes == handled,
              "early judgments advance the actual handled-note count");
      if (realtime) {
        scene.realtimeGameplaySession = std::make_unique<FixtureRealtimeSession>();
        auto &session = *scene.realtimeGameplaySession;
        session.worker = std::make_unique<FixtureWorker>();
        session.worker->snapshot.attempt.stagePassedNotes = handled;
        session.notes = buildRealtimeGameplayNoteLookup(*scene.chart);
        session.worker->snapshot.noteStates.resize(session.notes.size());
        // Deliberately disagree with the authoritative count in both directions.
        scene.state->stagePassedNotes = handled == 0 ? 1 : 0;
      }
      scene.showPauseMenu(true);
      const bool penalized = handled == 1;
      require(scene.state->lightAssistClearMark == penalized,
              "early judgment ends lead-in exemption while completed play stays exempt");
      require(irEligible() == !penalized,
              "pausing after an early judgment disqualifies the attempt from IR");
      if (penalized) {
        require(scene.attemptProvenance.eligibility == ScoreEligibility::Modified &&
                    scene.recordedReplay.provenance == scene.attemptProvenance &&
                    scene.analyticsReplay.provenance == scene.attemptProvenance,
                "early pause propagates modified provenance to both replay captures");
      }
    }
  }
}

void testPauseOnlyPenalizesUnfinishedNotePlay() {
  for (const auto offset : {-100'000LL, 0LL, 100'000LL}) {
    for (const auto &[time, handled, penalized] :
         {std::tuple{1'999'999LL, 0, false},
          std::tuple{2'000'000LL, 0, true},
          std::tuple{2'500'000LL, 1, true},
          std::tuple{3'050'000LL, 1, true},
          std::tuple{2'950'000LL, 2, false},
          std::tuple{3'500'000LL, 2, false}}) {
      for (const bool realtime : {false, true}) {
        GamePlayScene scene;
        FixturePauseView warning;
        scene.pausePenaltyText = &warning;
        auto *leadIn = new bms_parser::TimeLine(8, false);
        leadIn->Timing = 0;
        leadIn->AddBackgroundNote(new bms_parser::Note(1));
        leadIn->SetInvisibleNote(1, new bms_parser::Note(1));
        auto &timelines = scene.chart->Measures.front()->TimeLines;
        timelines.insert(timelines.begin(), leadIn);
        scene.offset = offset;
        scene.context.jukebox.time = time - offset;
        if (!realtime) {
          for (int i = 0; i < handled; ++i) {
            scene.state->commitJudge(JudgeResult(i == 0 ? PGreat : Poor, 0));
          }
        }
        if (realtime) {
          scene.realtimeGameplaySession = std::make_unique<FixtureRealtimeSession>();
          auto &session = *scene.realtimeGameplaySession;
          session.worker = std::make_unique<FixtureWorker>();
          session.worker->snapshot.generation = 1;
          session.worker->snapshot.attempt.stagePassedNotes = handled;
        }
        const auto before = scene.attemptProvenance;
        scene.showPauseMenu(true);
        require(scene.context.jukebox.paused, "pause remains available outside note play");
        require(scene.state->lightAssistClearMark == penalized,
                "pause penalty starts at the first note and ends only when all notes are handled");
        require(warning.getVisible() == penalized &&
                    warning.display == (penalized ? YGDisplayFlex : YGDisplayNone),
                "safe unassisted pauses hide the penalty warning");
        if (!penalized) {
          require(scene.attemptProvenance == before,
                  "safe pauses preserve eligibility and assist metadata");
        }
      }
    }
  }
  GamePlayScene empty;
  empty.chart->Meta.TotalNotes = 0;
  empty.context.jukebox.time = 2'500'000;
  empty.showPauseMenu(true);
  require(!empty.state->lightAssistClearMark, "empty charts never incur pause penalties");
}

void testPausePenaltyIncludesRemainingMinesAndLongNoteGaugeEffects() {
  // All scored notes can be resolved while a trailing mine can still damage
  // the gauge. The worker's latest note flags override stale presentation.
  for (const bool realtime : {false, true}) {
    for (const bool resolved : {false, true}) {
      GamePlayScene scene;
      auto *timeline = new bms_parser::TimeLine(8, false);
      timeline->Timing = 4'000'000;
      auto *mine = new bms_parser::LandmineNote(20);
      timeline->SetLandmineNote(1, mine);
      scene.chart->Measures.front()->TimeLines.push_back(timeline);
      scene.chart->Meta.TotalLandmineNotes = 1;
      scene.context.jukebox.time = resolved ? 4'100'000 : 3'500'000;
      for (int i = 0; i < 2; ++i) scene.state->commitJudge(JudgeResult(PGreat, 0));
      mine->IsDead = resolved;
      if (realtime) {
        scene.realtimeGameplaySession = std::make_unique<FixtureRealtimeSession>();
        auto &session = *scene.realtimeGameplaySession;
        session.worker = std::make_unique<FixtureWorker>();
        session.notes = buildRealtimeGameplayNoteLookup(*scene.chart);
        session.worker->snapshot.noteStates.resize(session.notes.size());
        session.worker->snapshot.attempt.stagePassedNotes = 2;
        session.worker->snapshot.noteStates.back().dead = resolved;
        mine->IsDead = !resolved;
      }
      scene.showPauseMenu(true);
      require(scene.state->lightAssistClearMark == !resolved,
              "trailing mine keeps pause assisted until authoritative mine resolution");
    }
  }
  for (const long long time : {999'999LL, 1'000'000LL}) {
    GamePlayScene scene;
    auto *timeline = new bms_parser::TimeLine(8, false);
    timeline->Timing = 1'000'000;
    timeline->SetLandmineNote(1, new bms_parser::LandmineNote(20));
    scene.chart->Measures.front()->TimeLines.insert(
        scene.chart->Measures.front()->TimeLines.begin(), timeline);
    scene.context.jukebox.time = time;
    scene.showPauseMenu(true);
    require(scene.state->lightAssistClearMark == (time == 1'000'000),
            "lead-in exemption ends at a leading mine's score-affecting timing");
  }
  for (const auto type : {bms_parser::LongNoteType::LongNote,
                         bms_parser::LongNoteType::ChargeNote,
                         bms_parser::LongNoteType::HellChargeNote}) {
    for (const bool realtime : {false, true}) {
      for (const long long time : {2'800'000LL, 3'000'000LL}) {
        GamePlayScene scene;
        auto &timelines = scene.chart->Measures.front()->TimeLines;
        for (auto *timeline : timelines) {
          delete timeline->Notes[0];
          timeline->Notes[0] = nullptr;
        }
        auto *head = new bms_parser::LongNote(1, type);
        auto *tail = new bms_parser::LongNote(1, type);
        head->Tail = tail;
        tail->Head = head;
        timelines[0]->SetNote(1, head);
        timelines[1]->SetNote(1, tail);
        scene.chart->Meta.TotalNotes = type == bms_parser::LongNoteType::LongNote ? 1 : 2;
        scene.context.jukebox.time = time;
        for (int i = 0; i < scene.chart->Meta.TotalNotes; ++i) {
          scene.state->commitJudge(JudgeResult(PGreat, 0));
        }
        head->IsPlayed = true;
        head->PlayedTime = 2'000'000;
        tail->IsPlayed = true;
        tail->IsDead = true;
        tail->PlayedTime = 2'750'000;
        if (realtime) {
          scene.realtimeGameplaySession = std::make_unique<FixtureRealtimeSession>();
          auto &session = *scene.realtimeGameplaySession;
          session.worker = std::make_unique<FixtureWorker>();
          session.notes = buildRealtimeGameplayNoteLookup(*scene.chart);
          session.worker->snapshot.attempt.stagePassedNotes = scene.chart->Meta.TotalNotes;
          session.worker->snapshot.noteStates = {
              {.played = true, .playedTimeMicros = 2'000'000},
              {.played = true, .dead = true, .playedTimeMicros = 2'750'000}};
          // The UI copy has already lost the early-release timing.
          tail->PlayedTime = 3'000'000;
        }
        scene.showPauseMenu(true);
        const bool hasGaugeInterval =
            type == bms_parser::LongNoteType::HellChargeNote && time < 3'000'000;
        require(scene.state->lightAssistClearMark == hasGaugeInterval,
                "early-resolved HCN tail keeps pause assisted only through its remaining gauge interval");
      }
    }
  }
}

void testPractice(bool loop, bool chartTerminal, long long offset) {
  GamePlayScene scene;
  scene.offset = offset;
  practice::Configuration configuration;
  configuration.startMicros = 0;
  configuration.endMicros = 4'000'000;
  configuration.loop = loop;
  scene.options.practiceSession = std::make_shared<practice::Session>(configuration);
  scene.options.practiceSession->beginAttempt();
  for (const long long time : {0LL, 100'000LL, 500'000LL, 1'000'000LL}) {
    scene.context.jukebox.time = time - offset;
    scene.update(0.016F);
    require(scene.options.practiceSession->completedAttempts().empty(),
            "COR01: in-range unfinished legacy update must not complete practice");
    require(scene.state->judgeCount[Poor] == 0 && scene.transitions == 0 &&
                scene.resets == 0 && !scene.state->isEnding,
            "COR01: in-range updates must not finalize pending notes");
  }
  if (chartTerminal) {
    scene.state->passedMeasureCount = scene.chart->Measures.size();
  } else {
    scene.context.jukebox.time = 4'000'000 - offset;
  }
  scene.update(0.016F);
  const auto &attempts = scene.options.practiceSession->completedAttempts();
  require(attempts.size() == 1 && attempts.front().events.size() == 2,
          "COR01: legitimate boundary completes once and finalizes two notes");
  require(scene.transitions == (loop ? 0 : 1) && scene.resets == (loop ? 1 : 0),
          "COR01: boundary preserves loop versus result routing");
  scene.update(0.016F);
  require(attempts.size() == 1 && scene.transitions == (loop ? 0 : 1) &&
              scene.resets == (loop ? 1 : 0),
          "COR01: repeated terminal update cannot duplicate completion");
}

void testQueuedAbortLifetime() {
  for (const auto kind : {replay::LogicalControlKind::Lane,
                          replay::LogicalControlKind::ScratchClockwise}) {
    GamePlayScene scene;
    scene.realtimeGameplaySession = std::make_unique<FixtureRealtimeSession>();
    scene.startSelectControl.emplace(gameplay::StartSelectControl::Configuration{});
    auto &queue = scene.realtimeGameplaySession->startSelectInputs;
    require(queue.tryPush({.control = {.kind = replay::LogicalControlKind::Start},
                           .pressed = true, .timestampMicros = 10'000'000}), "queue Start");
    require(queue.tryPush({.control = {.kind = replay::LogicalControlKind::Select},
                           .pressed = true, .timestampMicros = 10'000'100}), "queue Select");
    require(queue.tryPush({.control = {.kind = kind, .player = 1, .lane = 0},
                           .pressed = true, .timestampMicros = 11'000'101}),
            "queue lane/scratch after exit hold duration");
    scene.clock = 11'000'102;
    scene.update(0.016F);
    require(scene.realtimeGameplaySession == nullptr && scene.state->isEnding &&
                scene.transitions == 1,
            "GAME01: drain destroys authority and transitions exactly once");
    scene.update(0.016F);
    require(scene.transitions == 1, "GAME01: no duplicate terminal transition");
  }
}

void testAbortOutcome() {
  for (const auto gauge : {GaugeType::Hard, GaugeType::ExHard, GaugeType::Hazard,
                           GaugeType::Normal}) {
    for (const auto shift : {GaugeAutoShiftMode::None, GaugeAutoShiftMode::BestClear,
                             GaugeAutoShiftMode::SelectToUnder,
                             GaugeAutoShiftMode::SurvivalToGroove,
                             GaugeAutoShiftMode::Continue}) {
      for (const bool midway : {false, true}) {
        for (const bool realtime : {false, true}) {
          GamePlayScene scene;
          scene.state->configureGauge(gauge, shift);
          scene.options.gaugeType = gauge;
          scene.options.gaugeAutoShift = shift;
          configureAbortCapture(scene);
          if (midway) {
            auto *note = scene.chart->Measures.front()->TimeLines.front()->Notes[0];
            note->Play(2'000'000);
            scene.state->commitJudge(JudgeResult(PGreat, 0));
            recordAcceptedHit(scene);
          }
          scene.context.jukebox.time = midway ? 2'100'000 : 1'000'000;
          if (realtime) {
            scene.realtimeGameplaySession = std::make_unique<FixtureRealtimeSession>();
            scene.stoppedSnapshot = std::make_unique<RhythmState>(*scene.state);
          }
          scene.startSelectControl.emplace(gameplay::StartSelectControl::Configuration{});
          scene.consumeStartSelectInput({.control = {.kind = replay::LogicalControlKind::Start},
                                          .pressed = true, .timestampMicros = 10'000'000});
          scene.consumeStartSelectInput({.control = {.kind = replay::LogicalControlKind::Select},
                                          .pressed = true, .timestampMicros = 10'000'100});
          scene.applyStartSelectControlActions(scene.startSelectControl->tick(11'000'101));
          require(scene.state->getClearTypeRank() == kClearTypeFailedRank &&
                      scene.state->currentGauge == 0.0F,
                  "COR02: Start+Select must fail every gauge after final snapshot transfer");
          require(scene.state->judgeCount[PGreat] == (midway ? 1 : 0) &&
                      scene.state->judgeCount[Poor] == (midway ? 1 : 2) &&
                      scene.state->stagePassedNotes == 2 && scene.state->combo == 0,
                  "COR02: abort accounts remaining notes exactly once");
          require(scene.recordedReplay.clearType == kClearTypeFailedRank &&
                      scene.recordedReplay.finalGauge == 0.0F && scene.transitions == 1,
                  "COR02: replay and result transition retain failure");
          scene.abortPlayFromStartSelectControl();
          scene.finishReplayRecording();
          require(scene.state->judgeCount[Poor] == (midway ? 1 : 2) &&
                      scene.transitions == 1 && scene.recordedReplay.clearType == kClearTypeFailedRank,
                  "COR02: repeated terminal capture must not change the outcome");
          if (!realtime) {
            testDurableAbort(scene, midway);
          }
        }
      }
    }
  }
}

void testSignedAbortCaptureAndWatch() {
  for (const auto abortTime : {-30'000'000LL, -1'000'000LL, -1LL, 1'000'000LL}) {
    GamePlayScene scene;
    configureAbortCapture(scene);
    scene.context.jukebox.time = abortTime;
    scene.abortPlayFromStartSelectControl();
    require(scene.recordedReplay.abortedAtSongTimeMicros == abortTime,
            "live abort records the exact authoritative song time");
    scene.context.jukebox.time = 7'000'000;
    const auto capture = scene.completeModernReplayCapture();
    require(capture.timeBounds.completionSongTimeMicros == abortTime &&
                capture.timeBounds.aborted == true && capture.acceptedInput,
            "completed capture retains recorded abort rather than rereading or clamping the clock");
    std::string diagnostic;
    const auto result = result_persistence::captureModernChartResult(
        "123e4567-e89b-42d3-a456-426614174000", scene.chart->Meta, *scene.state,
        scene.attemptProvenance, 0, 1'700'000'000'000, diagnostic);
    require(result.has_value(), diagnostic);
    const auto attempt = replay::captureChartReplayPersistenceAttempt(
        {.result = *result,
         .setupFacts = {.chart = {.md5 = scene.chart->Meta.MD5,
                                  .sha256 = scene.chart->Meta.SHA256, .keyMode = 7},
                        .longNoteMode = 1},
         .acceptedInput = capture.acceptedInput,
         .touchSamples = capture.touchSamples,
         .laneCoverEvents = capture.laneCoverEvents,
         .timeBounds = capture.timeBounds}, diagnostic);
    require(attempt && attempt->replay, diagnostic);
    replay::BeatorajaReplayCodec codec;
    const auto bytes = codec.encodeChart(*attempt->replay, 0, diagnostic);
    require(bytes.has_value(), diagnostic);
    const auto decoded = codec.decode(*bytes, {.stageKeyModes = {7}});
    require(decoded.chart && decoded.chart->timeBounds == capture.timeBounds,
            "codec roundtrip retains authoritative signed abort");
    auto chart = freshAbortChart();
    const auto materialized = replay::ReplayPlaybackMaterializer::materializeForConsumers(
        *decoded.chart, *result, *chart, 128);
    require(materialized.matched() && materialized.playable(), materialized.diagnostic);
    require(materialized.replayData->abortedAtSongTimeMicros == abortTime,
            "consumer materialization retains authoritative signed abort");
    GamePlayScene watch;
    watch.options.replayData = materialized.replayData;
    watch.buildReplayNoteLookup();
    watch.processReplayEvents(abortTime - 1);
    require(!watch.state->isEnding && watch.transitions == 0,
            "Watch does not abort before the recorded timestamp");
    watch.processReplayEvents(abortTime);
    require(watch.state->isEnding && watch.transitions == 1 &&
                watch.state->judgeCount[Poor] == 2 &&
                watch.recordedReplay.abortedAtSongTimeMicros == abortTime,
            "Watch aborts at the signed timestamp and accounts remaining notes once");
  }
}

void testAbortCaptureRejectsLateEvidence() {
  for (const auto abortTime : {-1'000'000LL, 1'000'000LL}) {
    for (const int stream : {0, 1, 2}) {
      GamePlayScene scene;
      configureAbortCapture(scene);
      scene.context.jukebox.time = abortTime;
      scene.abortPlayFromStartSelectControl();
      std::string diagnostic;
      if (stream == 0) {
        require(scene.modernReplayInputRecorder->recordSongTime(abortTime + 1,
                    {.kind = replay::LogicalControlKind::Lane, .player = 1, .lane = 0},
                    true, diagnostic), diagnostic);
      } else if (stream == 1) {
        scene.recordedReplay.touchSamples.push_back({.action = ReplayTouchAction::Down,
            .fingerId = 1, .songTimeMicros = abortTime + 1, .x = 0.5F, .y = 0.5F});
      } else {
        scene.recordedReplay.laneCoverEvents.push_back({.songTimeMicros = abortTime + 1,
            .noteStartPositionPercent = 20});
      }
      const auto capture = scene.completeModernReplayCapture();
      require(capture.timeBounds.completionSongTimeMicros == abortTime,
              "late live evidence never moves the recorded abort boundary");
      if (stream == 0) {
        require(!capture.acceptedInput && !scene.modernReplayCaptureDiagnostic.empty(),
                "raw input after abort makes capture unavailable");
      } else {
        replay::ReplayPlaybackData playback;
        playback.setup.chart = {.md5 = scene.chart->Meta.MD5,
            .sha256 = scene.chart->Meta.SHA256, .keyMode = 7};
        playback.setup.longNoteMode = 1;
        playback.touchSamples = capture.touchSamples;
        playback.laneCoverEvents = capture.laneCoverEvents;
        const auto validation = replay::validateReplayPlayback(playback,
            replay::ReplaySetupSource::LocalCapture, capture.timeBounds);
        require(validation.issue == (stream == 1 ? replay::ReplayPlaybackIssue::TouchTime
                                                : replay::ReplayPlaybackIssue::LaneCoverTime),
                "late live auxiliary evidence fails canonical playback validation");
      }
    }
  }
}

void testAuthoredCourseStageLiveCarry() {
  for (const int authoredMode : {2, 3}) {
    for (const std::size_t affectedStage : {std::size_t{0}, std::size_t{1}}) {
      for (const int replayFailure : {0, 1, 2}) {
        auto session = std::make_shared<CoursePlaySession>();
        session->entries.resize(3);
        session->longNoteMode = 1;
        for (std::size_t stageIndex = 0; stageIndex <= affectedStage; ++stageIndex) {
          GamePlayScene scene;
          scene.options.courseSession = session;
          scene.options.longNoteMode = 1;
          scene.options.gaugeType = GaugeType::Hard;
          scene.chart->Meta.LnMode = stageIndex == affectedStage ? authoredMode : 1;
          scene.state->configureGauge(GaugeType::Hard, GaugeAutoShiftMode::None);
          scene.courseStageInitialGauge = scene.state->gaugeSnapshot();
          session->currentIndex = stageIndex;
          session->entries[stageIndex].meta = scene.chart->Meta;
          configureAbortCapture(scene);
          scene.state->commitJudge(JudgeResult(PGreat, 0));
          scene.state->commitJudge(JudgeResult(PGreat, 0));
          auto liveGauge = scene.state->gaugeSnapshot();
          liveGauge.currentGauge = stageIndex == affectedStage ? 60.0F : 74.0F;
          scene.state->restoreGaugeState(liveGauge);
          auto capture = scene.completeModernReplayCapture();
          if (stageIndex == affectedStage && replayFailure == 1) {
            capture.acceptedInput.reset();
          }
          if (stageIndex == affectedStage && replayFailure == 2) {
            scene.recordedReplay.laneCoverEvents.push_back(
                {.noteStartPositionPercent = 101});
          }
          const auto completedGauge = scene.state->gaugeSnapshot();
          scene.recordModernCourseStage(capture);
          if (!session->modernCourseContinuation) {
            std::cerr << "authoredMode=" << authoredMode << " stage=" << stageIndex
                      << " replayFailure=" << replayFailure << " diagnostic="
                      << session->modernCourseDiagnostic << '\n';
          }
          require(session->modernCourseStageResults.size() == stageIndex + 1,
                  "COR03 actual scene captures contiguous authored-mode stage results");
          require(session->courseCarriedGauge() &&
                      session->courseCarriedGauge()->currentGauge == completedGauge.currentGauge &&
                      session->courseCarriedGauge()->gaugeValues == completedGauge.gaugeValues &&
                      session->courseCarriedGauge()->gaugeSurvivalFailed == completedGauge.gaugeSurvivalFailed &&
                      session->courseCarriedCombo() == 2,
                  "COR03 actual scene preserves exact live carry despite authored mode or replay failure");
          require(session->modernCourseContinuation &&
                      session->modernCourseContinuation->nextStageIndex == stageIndex + 1,
                  "COR03 replay availability cannot break valid live continuation");
          require(session->modernCourseReplayStages.back().playback.has_value() ==
                      (stageIndex != affectedStage || replayFailure == 0),
                  "COR03 failed replay capture is not retained as valid playback");
        }
      }
    }
  }
}

void testCourseAbort() {
  for (const bool modern : {false, true}) {
    for (const bool midway : {false, true}) {
      GamePlayScene scene;
      scene.options.courseSession = std::make_shared<CoursePlaySession>();
      auto &session = *scene.options.courseSession;
      session.entries.resize(2);
      for (auto &entry : session.entries) entry.meta = scene.chart->Meta;
      scene.options.gaugeType = GaugeType::Hard;
      scene.state->configureGauge(GaugeType::Hard, GaugeAutoShiftMode::None);
      configureAbortCapture(scene);
      scene.courseStageInitialGauge = scene.state->gaugeSnapshot();
      session.carriedGauge = scene.state->gaugeSnapshot();
      if (modern) {
        const auto started = replay::startCourseContinuation(
            {.totalStages = 2, .initialGauge = *scene.courseStageInitialGauge,
             .constraints = {.longNoteMode = 1}});
        require(started.ready(), "modern course continuation begins");
        session.adoptModernCourseContinuation(*started.state);
      }
      if (midway) {
        scene.chart->Measures.front()->TimeLines.front()->Notes[0]->Play(2'000'000);
        scene.state->commitJudge(JudgeResult(PGreat, 0));
        recordAcceptedHit(scene);
      }
      scene.context.jukebox.time = midway ? 2'100'000 : 1'000'000;
      scene.abortPlayFromStartSelectControl();
      require(session.courseCarriedGauge() && session.courseCarriedGauge()->currentGauge == 0,
              "COR02: aborted stage cannot retain previous positive course carry");
      require(session.completedResults.size() == 1 &&
                  session.completedResults.front().state.getClearTypeRank() == kClearTypeFailedRank,
              "COR02: first course result capture retains failure");
      if (modern) {
        scene.recordModernCourseStage(scene.completeModernReplayCapture());
        require(session.modernCourseStageResults.size() == 1 &&
                    session.modernCourseStageResults.front().score.clearType == kClearTypeFailedRank &&
                    session.modernCourseContinuation &&
                    session.modernCourseContinuation->gauge.currentGauge == 0,
                "COR02: actual later modern stage capture preserves terminal failure");
      }
      session.recordResult(scene.chart->Meta, *scene.state);
      ResultScene result;
      result.local.courseOptions.session = scene.options.courseSession;
      result.local.resultState = *scene.state;
      result.continueCourse();
      result.continueCourse();
      require(result.summaries == 1 && session.currentIndex == 0 &&
                  session.completedResults.size() == 1,
              "COR02: actual result continuation cannot skip the unfinished stage");
    }
  }
}

void testPracticeTerminalExceptions() {
  for (const bool loop : {false, true}) {
    GamePlayScene abandoned;
    practice::Configuration configuration;
    configuration.endMicros = 10'000'000;
    configuration.loop = loop;
    abandoned.options.practiceSession = std::make_shared<practice::Session>(configuration);
    abandoned.options.practiceSession->beginAttempt();
    abandoned.abortPlayFromStartSelectControl();
    abandoned.abortPlayFromStartSelectControl();
    require(abandoned.options.practiceSession->abandonedAttemptCount() == 1 &&
                abandoned.options.practiceSession->completedAttempts().empty() &&
                abandoned.state->judgeCount[Poor] == 0 && abandoned.transitions == 1,
            "session practice abort still abandons without completing or penalizing the range");
    GamePlayScene completed;
    completed.options.practiceSession = std::make_shared<practice::Session>(configuration);
    completed.options.practiceSession->beginAttempt();
    completed.sourcePlaytime = 1'500;
    completed.context.jukebox.time = 1'000'000;
    completed.update(0.016F);
    require(completed.options.practiceSession->completedAttempts().empty(),
            "source playtime does not terminate practice before its deadline");
    completed.context.jukebox.time = 1'501'000;
    completed.update(0.016F);
    require(completed.options.practiceSession->completedAttempts().size() == 1 &&
                completed.transitions == (loop ? 0 : 1),
            "source terminal condition completes practice before the configured end");
  }
}

void testRealtimeTerminalRouting() {
  for (const bool practice : {false, true}) {
    GamePlayScene scene;
    scene.realtimeGameplaySession = std::make_unique<FixtureRealtimeSession>();
    scene.realtimeGameplaySession->worker = std::make_unique<FixtureWorker>();
    scene.realtimeGameplaySession->worker->snapshot.terminalReason =
        gameplay::GameplayTerminalReason::Aborted;
    if (practice) {
      practice::Configuration configuration;
      configuration.endMicros = 4'000'000;
      scene.options.practiceSession = std::make_shared<practice::Session>(configuration);
      scene.options.practiceSession->beginAttempt();
    }
    scene.stoppedSnapshot = std::make_unique<RhythmState>(*scene.state);
    scene.context.jukebox.time = 500'000;
    scene.update(0.016F);
    require(scene.transitions == 1 && scene.state->isEnding &&
                !scene.realtimeGameplaySession,
            "explicit realtime abort routes to one result rather than an integrity error");
    require(scene.state->judgeCount[Poor] == (practice ? 0 : 2),
            "realtime abort preserves practice abandonment versus ordinary remaining notes");
  }
  for (const bool loop : {false, true}) {
    GamePlayScene scene;
    practice::Configuration configuration;
    configuration.endMicros = 4'000'000;
    configuration.loop = loop;
    scene.options.practiceSession = std::make_shared<practice::Session>(configuration);
    scene.options.practiceSession->beginAttempt();
    scene.realtimeGameplaySession = std::make_unique<FixtureRealtimeSession>();
    scene.realtimeGameplaySession->worker = std::make_unique<FixtureWorker>();
    scene.realtimeGameplaySession->worker->snapshot.terminalReason =
        gameplay::GameplayTerminalReason::PracticeComplete;
    scene.stoppedSnapshot = std::make_unique<RhythmState>(*scene.state);
    scene.stoppedSnapshot->commitJudge(JudgeResult(Poor, 1'999'999));
    scene.stoppedSnapshot->commitJudge(JudgeResult(Poor, 999'999));
    scene.context.jukebox.time = 4'000'000;
    scene.update(0.016F);
    require(scene.options.practiceSession->completedAttempts().size() == 1 &&
                scene.recordedReplay.events.empty() && scene.resets == (loop ? 1 : 0) &&
                scene.transitions == (loop ? 0 : 1),
            "realtime finalized practice must not finalize its pending chart a second time");
  }
}

void testLongNoteAbortAccounting() {
  for (const bool held : {false, true}) {
    GamePlayScene scene;
    for (auto *measure : scene.chart->Measures) delete measure;
    scene.chart->Measures.clear();
    scene.chart->Meta.TotalNotes = 6;
    auto *measure = new bms_parser::Measure();
    auto *heads = new bms_parser::TimeLine(8, false);
    auto *tails = new bms_parser::TimeLine(8, false);
    auto *normal = new bms_parser::TimeLine(8, false);
    heads->Timing = 1'000'000;
    tails->Timing = 3'000'000;
    normal->Timing = 4'000'000;
    int lane = 0;
    for (const auto type : {bms_parser::LongNoteType::LongNote,
                            bms_parser::LongNoteType::ChargeNote,
                            bms_parser::LongNoteType::HellChargeNote}) {
      auto *head = new bms_parser::LongNote(1, type);
      auto *tail = new bms_parser::LongNote(1, type);
      head->Tail = tail;
      tail->Head = head;
      heads->SetNote(lane, head);
      tails->SetNote(lane, tail);
      ++lane;
    }
    normal->SetNote(3, new bms_parser::Note(1));
    measure->TimeLines = {heads, tails, normal};
    scene.chart->Measures.push_back(measure);
    scene.options.ruleset = GameplayRuleset::Beatoraja;
    scene.options.longNoteMode = 1;
    scene.options.gaugeType = GaugeType::Hard;
    scene.state = std::make_unique<RhythmState>(scene.chart, false);
    scene.state->isPlaying = true;
    scene.state->configureGauge(GaugeType::Hard, GaugeAutoShiftMode::None);
    const auto policy = buildGameplayRulesetPolicyAtPlayStart(
        scene.options, scene.chart->Meta, AppSettings::NotePriorityMode::Lowest);
    require(policy.built(), policy.diagnostic);
    const auto definition = gameplay::buildGameplayDefinition(*scene.chart, 1);
    gameplay::GameplaySimulation simulation(definition,
        {.judge = policy.policy->judge, .gaugeRules = policy.policy->gauge,
         .attempt = {.initialGaugeType = GaugeType::Hard,
                     .replayCapacity = 64, .automaticResultCapacity = 64,
                     .gaugeHistoryCapacity = 64}});
    if (held) {
      for (int laneIndex = 0; laneIndex < 3; ++laneIndex) {
        auto *head = static_cast<bms_parser::LongNote *>(heads->Notes[laneIndex]);
        head->Press(1'000'000);
        if (laneIndex != 0) scene.state->commitJudge(JudgeResult(PGreat, 0));
        simulation.applyPressAt(laneIndex, laneIndex,
            {.songTimeMicros = 1'000'000, .laneBeamTimeMicros = 1'000'000});
      }
    }
    scene.context.jukebox.time = held ? 1'100'000 : 500'000;
    scene.abortPlayFromStartSelectControl();
    simulation.finalizeAbortedAttempt(scene.context.jukebox.time);
    require(scene.state->judgeCount[Poor] == (held ? 4 : 6) &&
                scene.state->judgeCount[PGreat] == (held ? 2 : 0) &&
                simulation.scoreState().judgeCount.at(Poor) == (held ? 4 : 6) &&
                simulation.scoreState().judgeCount.at(PGreat) == (held ? 2 : 0),
            "classic/CN/HCN abort accounts heads and held tails without duplicate judgments");
    const auto eventCount = simulation.replayEvents().size();
    simulation.finalizeAbortedAttempt(scene.context.jukebox.time + 1);
    require(simulation.replayEvents().size() == eventCount &&
                simulation.finalSummary().clearTypeRank == kClearTypeFailedRank,
            "simulation abort is terminal and idempotent");
  }
}

struct TerminalWorkerClock {
  std::atomic<std::int64_t> songTimeMicros{0};
  static std::optional<std::int64_t> map(void *, std::int64_t steadyMicros) {
    return steadyMicros;
  }
  static std::optional<std::int64_t> now(void *context) {
    return static_cast<TerminalWorkerClock *>(context)->songTimeMicros.load();
  }
};

template <typename Predicate> void requireWorkerState(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!predicate() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  require(predicate(), "bounded real worker reaches the requested state");
}

void testStoppedWorkerAbortWatch(bool pastChartEnd = false) {
  const long long abortTimeMicros = pastChartEnd ? 4'600'000 : 2'600'000;
  TerminalWorkerClock clock;
  InputDeviceRegistry registry(std::vector<InputDeviceRegistry::BackendFactory>{});
  std::vector<std::string> teardown;
  GamePlayScene scene;
  scene.options.gaugeType = GaugeType::Hazard;
  scene.state->configureGauge(GaugeType::Hazard, GaugeAutoShiftMode::None);
  configureAbortCapture(scene);
  const auto policy = buildGameplayRulesetPolicyAtPlayStart(
      scene.options, scene.chart->Meta, AppSettings::NotePriorityMode::Lowest);
  require(policy.built(), policy.diagnostic);
  scene.realtimeGameplaySession = std::make_unique<FixtureRealtimeSession>();
  auto &session = *scene.realtimeGameplaySession;
  session.notes = buildRealtimeGameplayNoteLookup(*scene.chart);
  for (std::size_t index = 0; index < session.notes.size(); ++index) {
    scene.skinGameplayGraphSourceIds.emplace(session.notes[index], index + 1);
  }
  session.worker = std::make_unique<FixtureWorker>();
  session.worker->native = std::make_unique<gameplay::RealtimeGameplayWorker>(
      gameplay::buildGameplayDefinition(*scene.chart, 1),
      gameplay::RealtimeGameplayWorkerConfig{
          .epoch = 17,
          .simulation = {.judge = policy.policy->judge, .gaugeRules = policy.policy->gauge,
                         .attempt = {.initialGaugeType = GaugeType::Hazard,
                                     .replayCapacity = 128, .automaticResultCapacity = 128,
                                     .gaugeHistoryCapacity = 128}},
          .clock = {.context = &clock, .mapSteadyToSong = &TerminalWorkerClock::map,
                    .currentSongTime = &TerminalWorkerClock::now},
          .inputTriggeredKeysounds = false});
  auto &worker = *session.worker->native;
  require(worker.start(), "Hazard terminal worker starts");
  scene.onIngressClosed = [&] { teardown.emplace_back("ingress closed"); };
  scene.onTouchDrain = [&] {
    if (!teardown.empty()) teardown.emplace_back("touches drained");
  };
  gameplay::RealtimeGameplayInputRegistration::Configuration registration;
  registration.claimedClasses[static_cast<std::size_t>(input::DeviceClass::Keyboard)] = true;
  registration.setLegacyClassEnabled = [&](auto, bool enabled) {
    if (enabled) teardown.emplace_back("native detached");
  };
  session.inputRegistration = std::make_unique<gameplay::RealtimeGameplayInputRegistration>(
      registry, session.acceptingNativeInput, std::move(registration));
  require(session.inputRegistration->activate(), "terminal fixture native input activates");
  session.worker->beforeStop = [&] {
    require(teardown == std::vector<std::string>{"ingress closed", "native detached", "touches drained"},
            "production shutdown closes ingress and native registration before draining and stopping");
    require(!session.acceptingNativeInput, "native acceptance is closed before the worker stops");
  };
  const replay::LogicalControl control{
      .kind = replay::LogicalControlKind::Lane, .player = 1, .lane = 1};
  for (const bool pressed : {true, false}) {
    require(worker.enqueueInput(
                {.epoch = 17,
                 .type = pressed ? gameplay::RealtimeGameplayInputType::Press
                                 : gameplay::RealtimeGameplayInputType::Release,
                 .lane = 1, .compensateLane = 1,
                 .steadyTimestampMicros = pressed ? 500'000 : 510'000,
                 .hasReplayControl = true, .replayControl = control}),
            "unused-lane transitions enter real worker ingress");
  }
  clock.songTimeMicros.store(1'000'000);
  requireWorkerState([&] {
    auto snapshot = worker.acquireLatestSnapshot();
    return snapshot && snapshot->transactionSequence >= 2;
  });
  clock.songTimeMicros.store(2'500'000);
  requireWorkerState([&] {
    auto snapshot = worker.acquireLatestSnapshot();
    return snapshot && snapshot->terminalReason ==
                           gameplay::GameplayTerminalReason::SurvivalGaugeFailed;
  });
  {
    auto snapshot = worker.acquireLatestSnapshot();
    require(snapshot->attempt.judgeCounts[Poor] == 1 &&
                snapshot->noteStates[0].played && !snapshot->noteStates[1].played,
            "real stopped-worker boundary has one missed note and one pending note");
  }
  require(!session.notes[0]->IsPlayed && scene.state->judgeCount[Poor] == 0,
          "render-side note and score facts have not been synchronized prematurely");
  scene.context.jukebox.time = abortTimeMicros;
  scene.startSelectControl.emplace(gameplay::StartSelectControl::Configuration{});
  auto &queue = session.startSelectInputs;
  require(queue.tryPush({.control = {.kind = replay::LogicalControlKind::Start},
                         .pressed = true, .timestampMicros = 10'000'000}), "queue Start");
  require(queue.tryPush({.control = {.kind = replay::LogicalControlKind::Select},
                         .pressed = true, .timestampMicros = 10'000'100}), "queue Select");
  require(queue.tryPush({.control = control, .pressed = true,
                         .timestampMicros = 11'000'101}), "queue exit trigger");
  scene.clock = 11'000'102;
  scene.update(0.016F);
  require(!scene.realtimeGameplaySession && scene.transitions == 1 &&
              scene.state->judgeCount[Poor] == 2,
          "actual stopped-worker sync plus abort judges only the remaining note");
  require(scene.playfieldVisualStateStore->noteStates.size() == 2 &&
              scene.playfieldVisualStateStore->noteStates.at(1).judged &&
              scene.playfieldVisualStateStore->noteStates.at(1).playedTimeMicros < abortTimeMicros,
          "actual worker snapshot synchronizes the visual note DTO and source timestamp");
  const std::vector<replay::InputTransition> accepted{
      {.songTimeMicros = 500'000, .control = control, .pressed = true},
      {.songTimeMicros = 510'000, .control = control, .pressed = false}};
  require(scene.completedModernReplayInput == accepted && !scene.modernReplayInputRecorder,
          "actual shutdown transfers the worker's accepted raw input without reconstruction");
  require(scene.chart->Measures.front()->TimeLines.front()->Notes[0]->PlayedTime < abortTimeMicros &&
              scene.chart->Measures.front()->TimeLines.back()->Notes[0]->PlayedTime == abortTimeMicros,
          "final sync retains worker note-runtime time and finalizes only the pending note");
  const auto replay = testDurableAbort(scene, false);
  for (const bool splitFrames : {false, true}) {
    GamePlayScene watch;
    watch.state->configureGauge(GaugeType::Hazard, GaugeAutoShiftMode::None);
    watch.options.replayData = replay;
    watch.buildReplayNoteLookup();
    if (pastChartEnd) watch.sourcePlaytime = 3'000;
    if (splitFrames) {
      watch.context.jukebox.time = abortTimeMicros - 50'000;
      if (pastChartEnd) watch.update(0.016F);
      else watch.processReplayEvents(watch.context.jukebox.time);
      require(!watch.state->isEnding && watch.transitions == 0 &&
                  watch.state->judgeCount[Poor] == 1,
              "T2-R3: survival callback and chart deadline wait for the authoritative abort boundary");
    }
    watch.context.jukebox.time = abortTimeMicros + 100'000;
    watch.update(0.016F);
    watch.processReplayEvents(watch.context.jukebox.time + 1);
    require(watch.state->isEnding && watch.transitions == 1 &&
                watch.state->judgeCount[Poor] == 2 &&
                watch.state->getClearTypeRank() == kClearTypeFailedRank &&
                watch.chart->Measures.front()->TimeLines.back()->Notes[0]->PlayedTime == abortTimeMicros,
            "T2-R3: Watch crossing event and abort times accounts two judgments at the authoritative time once");
  }
  GamePlayScene ordinary;
  ordinary.state->configureGauge(GaugeType::Hazard, GaugeAutoShiftMode::None);
  ordinary.options.replayData = std::make_shared<ReplayData>(*replay);
  ordinary.options.replayData->abortedAtSongTimeMicros.reset();
  ordinary.buildReplayNoteLookup();
  ordinary.context.jukebox.time = 2'550'000;
  ordinary.processReplayEvents(ordinary.context.jukebox.time);
  require(ordinary.state->isEnding && ordinary.transitions == 1 &&
              ordinary.state->judgeCount[Poor] == 1,
          "T2-R3: ordinary non-abort survival playback still terminates on its first failure");
}

void testNativeFailurePausesBeforeHeldReleaseAndWaitsForExplicitResume() {
  TerminalWorkerClock clock;
  GamePlayScene scene;
  scene.context.jukebox.time = 2'050'000;
  scene.clock = 2'050'000;
  scene.realtimeGameplaySession = std::make_unique<FixtureRealtimeSession>();
  auto &session = *scene.realtimeGameplaySession;
  session.audio = &scene.context.jukebox;
  session.notes = buildRealtimeGameplayNoteLookup(*scene.chart);
  const auto policy = buildGameplayRulesetPolicyAtPlayStart(
      scene.options, scene.chart->Meta, AppSettings::NotePriorityMode::Lowest);
  require(policy.built(), policy.diagnostic);
  session.worker = std::make_unique<FixtureWorker>();
  session.worker->native = std::make_unique<gameplay::RealtimeGameplayWorker>(
      gameplay::buildGameplayDefinition(*scene.chart, 1),
      gameplay::RealtimeGameplayWorkerConfig{
          .epoch = 17,
          .simulation = {.judge = policy.policy->judge, .gaugeRules = policy.policy->gauge,
                         .attempt = {.replayCapacity = 128, .automaticResultCapacity = 128,
                                     .gaugeHistoryCapacity = 128}},
          .clock = {.context = &clock, .mapSteadyToSong = &TerminalWorkerClock::map,
                    .currentSongTime = &TerminalWorkerClock::now},
          .inputTriggeredKeysounds = false});
  auto &worker = *session.worker->native;
  InputProfile profile;
  profile.bindings.push_back({
      .id = "interrupted-key", .scope = {.player = 1, .keyMode = 7},
      .action = {.kind = input::LogicalActionKind::Lane, .lane = 0},
      .control = {.deviceId = "keyboard", .deviceClass = input::DeviceClass::Keyboard,
                  .kind = input::ControlKind::Key, .index = SDL_SCANCODE_S}});
  session.physicalInputRouter = std::make_unique<input::RealtimePhysicalInputRouter>(
      profile, makeGameplayInputScopes(7), [&](const auto &transition) {
        return worker.enqueueInput({.epoch = 17,
            .type = transition.type == input::RealtimePhysicalInputTransitionType::Press
                ? gameplay::RealtimeGameplayInputType::Press : gameplay::RealtimeGameplayInputType::Release,
            .source = gameplay::RealtimeGameplayInputSource::Physical,
            .lane = transition.lane, .steadyTimestampMicros = transition.steadyTimestampMicros,
            .hasReplayControl = transition.hasReplayControl,
            .replayControl = transition.replayControl});
      });
  require(worker.start(), "interruption worker starts");
  scene.setRealtimeGameplayIngressEnabled(true);
  input::PhysicalInputEvent key{
      .control = {.deviceId = "keyboard", .deviceClass = input::DeviceClass::Keyboard,
                  .kind = input::ControlKind::Key, .index = SDL_SCANCODE_S},
      .rawValue = 1.0, .normalizedValue = 1.0f};
  session.physicalInputRouter->consume(key, 2'000'000);
  requireWorkerState([&] { return worker.acquireLatestSnapshot()->attempt.judgeCounts[PGreat] == 1; });
  const auto accepted = worker.acquireLatestSnapshot()->transactionSequence;
  session.interruptInput({input::DeviceClass::Keyboard, 2'050'000, false});
  require(scene.context.jukebox.isPaused(), "actual interruption handler freezes audio immediately");
  key.rawValue = key.normalizedValue = 0;
  session.physicalInputRouter->consume(key, 2'050'000);
  scene.closePauseMenu();
  require(scene.context.jukebox.isPaused(), "resume cannot race an incomplete fallback handoff");
  session.interruptInput({input::DeviceClass::Keyboard, 2'050'000, true});
  clock.songTimeMicros = 10'000'000;
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  // A fresh fallback press/release while the main thread is stalled is tracked
  // by the real router but cannot enter scoring or become a delayed new hit.
  key.rawValue = key.normalizedValue = 1;
  session.physicalInputRouter->consume(key, 2'060'000);
  key.rawValue = key.normalizedValue = 0;
  session.physicalInputRouter->consume(key, 2'070'000);
  scene.update(0);
  require(scene.inputInterruptionPause && scene.context.jukebox.isPaused() &&
              scene.context.inputDeviceRegistry.fallbackCompletions == 1,
          "main recovery completes fallback once and leaves the same attempt paused");
  const auto paused = worker.acquireLatestSnapshot();
  require(paused->transactionSequence == accepted && paused->attempt.judgeCounts[PGreat] == 1 &&
              paused->attempt.judgeCounts[Poor] == 0 && !paused->noteStates.back().played,
          "native release and fresh paused fallback input do not judge or lose an existing score");
  clock.songTimeMicros = 2'050'000;
  scene.closePauseMenu();
  require(!scene.context.jukebox.isPaused() && !session.inputInterrupted,
          "explicit resume reopens gameplay without replacing the attempt");
  requireWorkerState([&] { return worker.acquireLatestSnapshot()->transactionSequence > accepted; });
  require(worker.acquireLatestSnapshot()->attempt.judgeCounts[PGreat] == 1 &&
              worker.fault() == gameplay::RealtimeGameplayFault::None,
          "held release reconciliation keeps the score and authority valid");
  worker.stop();
}

void testNativeFailureRacingOrdinaryPauseResumeIsNotCleared() {
  GamePlayScene scene;
  scene.realtimeGameplaySession = std::make_unique<FixtureRealtimeSession>();
  auto &session = *scene.realtimeGameplaySession;
  session.audio = &scene.context.jukebox;
  session.worker = std::make_unique<FixtureWorker>();
  scene.context.jukebox.paused = true;
  std::atomic_bool failureStarted{false};
  std::thread native;
  scene.context.jukebox.beforeResume = [&] {
    native = std::thread([&] {
      failureStarted.store(true, std::memory_order_release);
      session.interruptInput({input::DeviceClass::Keyboard, 1'000'000, false});
      session.interruptInput({input::DeviceClass::Keyboard, 1'000'000, true});
    });
    while (!failureStarted.load(std::memory_order_acquire)) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  };
  scene.closePauseMenu();
  native.join();
  scene.context.jukebox.beforeResume = {};
  require(session.inputInterrupted && session.inputFallbackReady &&
              scene.context.jukebox.isPaused(),
          "failure concurrent with normal resume retains its pause and ready notification");
  require(scene.drainRealtimeInputInterruption() && scene.inputInterruptionPause,
          "the raced interruption still reaches the main-thread pause UI");
  scene.closePauseMenu();
  require(!session.inputInterrupted && !scene.context.jukebox.isPaused(),
          "only a later explicit resume clears the recovered interruption");
}

#include "course_preparation_scene_fixture.h"
#include "chart_preparation_scene_fixture.h"

PREPARATION_IMPLEMENTATIONS
#include "dp_flip_preparation_fixture.h"

FLIP_IMPLEMENTATIONS

void queueReminderSwipe() {
  for (auto phase : {IOSRawTouchPhaseBegan, IOSRawTouchPhaseMoved, IOSRawTouchPhaseEnded}) {
    for (int i = 0; i < 4; ++i) {
      reminderTouches.push_back({.fingerId = i, .normalizedX = .2F + .1F * i,
          .normalizedY = phase == IOSRawTouchPhaseBegan ? .8F : .765F, .phase = phase});
    }
  }
}

void testReminderSceneStartup(std::string_view scenario) {
  GamePlayScene scene;
  scene.queueDeferred = true;
  scene.ipadGestureReminderPending = true;
  scene.state->isPlaying = false;
  reminderTouches.clear();
  queueReminderSwipe();
  scene.update(0);
  require(scene.attemptStarts == 0 && !scene.state->isPlaying &&
              scene.recordedReplay.events.empty() && !scene.modernReplayInputRecorder,
          "reminder swipe must leave gameplay and replay capture stopped");
  scene.finishFrame();
  if (scenario == "back") {
    scene.returnFromIpadGestureReminder();
    scene.update(0);
    scene.finishFrame();
    require(scene.attemptStarts == 0 && !scene.state->isPlaying,
            "Back after a completed swipe must cancel startup immediately");
    scene.finishFrame();
    require(scene.context.sceneManager->returns == 1, "Back must return once");
  } else if (scenario == "native-background") {
    scene.update(0);
    // UIKit runs after update, before deferred callbacks; SDL dispatch is next frame.
    scene.nativeBackground = true;
    scene.finishFrame();
    require(!scene.startedWhileNativeBackground,
            "startup must not run from deferred callbacks after native background arrival");
    scene.onApplicationBackgroundChanged(true);
  } else if (scenario == "native-cancel" || scenario == "cancel-then-swipe") {
    reminderTouches.push_back({.phase = IOSRawTouchPhaseCancelled});
    if (scenario == "cancel-then-swipe") queueReminderSwipe();
    scene.update(0);
    scene.finishFrame();
    require(scene.attemptStarts == 0 && scene.ipadGestureReminderPending,
            "native cancellation must invalidate readiness even before a fresh swipe in the same batch");
    if (scenario == "cancel-then-swipe") {
      scene.update(0);
      require(scene.attemptStarts == 1, "fresh swipe starts after its own event-dispatch boundary");
    }
  } else if (scenario == "cancel") {
    scene.onApplicationBackgroundChanged(true);
    scene.finishFrame();
    scene.onApplicationBackgroundChanged(false);
    scene.update(0);
    scene.finishFrame();
    require(scene.attemptStarts == 0 && scene.ipadGestureReminderPending,
            "background interruption must require a fresh swipe after foreground");
  } else {
    scene.update(0);
    require(scene.attemptStarts == 1 && scene.state->isPlaying &&
                !scene.ipadGestureReminderPending && !scene.reminderLayout.visible,
            "a successful swipe must start at the next update after event dispatch");
    scene.finishFrame();
    require(scene.attemptStarts == 1, "deferred callbacks must not repeat startup");
  }
}

int main(int argc, char **argv) {
  if (argc > 2 && std::string_view(argv[1]) == "ipad-reminder") {
    testReminderSceneStartup(argv[2]);
    return 0;
  }
  for (const auto scenario : {"back", "native-background", "cancel", "native-cancel", "cancel-then-swipe", "success"}) {
    testReminderSceneStartup(scenario);
  }
  testNativeFailureRacingOrdinaryPauseResumeIsNotCleared();
  testNativeFailurePausesBeforeHeldReleaseAndWaitsForExplicitResume();
  {
    GamePlayScene scene;
    scene.options.courseSession = std::make_shared<CoursePlaySession>();
    scene.context.jukebox.time = 2'500'000;
    scene.inputInterruptionPause = true;
    scene.showPauseMenu(true);
    require(scene.attemptProvenance.eligibility == ScoreEligibility::Modified &&
                scene.state->lightAssistClearMark &&
                scene.recordedReplay.provenance == scene.attemptProvenance,
            "a forced mid-stage course pause records the same assisted provenance as a real playback pause");
    scene.closePauseMenu();
    require(!scene.context.jukebox.isPaused(),
            "an explicit resume also resumes a course paused by native input failure");
  }
  if (argc > 1 && std::string_view(argv[1]) == "pause-penalty") {
    testPausePenaltyAndFreshAttemptBoundary();
    testPauseAfterEarlyJudgmentDisqualifiesIr();
    testPauseOnlyPenalizesUnfinishedNotePlay();
    testPausePenaltyIncludesRemainingMinesAndLongNoteGaugeEffects();
    return 0;
  }
  if (argc > 1 && std::string_view(argv[1]) == "partial-course-retry-same") {
    testPartialCourseRetrySameRestoresSavedOptions(argc > 2 ? argv[2] : "all");
    return 0;
  }
  if (argc > 2 && std::string_view(argv[1]) == "dp-flip") {
    testActualDoublePlayFlipPreparation(argv[2]);
    return 0;
  }
  if (argc > 2 && std::string_view(argv[1]) == "chart-preparation") {
    testActualChartPreparationOrdering(argv[2]);
    return 0;
  }
  if (argc > 1 && std::string_view(argv[1]) == "course-prepared-facts") {
    testEffectiveCourseFactsPersistThroughResultScene();
    return 0;
  }
  if (argc > 1 && std::string_view(argv[1]) == "authored-course-carry") {
    testAuthoredCourseStageLiveCarry();
    std::cout << "COR03 actual scene carry tests passed\n";
    return 0;
  }
  if (argc > 1 && std::string_view(argv[1]) == "worker-abort") {
    testStoppedWorkerAbortWatch();
    testStoppedWorkerAbortWatch(true);
    return 0;
  }
  if (argc > 1 && std::string_view(argv[1]) == "signed-abort") {
    testSignedAbortCaptureAndWatch();
    testAbortCaptureRejectsLateEvidence();
    return 0;
  }
  if (argc > 1 && std::string_view(argv[1]) == "export-abort") {
    GamePlayScene scene;
    scene.state->configureGauge(GaugeType::Hard, GaugeAutoShiftMode::None);
    scene.options.gaugeType = GaugeType::Hard;
    configureAbortCapture(scene);
    scene.context.jukebox.time = 1'000'000;
    scene.abortPlayFromStartSelectControl();
    testDurableAbort(scene, false);
    return 0;
  }
  if (argc > 1 && std::string_view(argv[1]) == "abort-policy") {
    require(gameplay::classifyRealtimeGameplayTerminal(
                gameplay::GameplayTerminalReason::Aborted, false, false) !=
                gameplay::RealtimeGameplayTerminalAction::IntegrityFailure,
            "COR02: an explicit abort is a failed exit, not corrupt input");
    return 0;
  }
  if (argc > 1 && std::string_view(argv[1]) == "cor02") {
    testAbortOutcome();
    std::cout << "COR02 actual scene abort matrix passed\n";
    return 0;
  }
  if (argc > 1 && std::string_view(argv[1]) == "course") {
    testCourseAbort();
    std::cout << "COR02 actual course terminal tests passed\n";
    return 0;
  }
  if (argc > 1 && std::string_view(argv[1]) == "game01") {
    testQueuedAbortLifetime();
    std::cout << "GAME01 actual scene queued-input lifetime tests passed\n";
    return 0;
  }
  for (const bool loop : {false, true}) {
    for (const bool chartTerminal : {false, true}) {
      for (const long long offset : {-100'000LL, 0LL, 100'000LL}) {
        testPractice(loop, chartTerminal, offset);
      }
    }
  }
  std::cout << "COR01 actual scene practice tests passed\n";
  testQueuedAbortLifetime();
  testPausePenaltyAndFreshAttemptBoundary();
  testPauseOnlyPenalizesUnfinishedNotePlay();
  std::cout << "GAME01 actual scene queued-input lifetime tests passed\n";
  testAbortOutcome();
  testAuthoredCourseStageLiveCarry();
  testSignedAbortCaptureAndWatch();
  testAbortCaptureRejectsLateEvidence();
  testEffectiveCourseFactsPersistThroughResultScene();
  testPartialCourseRetrySameRestoresSavedOptions();
  testPausePenaltyAndFreshAttemptBoundary();
  testPauseAfterEarlyJudgmentDisqualifiesIr();
  testPauseOnlyPenalizesUnfinishedNotePlay();
  testPausePenaltyIncludesRemainingMinesAndLongNoteGaugeEffects();
  for (const auto path : {"constructors", "retry", "practice", "skin-practice", "viewer", "in-game-retry"}) {
    testActualChartPreparationOrdering(path);
  }
  for (const auto path : {"selected", "preloaded", "course", "course-next", "course-in-game",
                          "course-menu", "result-retry", "replay", "retry", "practice", "fallback"}) {
    testActualDoublePlayFlipPreparation(path);
  }
  testCourseAbort();
  testPracticeTerminalExceptions();
  testLongNoteAbortAccounting();
  testRealtimeTerminalRouting();
  testStoppedWorkerAbortWatch();
  testStoppedWorkerAbortWatch(true);
  std::cout << "COR02 actual scene, durable replay/cache/recall, and course tests passed\n";
}
