#include "scene/play/GamePlayStartOptions.h"
#include "scene/play/GamePlayTiming.h"
#include "scene/play/PracticeNoteFinalizer.h"
#include "scene/play/RealtimeGameplayAuthorityPolicy.h"
#include "scene/play/RealtimeGameplayWorker.h"
#include "scene/play/PlayfieldPresentationEvents.h"
#include "scene/play/GameplayNoteJudgeRole.h"
#include "scene/play/StartSelectControl.h"
#include "replay/ReplayInputRecorder.h"
#include "practice/PracticeResultFlow.h"
#include "Uuid.h"
#include "CourseConstraintUtils.h"
#include "replay/ReplaySetupProvenance.h"
#include "replay/CourseReplayConsumer.h"
#include "skin/beatoraja/GameplaySkinEndAnimation.h"
#include <SDL2/SDL_log.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <unordered_set>

void require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}


struct FixtureJukebox {
  long long time = 0;
  long long getTimeMicros() const { return time; }
  void stop() {}
  void playKeySound(int) { require(false, "Watch must not request live keysounds"); }
};

struct FixtureInput {
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
  void stop() { native->stop(); }
  auto copyGaugeHistoryAfterStop() const { return native->copyGaugeHistoryAfterStop(); }
  auto copyGaugeHistoriesAfterStop() const { return native->copyGaugeHistoriesAfterStop(); }
  auto copyAcceptedReplayInputAfterStop() const { return native->copyAcceptedReplayInputAfterStop(); }
  auto copyReplayEventsAfterStop() const { return native->copyReplayEventsAfterStop(); }
};

struct FixtureRealtimeSession {
  std::mutex touchRouterMutex;
  std::unique_ptr<FixtureTouchRouter> touchRouter;
  std::atomic_bool acceptingTouch{false};
  std::atomic_bool touchRoutingRecoveryRequested{false};
  std::unique_ptr<FixtureWorker> worker;
  gameplay::BoundedMpscQueue<gameplay::StartSelectControlInput, 16> startSelectInputs;
  std::atomic_bool startSelectInputOverflow{false};
  std::vector<bms_parser::Note *> notes;
  std::uint64_t appliedSnapshotGeneration = 0;
  std::uint64_t appliedTransactionSequence = 0;
};

struct FixturePresentation {
  void onLanePressed(int, JudgeResult, long long) {}
  void onLaneReleased(int, long long) {}
  void onJudge(JudgeResult, int, int, PlayfieldJudgeEventClock, bool) {}
  void applyGameplayGraphState(const SkinGameplayDynamicGraphState &) {}
};

class GamePlayScene {
public:
  bms_parser::Chart ownedChart;
  bms_parser::Chart *chart = &ownedChart;
  std::unique_ptr<RhythmState> state;
  StartOptions options;
  struct { FixtureJukebox jukebox; } context;
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
  void drainRealtimeTouchSamples() {}
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
    options.practiceSession->beginAttempt();
  }
};

SCENE_METHODS

class ResultScene {
public:
  struct LocalSource {
    struct CourseOptions {
      std::shared_ptr<CoursePlaySession> session;
      bool savedResultBrowsing = false;
    } courseOptions;
    RhythmState resultState{nullptr, false};
    bool courseTransitionStarted = false;
  } local;
  int summaries = 0;
  LocalSource *localSource() { return &local; }
  bool isCourseStageResult() const { return true; }
  long long recordCourseStageRestTime() { return 0; }
  void showCourseResult() { ++summaries; }
  void showSavedCourseStage() { require(false, "not browsing saved stages"); }
  void startCourseReplayStage(std::shared_ptr<CoursePlaySession>) {
    require(false, "not a course replay");
  }
  void continueCourse();
};

RESULT_CONTINUE_PREFIX

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

int main(int argc, char **argv) {
  if (argc > 1 && std::string_view(argv[1]) == "worker-abort") {
    testStoppedWorkerAbortWatch();
    testStoppedWorkerAbortWatch(true);
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
  std::cout << "GAME01 actual scene queued-input lifetime tests passed\n";
  testAbortOutcome();
  testCourseAbort();
  testPracticeTerminalExceptions();
  testLongNoteAbortAccounting();
  testRealtimeTerminalRouting();
  testStoppedWorkerAbortWatch();
  testStoppedWorkerAbortWatch(true);
  std::cout << "COR02 actual scene, durable replay/cache/recall, and course tests passed\n";
}
