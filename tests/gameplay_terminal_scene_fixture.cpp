#include "scene/play/GamePlayStartOptions.h"
#include "scene/play/GamePlayTiming.h"
#include "scene/play/PracticeNoteFinalizer.h"
#include "scene/play/RealtimeGameplayAuthorityPolicy.h"
#include "scene/play/RealtimeGameplayWorker.h"
#include "scene/play/StartSelectControl.h"
#include "skin/beatoraja/GameplaySkinEndAnimation.h"
#include <SDL2/SDL_log.h>

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>

void require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

void SDL_Log(const char *, ...) {}
void SDL_LogError(int, const char *, ...) {}
void SDL_LogWarn(int, const char *, ...) {}

struct FixtureJukebox {
  long long time = 0;
  long long getTimeMicros() const { return time; }
  void stop() {}
};

struct FixtureInput {
  void pumpPendingTouchEvents() {}
};

struct FixtureTouchRouter {
  bool advanceSpinScratch(long long) { return true; }
};

struct FixtureWorker {
  struct Snapshot {
    gameplay::GameplayTerminalReason terminalReason =
        gameplay::GameplayTerminalReason::None;
  };
  gameplay::RealtimeGameplayFault fault() const {
    return gameplay::RealtimeGameplayFault::None;
  }
  std::shared_ptr<Snapshot> acquireLatestSnapshot() const {
    return std::make_shared<Snapshot>();
  }
};

struct FixtureRealtimeSession {
  std::mutex touchRouterMutex;
  std::unique_ptr<FixtureTouchRouter> touchRouter;
  std::atomic_bool acceptingTouch{false};
  std::atomic_bool touchRoutingRecoveryRequested{false};
  std::unique_ptr<FixtureWorker> worker;
  gameplay::BoundedMpscQueue<gameplay::StartSelectControlInput, 16> startSelectInputs;
  std::atomic_bool startSelectInputOverflow{false};
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
  ScoreProvenance attemptProvenance = ScoreProvenance::Legacy();
  int transitions = 0;
  int resets = 0;
  long long offset = 0;
  long long clock = 0;
  std::optional<std::int64_t> sourcePlaytime;

  GamePlayScene() {
    ownedChart.Meta.TotalNotes = 2;
    ownedChart.Meta.KeyMode = 7;
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

  bool realtimeGameplayAuthorityActive() const {
    return realtimeGameplaySession != nullptr;
  }
  void applyPendingBestReplay() {}
  void drainRealtimeInputCommands() {}
  void drainRealtimeTouchSamples() {}
  long long nowMicros() const { return clock; }
  void startPracticeAttemptFromMenu() { require(false, "unexpected practice menu"); }
  bool isReplayPlayback() const { return false; }
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
  void syncRealtimeGameplaySnapshot() { require(false, "unexpected realtime sync"); }
  void updateRealtimeVisualTimeline(long long) {}
  void processReplayEvents(long long) { require(false, "unexpected playback"); }
  void processReplayLaneCoverEvents(long long) {}
  bool preparationIndicatorActive(long long) const { return false; }
  std::optional<std::int64_t> beatorajaPlaytimeMillis(
      bms_parser::Chart *, const StartOptions &) const { return sourcePlaytime; }
  void stopRealtimeGameplayAuthority(bool) { realtimeGameplaySession.reset(); }
  void showPlaybackInitializationFailure(const char *) {
    require(false, "unexpected realtime failure");
  }
  void updateHellChargeGauge(long long) {}
  bool finishIfGaugeFailed() {
    require(!state->activeGaugeFailed(), "unexpected gauge failure");
    return false;
  }
  void checkPassedTimeline(long long time) {
    require(time < 2'000'000 || options.practiceSession != nullptr,
            "fixture timeline driver only covers the opening gap");
  }
  void publishPracticeGhost() {}
  bool isCoursePlayback() const { return options.courseSession != nullptr; }
  bool usesModernCourseContinuation() const { return false; }
  bool shouldRecordReplay() const { return true; }
  void cancelGameplaySkinPreparation() {}
  void finishPractice() {
    options.practiceSession->abandonAttempt();
    state->isEnding = true;
    context.jukebox.stop();
    scheduleResultTransition(0);
  }
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
  long long judgeEventClock(long long time) { return time; }
  void onJudge(const JudgeResult &judge, long long, bool,
               const bms_parser::Note *) {
    if (!state->isEnding) {
      state->commitJudge(judge);
    }
  }
  void appendReplayEvent(ReplayEventAction action, int lane,
                         const bms_parser::Note *note, long long songTime,
                         long long judgeTime, const JudgeResult &judge) {
    require(!state->isEnding, "miss recording precedes terminal latch");
    recordedReplay.events.push_back({.action = action,
                                    .lane = lane,
                                    .noteTimeMicros = note->Timeline->Timing,
                                    .songTimeMicros = songTime,
                                    .judgeTimeMicros = judgeTime,
                                    .judgement = judge.judgement});
  }
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

int main(int argc, char **argv) {
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
}
