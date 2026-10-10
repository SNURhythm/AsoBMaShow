#include "scene/play/RealtimeGameplayWorker.h"
#include "scene/play/RealtimeGameplayWake.h"
#include "input/SDLTouchInputSource.h"
#include "input/AndroidRawTouchInput.h"
#include "scene/play/RealtimeSdlTouchInput.h"

#include "bms_parser.hpp"
#include "input/LogicalGameplayInputAdapter.h"
#include "scene/play/RealtimeGameplayInputBridge.h"
#include "scene/play/GameplayJudgeRules.h"
#include "scene/play/Judge.h"
#include "support/AllocationFailure.h"
#include "perf/LatencyTelemetry.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <new>
#include <thread>

namespace rendering {
int render_width = 1000, render_height = 500;
int window_width = 1000, window_height = 500;
float ui_scale_x = 1, ui_scale_y = 1;
int ui_offset_x = 0, ui_offset_y = 0;
float widthScale = 1, heightScale = 1;
}

namespace {

using namespace std::chrono_literals;

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

void testWakeTimeoutPreservesConcurrentSignal() {
  struct ScriptedSemaphore {
    std::atomic_bool &pending;
    int tokens = 0;
    bool signalDuringTimeout = true;

    void signal() {
      if (!pending.exchange(true)) ++tokens;
    }
    bool try_acquire_for(std::chrono::milliseconds) {
      if (signalDuringTimeout) {
        signalDuringTimeout = false;
        // The wait has timed out, but a producer signals before it returns.
        signal();
        return false;
      }
      if (tokens == 0) return false;
      --tokens;
      return true;
    }
  };
  std::atomic_bool pending{false};
  ScriptedSemaphore wake{pending};
  gameplay::detail::waitForGameplayWake(wake, pending, 1ms);
  wake.signal();
  require(wake.tokens == 1 && pending.load(),
          "a timeout must preserve a concurrent wake and coalesce the next producer");
  gameplay::detail::waitForGameplayWake(wake, pending, 1ms);
  require(wake.tokens == 0 && !pending.load(),
          "acquiring the token acknowledges its pending signal");
  wake.signal();
  gameplay::detail::waitForGameplayWake(wake, pending, 1ms);
  require(wake.tokens == 0 && !pending.load(),
          "a later signal remains available after acknowledgement");
}

bool sameAttemptSnapshot(const gameplay::GameplayAttemptSnapshot &left,
                         const gameplay::GameplayAttemptSnapshot &right) {
  return left.judgeCounts == right.judgeCounts &&
         left.combo == right.combo && left.maxCombo == right.maxCombo &&
         left.comboBreak == right.comboBreak && left.score == right.score &&
         left.gauge == right.gauge && left.gaugeType == right.gaugeType &&
         left.clearTypeRank == right.clearTypeRank;
}

bms_parser::TimeLine *addTimeline(bms_parser::Measure &measure,
                                  long long timingMicros) {
  auto *timeline = new bms_parser::TimeLine(8, false);
  timeline->Timing = timingMicros;
  measure.TimeLines.push_back(timeline);
  return timeline;
}

gameplay::GameplayDefinition makeRapidDefinition() {
  bms_parser::Chart chart;
  chart.Meta.TotalNotes = 2;
  chart.Meta.KeyMode = 7;
  auto *measure = new bms_parser::Measure();
  addTimeline(*measure, 1'000'000)->SetNote(1, new bms_parser::Note(11));
  addTimeline(*measure, 1'100'000)->SetNote(1, new bms_parser::Note(22));
  chart.Measures.push_back(measure);
  return gameplay::buildGameplayDefinition(chart, 0);
}

gameplay::GameplayDefinition makeScratchlessDefinition(int keyMode) {
  bms_parser::Chart chart;
  chart.Meta.TotalNotes = 1;
  chart.Meta.KeyMode = keyMode;
  auto *measure = new bms_parser::Measure();
  addTimeline(*measure, 1'000'000)->SetNote(0, new bms_parser::Note(12));
  chart.Measures.push_back(measure);
  return gameplay::buildGameplayDefinition(chart, 0);
}

gameplay::GameplayDefinition makeArbitraryLaneDefinition(int keyMode,
                                                         int lane) {
  bms_parser::Chart chart;
  chart.Meta.TotalNotes = 1;
  chart.Meta.KeyMode = keyMode;
  auto *measure = new bms_parser::Measure();
  auto *timeline = new bms_parser::TimeLine(lane + 1, false);
  timeline->Timing = 1'000'000;
  timeline->SetNote(lane, new bms_parser::Note(12));
  measure->TimeLines.push_back(timeline);
  chart.Measures.push_back(measure);
  return gameplay::buildGameplayDefinition(chart, 0);
}

gameplay::GameplayDefinition makePracticeDefinition() {
  bms_parser::Chart chart;
  chart.Meta.TotalNotes = 2;
  chart.Meta.KeyMode = 7;
  auto *measure = new bms_parser::Measure();
  addTimeline(*measure, 1'000'000)->SetNote(1, new bms_parser::Note(31));
  addTimeline(*measure, 1'100'000)->SetNote(2, new bms_parser::Note(32));
  chart.Measures.push_back(measure);
  return gameplay::buildGameplayDefinition(chart, 0);
}

gameplay::GameplayDefinition makeMultiBadDefinition() {
  bms_parser::Chart chart;
  chart.Meta.TotalNotes = 3;
  chart.Meta.KeyMode = 5;
  auto *measure = new bms_parser::Measure();
  addTimeline(*measure, 800'000)->SetNote(1, new bms_parser::Note(51));
  addTimeline(*measure, 950'000)->SetNote(2, new bms_parser::Note(52));
  addTimeline(*measure, 1'150'000)->SetNote(1, new bms_parser::Note(53));
  chart.Measures.push_back(measure);
  return gameplay::buildGameplayDefinition(chart, 0);
}

gameplay::GameplayDefinition makeScratchLongDefinition() {
  bms_parser::Chart chart;
  chart.Meta.TotalNotes = 2;
  chart.Meta.KeyMode = 7;
  auto *measure = new bms_parser::Measure();
  auto *headTimeline = addTimeline(*measure, 1'000'000);
  auto *tailTimeline = addTimeline(*measure, 2'000'000);
  auto *head = new bms_parser::LongNote(
      41, bms_parser::LongNoteType::ChargeNote);
  auto *tail = new bms_parser::LongNote(
      41, bms_parser::LongNoteType::ChargeNote);
  head->Tail = tail;
  tail->Head = head;
  headTimeline->SetNote(7, head);
  tailTimeline->SetNote(7, tail);
  chart.Measures.push_back(measure);
  return gameplay::buildGameplayDefinition(chart, 0);
}

struct FakeClock {
  std::atomic<long long> nowMicros{0};

  static std::optional<std::int64_t> map(void *, std::int64_t steadyMicros) {
    return steadyMicros;
  }

  static std::optional<std::int64_t> now(void *context) {
    return static_cast<FakeClock *>(context)->nowMicros.load(
        std::memory_order_acquire);
  }
};

struct FakeAudio {
  std::atomic_bool allowReserve{true};
  std::atomic_bool allowCommit{true};
  std::atomic<int> reserveCount{0};
  std::atomic<int> commitCount{0};
  std::atomic<gameplay::NoteId> lastCommitted{gameplay::kInvalidNoteId};

  static bool reserve(void *context, gameplay::NoteId noteId,
                      gameplay::RealtimeGameplayAudioReservation &result) {
    auto &self = *static_cast<FakeAudio *>(context);
    self.reserveCount.fetch_add(1, std::memory_order_relaxed);
    if (!self.allowReserve.load(std::memory_order_acquire)) {
      return false;
    }
    result.value = noteId;
    return true;
  }

  static bool commit(void *context,
                     gameplay::RealtimeGameplayAudioReservation reservation,
                     gameplay::NoteId noteId) {
    auto &self = *static_cast<FakeAudio *>(context);
    if (!self.allowCommit.load(std::memory_order_acquire) ||
        reservation.value != noteId) {
      return false;
    }
    self.lastCommitted.store(noteId, std::memory_order_release);
    self.commitCount.fetch_add(1, std::memory_order_release);
    return true;
  }
};

gameplay::RealtimeGameplayWorkerConfig makeConfig(FakeClock &clock,
                                                   FakeAudio &audio) {
  return {
      .epoch = 7,
      .simulation = {.judge = gameplay::CompiledGameplayJudge::from(Judge(1))},
      .clock = {.context = &clock,
                .mapSteadyToSong = &FakeClock::map,
                .currentSongTime = &FakeClock::now},
      .audio = {.context = &audio,
                .reserve = &FakeAudio::reserve,
                .commit = &FakeAudio::commit},
      .inputTriggeredKeysounds = true,
  };
}

template <typename Predicate> bool waitUntil(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(1ms);
  }
  return predicate();
}

// The scene owns these boundaries in production. Ingress, immutable hit lookup,
// routing/publication and worker submission below are extracted unchanged.
struct RealtimeGameplaySession {
  struct Scene { struct Context { std::atomic_bool appInBackground{false}; } context; } owner;
  Scene *scene = &owner;
  std::atomic_bool acceptingTouch{true}, inputInterrupted{false};
  std::atomic_bool auxiliaryTouchOverflow{false}, touchRoutingRecoveryRequested{false};
  std::mutex touchRouterMutex;
  gameplay::RealtimeGameplayWorker *worker = nullptr;
  std::unique_ptr<gameplay::RealtimeTouchInputRouter> touchRouter;
  gameplay::RealtimeTouchHitSnapshotPublication touchHitSnapshots;
  gameplay::RealtimeTouchHitCaptureTracker rawHitCaptures;
  std::atomic<std::uint64_t> requestedHitCaptureReset{0};
  std::uint64_t appliedRawHitCaptureReset = 0;
  gameplay::BoundedMpscQueue<gameplay::RealtimeTouchSample, 64> auxiliaryTouches;
  std::vector<gameplay::RealtimeGameplayInput> startSelectInputs;
  void enqueueStartSelectInput(const gameplay::RealtimeGameplayInput &input) {
    startSelectInputs.push_back(input);
  }
#include "android_realtime_touch_ingress_methods.h"
};

void testAndroidTouchReachesWorkerWithoutRenderDrain() {
  for (bool deferred : {false, true}) {
    FakeClock clock;
    FakeAudio audio;
    clock.nowMicros.store(1'000'000);
    auto config = makeConfig(clock, audio);
    config.clock.mapSteadyToSong = [](void *, std::int64_t) -> std::optional<std::int64_t> {
      return 1'000'000;
    };
    gameplay::RealtimeGameplayWorker worker(makeScratchlessDefinition(4), config);
    gameplay::RealtimeTouchLayout layout{
        .revision = 1, .bottomLeft = {0, 1}, .bottomRight = {1, 1},
        .topLeft = {0, 0}, .topRight = {1, 0},
        .lanes = {0}, .scratch = {false}, .laneCount = 1, .keyMode = 4};
    RealtimeGameplaySession session;
    session.worker = &worker;
    session.touchRouter = std::make_unique<gameplay::RealtimeTouchInputRouter>(7, layout,
        gameplay::RealtimeTouchInputSink{.context = &session,
                                         .emit = &RealtimeGameplaySession::emitTouchInput});
    require(session.touchHitSnapshots.publish({.layoutRevision = 1,
        .uiTransform = {.renderWidth = 1000, .renderHeight = 500,
                        .uiScaleX = 1, .uiScaleY = 1, .uiWidth = 1000, .uiHeight = 500}}),
            "immutable input geometry publishes");
    SDLTouchInputSource source(deferred);
    source.setRawEventCallback([&](const SDL_Event &event, std::uint64_t time) {
      RealtimeGameplaySession::sdlTouchSink(session, event, time);
    });
    require(worker.start(), "SDL timing worker starts");
    SDL_Event event{};
    event.type = SDL_EVENT_FINGER_DOWN;
    event.tfinger.fingerID = 42;
    event.tfinger.x = .5F;
    event.tfinger.y = .5F;
    for (int gate = 0; gate < 3; ++gate) {
      session.acceptingTouch.store(gate != 0);
      session.owner.context.appInBackground.store(gate == 1);
      session.inputInterrupted.store(gate == 2);
      SDLTouchInputSource::EventHandler(&source, &event);
      gameplay::RealtimeTouchSample ignored;
      require(!session.auxiliaryTouches.tryPop(ignored),
              "closed, background and interrupted ingress must not publish touches");
    }
    session.acceptingTouch.store(true);
    session.owner.context.appInBackground.store(false);
    session.inputInterrupted.store(false);
    std::thread producer([&] { SDLTouchInputSource::EventHandler(&source, &event); });
    producer.join();
    const bool admitted = waitUntil([&] {
      return worker.acquireLatestSnapshot()->attempt.judgeCounts[PGreat] == 1;
    });
    require(admitted && audio.commitCount.load(std::memory_order_acquire) == 1,
            "Android touch commits its keysound before any render or touch-queue drain");
    clock.nowMicros.store(3'000'000, std::memory_order_release);
    require(waitUntil([&] {
      return worker.acquireLatestSnapshot()->noteStates[0].played;
    }), "worker passes note deadline without rendering");
    source.pumpPendingEvents();
    auto snapshot = worker.acquireLatestSnapshot();
    gameplay::RealtimeTouchSample sample;
    int auxiliarySamples = 0;
    while (session.auxiliaryTouches.tryPop(sample)) ++auxiliarySamples;
    require(admitted && snapshot->attempt.judgeCounts[PGreat] == 1 &&
                snapshot->attempt.judgeCounts[Poor] == 0 && auxiliarySamples == 1,
            "Android touch must reach judgement before render drain and automatic Poor");
    source.setRawEventCallback({});
    worker.stop();
  }
  SDL_Event mouse{};
  mouse.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
  mouse.button.x = 250;
  mouse.button.y = 125;
  const auto converted = gameplay::realtimeTouchSampleFromSdl(mouse, 123456,
      {.renderWidth = 1000, .renderHeight = 500, .inputScaleX = 2, .inputScaleY = 2});
  require(converted && converted->normalizedX == .5F && converted->normalizedY == .5F &&
              converted->phase == gameplay::RealtimeTouchPhase::Down &&
              converted->steadyTimestampMicros == 123456,
          "mouse ingress uses published drawable scaling and preserves time");
}

void testAndroidDedicatedTouchBypassesBlockedSdlWatch() {
  require(SDL_Init(SDL_INIT_EVENTS), "dedicated touch fixture initializes SDL events");
  FakeClock clock;
  FakeAudio audio;
  clock.nowMicros.store(1'000'000);
  auto config = makeConfig(clock, audio);
  config.clock.mapSteadyToSong = [](void *, std::int64_t) -> std::optional<std::int64_t> {
    return 1'000'000;
  };
  gameplay::RealtimeGameplayWorker worker(makeScratchlessDefinition(4), config);
  RealtimeGameplaySession session;
  session.worker = &worker;
  gameplay::RealtimeTouchLayout layout{
      .revision = 1, .bottomLeft = {0, 1}, .bottomRight = {1, 1},
      .topLeft = {0, 0}, .topRight = {1, 0},
      .lanes = {0}, .scratch = {false}, .laneCount = 1, .keyMode = 4};
  session.touchRouter = std::make_unique<gameplay::RealtimeTouchInputRouter>(7, layout,
      gameplay::RealtimeTouchInputSink{.context = &session,
                                       .emit = &RealtimeGameplaySession::emitTouchInput});
  require(session.touchHitSnapshots.publish({.layoutRevision = 1,
      .uiTransform = {.renderWidth = 1000, .renderHeight = 500,
                      .uiScaleX = 1, .uiScaleY = 1, .uiWidth = 1000, .uiHeight = 500}}),
          "dedicated touch geometry publishes");
  require(worker.start(), "dedicated touch worker starts");
  input::android::RawTouchRegistration registration(
      &RealtimeGameplaySession::nativeRawTouchSink, &session);
  input::android::RawTouchEvent raw{
      .pointerId = 42, .phase = input::android::TouchPhase::Down,
      .x = .5F, .y = .5F, .steadyTimestampMicros = 1'000'000};
  for (int gate = 0; gate < 3; ++gate) {
    session.acceptingTouch.store(gate != 0);
    session.owner.context.appInBackground.store(gate == 1);
    session.inputInterrupted.store(gate == 2);
    input::android::RawTouchRegistration::dispatch(raw);
    gameplay::RealtimeTouchSample ignored;
    require(!session.auxiliaryTouches.tryPop(ignored),
            "dedicated ingress respects closed, background and interrupted gates");
  }
  session.acceptingTouch.store(true);
  session.owner.context.appInBackground.store(false);
  session.inputInterrupted.store(false);

  struct Blocker {
    std::atomic_bool entered{false};
    std::binary_semaphore release{0};
  } blocker;
  const auto blockWatch = +[](void *context, SDL_Event *event) {
    if (event->type == SDL_EVENT_USER && event->user.code == 9137) {
      auto &state = *static_cast<Blocker *>(context);
      state.entered.store(true, std::memory_order_release);
      state.release.acquire();
    }
    return true;
  };
  require(SDL_AddEventWatch(blockWatch, &blocker), "unrelated SDL watch registers");
  std::thread stalledSdl([&] {
    SDL_Event event{};
    event.type = SDL_EVENT_USER;
    event.user.code = 9137;
    (void)SDL_PushEvent(&event);
  });
  require(waitUntil([&] { return blocker.entered.load(std::memory_order_acquire); }),
          "unrelated SDL eventwatch enters its blocking callback");
  std::thread nativeProducer([&] { input::android::RawTouchRegistration::dispatch(raw); });
  const bool judgedWhileSdlBlocked = waitUntil([&] {
    return worker.acquireLatestSnapshot()->attempt.judgeCounts[PGreat] == 1 &&
           audio.commitCount.load(std::memory_order_acquire) == 1;
  });
  // Release and join even when testing a broken implementation that waits on SDL.
  blocker.release.release();
  stalledSdl.join();
  nativeProducer.join();
  SDL_RemoveEventWatch(blockWatch, &blocker);
  require(judgedWhileSdlBlocked,
          "dedicated touch judges and commits its keysound while SDL eventwatch is blocked");
  gameplay::RealtimeTouchSample metadata;
  require(session.auxiliaryTouches.tryPop(metadata) && metadata.fingerId == raw.pointerId + 1 &&
              metadata.phase == gameplay::RealtimeTouchPhase::Down &&
              metadata.steadyTimestampMicros == raw.steadyTimestampMicros &&
              !session.auxiliaryTouches.tryPop(metadata),
          "dedicated touch publishes its SDL-compatible identity and original timestamp once");

  SDLTouchInputSource source(true);
  source.setRawEventCallback([&](const SDL_Event &event, std::uint64_t timestamp) {
    if (input::android::isSdlFingerEvent(event.type)) return;
    RealtimeGameplaySession::sdlTouchSink(session, event, timestamp);
  });
  SDL_Event copied{};
  copied.tfinger.fingerID = raw.pointerId + 1;
  copied.tfinger.x = .5F;
  copied.tfinger.y = .5F;
  for (const auto type : {SDL_EVENT_FINGER_DOWN, SDL_EVENT_FINGER_MOTION,
                          SDL_EVENT_FINGER_UP, SDL_EVENT_FINGER_CANCELED}) {
    copied.type = type;
    SDLTouchInputSource::EventHandler(&source, &copied);
  }
  require(!session.auxiliaryTouches.tryPop(metadata) &&
              worker.acquireLatestSnapshot()->lanePressed[0],
          "copied SDL fingers neither duplicate native input nor release its ownership");
  raw.phase = input::android::TouchPhase::Cancel;
  input::android::RawTouchRegistration::dispatch(raw);
  require(waitUntil([&] { return !worker.acquireLatestSnapshot()->lanePressed[0]; }),
          "dedicated cancellation releases the lane without an SDL event");
  require(session.auxiliaryTouches.tryPop(metadata) &&
              metadata.phase == gameplay::RealtimeTouchPhase::Cancel &&
              !session.auxiliaryTouches.tryPop(metadata),
          "dedicated cancellation publishes one terminal metadata sample");
  SDL_Event mouse{};
  mouse.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
  mouse.button.x = 500;
  mouse.button.y = 250;
  SDLTouchInputSource::EventHandler(&source, &mouse);
  require(waitUntil([&] { return worker.acquireLatestSnapshot()->lanePressed[0]; }),
          "real mouse input still reaches gameplay through the SDL fallback");
  mouse.type = SDL_EVENT_MOUSE_BUTTON_UP;
  SDLTouchInputSource::EventHandler(&source, &mouse);
  require(waitUntil([&] { return !worker.acquireLatestSnapshot()->lanePressed[0]; }),
          "real mouse release still reaches gameplay through the SDL fallback");
  source.setRawEventCallback({});
  worker.stop();
  SDL_QuitSubSystem(SDL_INIT_EVENTS);
}

void testAndroidSpinScratchExpiresWithoutRenderDrain() {
  for (const bool deferred : {false, true}) {
    FakeClock clock;
    FakeAudio audio;
    clock.nowMicros.store(1'000'000);
    RealtimeGameplaySession session;
    struct Maintenance {
      RealtimeGameplaySession &session;
      std::atomic<std::int64_t> steadyMicros{1'000'000};
      std::atomic<int> calls{0};
    } maintenance{session};
    auto config = makeConfig(clock, audio);
    config.inputMaintenance = {.context = &maintenance,
        .run = [](void *context, std::int64_t) {
          auto &state = *static_cast<Maintenance *>(context);
          RealtimeGameplaySession::maintainTouchInput(
              &state.session, state.steadyMicros.load());
          state.calls.fetch_add(1);
        }};
    gameplay::RealtimeGameplayWorker worker(makeScratchLongDefinition(), config);
    session.worker = &worker;
    gameplay::RealtimeTouchLayout layout;
    layout.revision = 1;
    layout.keyMode = 7;
    layout.laneRegions = {{
        .bottomLeft = {.20F, .80F}, .bottomRight = {.80F, .80F},
        .topLeft = {.20F, .20F}, .topRight = {.80F, .20F},
        .lane = 7, .scratch = true, .spinScratch = true,
        .requiresInside = true,
        .circle = gameplay::RealtimeTouchCircle{
            .center = {.50F, .50F}, .radiusX = .30F, .radiusY = .30F}}};
    session.touchRouter = std::make_unique<gameplay::RealtimeTouchInputRouter>(7, layout,
        gameplay::RealtimeTouchInputSink{.context = &session,
                                         .emit = &RealtimeGameplaySession::emitTouchInput});
    require(session.touchHitSnapshots.publish({.layoutRevision = 1,
        .uiTransform = {.renderWidth = 1000, .renderHeight = 500,
                        .uiScaleX = 1, .uiScaleY = 1, .uiWidth = 1000, .uiHeight = 500}}),
            "spin input geometry publishes");
    SDLTouchInputSource source(deferred);
    source.setRawEventCallback([&](const SDL_Event &event, std::uint64_t) {
      RealtimeGameplaySession::sdlTouchSink(session, event,
                                            maintenance.steadyMicros.load());
    });
    require(worker.start(), "spin expiry worker starts");
    SDL_Event event{};
    event.type = SDL_EVENT_FINGER_DOWN;
    event.tfinger.fingerID = 302;
    event.tfinger.x = .80F;
    event.tfinger.y = .50F;
    SDLTouchInputSource::EventHandler(&source, &event);
    event.type = SDL_EVENT_FINGER_MOTION;
    event.tfinger.x = .799F;
    event.tfinger.y = .521F;
    SDLTouchInputSource::EventHandler(&source, &event);
    require(waitUntil([&] { return worker.acquireLatestSnapshot()->lanePressed[7]; }),
            "a completed spin tick holds the scratch before rendering");
    if (deferred) {
      require(worker.suspend(), "spin maintenance suspends with gameplay");
      const int calls = maintenance.calls.load();
      maintenance.steadyMicros.store(1'150'000);
      std::this_thread::sleep_for(5ms);
      require(maintenance.calls.load() == calls &&
                  worker.acquireLatestSnapshot()->lanePressed[7],
              "suspended gameplay never runs touch maintenance");
      require(worker.resume(), "spin maintenance resumes with gameplay");
    } else {
      maintenance.steadyMicros.store(1'150'000);
    }
    require(waitUntil([&] { return !worker.acquireLatestSnapshot()->lanePressed[7]; }),
            "spin grace expires on the worker without rendering or another touch");
    maintenance.steadyMicros.store(1'150'010);
    event.tfinger.x = .797F;
    event.tfinger.y = .542F;
    SDLTouchInputSource::EventHandler(&source, &event);
    require(waitUntil([&] { return worker.acquireLatestSnapshot()->lanePressed[7]; }),
            "the same captured finger can turn again after worker expiry");
    source.setRawEventCallback({});
    worker.stop();
    const auto replay = worker.copyAcceptedReplayInputAfterStop();
    require(replay && replay->size() == 3 && (*replay)[0].pressed &&
                !(*replay)[1].pressed && (*replay)[2].pressed,
            "worker expiry records the matching release and renewed press");
  }
}

void testCommandOnlyTouchScratchBypassesGameplayAndReplay() {
  for (const int mode : {4, 6, 8}) {
    FakeClock clock;
    FakeAudio audio;
    clock.nowMicros.store(1'000'000);
    gameplay::RealtimeGameplayWorker worker(makeScratchlessDefinition(mode), makeConfig(clock, audio));
    RealtimeGameplaySession session;
    session.worker = &worker;
    require(worker.start(), "command-only touch worker starts");
    for (const auto type : {gameplay::RealtimeGameplayInputType::Press,
                            gameplay::RealtimeGameplayInputType::Release}) {
      require(RealtimeGameplaySession::emitTouchInput(&session,
          {.epoch = 7, .type = type, .lane = -1, .steadyTimestampMicros = 1'000'000,
           .hasReplayControl = true,
           .replayControl = {.kind = replay::LogicalControlKind::ScratchClockwise}}),
          "command-only touch scratch reaches the scene command queue");
    }
    require(RealtimeGameplaySession::emitTouchInput(&session,
        {.epoch = 7, .type = gameplay::RealtimeGameplayInputType::Press, .lane = 0,
         .steadyTimestampMicros = 1'000'000, .hasReplayControl = true,
         .replayControl = {.kind = replay::LogicalControlKind::Lane, .lane = 0}}),
        "a following real key still reaches gameplay");
    require(waitUntil([&] { return worker.acquireLatestSnapshot()->attempt.judgeCounts[PGreat] == 1; }),
            "worker processes the real key after scratch commands");
    worker.stop();
    const auto recorded = worker.copyAcceptedReplayInputAfterStop();
    require(recorded && recorded->size() == 1 &&
                recorded->front().control.kind == replay::LogicalControlKind::Lane &&
                session.startSelectInputs.size() == 3,
            "command-only scratch reaches controls but never the note replay stream");
  }
}

void testAndroidCancellationReleasesBeforeRenderAndAllowsFingerReuse() {
  for (const bool deferred : {false, true}) {
    FakeClock clock;
    FakeAudio audio;
    clock.nowMicros.store(1'000'000);
    auto config = makeConfig(clock, audio);
    config.clock.mapSteadyToSong = [](void *, std::int64_t) -> std::optional<std::int64_t> {
      return 1'000'000;
    };
    gameplay::RealtimeGameplayWorker worker(makeScratchlessDefinition(4), config);
    RealtimeGameplaySession session;
    session.worker = &worker;
    gameplay::RealtimeTouchLayout layout{
        .revision = 1, .bottomLeft = {0, 1}, .bottomRight = {1, 1},
        .topLeft = {0, 0}, .topRight = {1, 0},
        .lanes = {0, 1}, .scratch = {false, false}, .laneCount = 2, .keyMode = 4};
    session.touchRouter = std::make_unique<gameplay::RealtimeTouchInputRouter>(7, layout,
        gameplay::RealtimeTouchInputSink{.context = &session,
                                         .emit = &RealtimeGameplaySession::emitTouchInput});
    require(session.touchHitSnapshots.publish({.layoutRevision = 1,
        .uiTransform = {.renderWidth = 1000, .renderHeight = 500,
                        .uiScaleX = 1, .uiScaleY = 1, .uiWidth = 1000, .uiHeight = 500}}),
            "cancellation input geometry publishes");
    SDLTouchInputSource source(deferred);
    source.setRawEventCallback([&](const SDL_Event &event, std::uint64_t time) {
      RealtimeGameplaySession::sdlTouchSink(session, event, time);
    });
    require(worker.start(), "cancellation worker starts");
    SDL_Event finger{};
    finger.type = SDL_EVENT_FINGER_DOWN;
    finger.tfinger.fingerID = 42;
    finger.tfinger.x = .25F;
    finger.tfinger.y = .5F;
    SDLTouchInputSource::EventHandler(&source, &finger);
    require(waitUntil([&] { return worker.acquireLatestSnapshot()->lanePressed[0]; }),
            "finger owns its gameplay lane before cancellation");
    finger.tfinger.fingerID = 43;
    finger.tfinger.x = .75F;
    SDLTouchInputSource::EventHandler(&source, &finger);
    require(waitUntil([&] { return worker.acquireLatestSnapshot()->lanePressed[1]; }),
            "a second finger independently owns another lane");
    finger.tfinger.fingerID = 42;
    finger.tfinger.x = .25F;
    finger.type = SDL_EVENT_FINGER_CANCELED;
    SDLTouchInputSource::EventHandler(&source, &finger);
    require(waitUntil([&] {
      const auto snapshot = worker.acquireLatestSnapshot();
      return !snapshot->lanePressed[0] && snapshot->lanePressed[1];
    }),
            "Android terminal cancellation releases before any render or touch-queue drain");
    require(session.acceptingTouch.load() && !session.touchRoutingRecoveryRequested.load(),
            "terminal cancellation acknowledges ownership without entering recovery");
    gameplay::RealtimeTouchSample metadata;
    require(session.auxiliaryTouches.tryPop(metadata) &&
                metadata.phase == gameplay::RealtimeTouchPhase::Down && metadata.fingerId == 42 &&
                session.auxiliaryTouches.tryPop(metadata) &&
                metadata.phase == gameplay::RealtimeTouchPhase::Down && metadata.fingerId == 43 &&
                session.auxiliaryTouches.tryPop(metadata) &&
                metadata.phase == gameplay::RealtimeTouchPhase::Cancel && metadata.fingerId == 42 &&
                !session.auxiliaryTouches.tryPop(metadata),
            "terminal gameplay release preserves one Cancel for presentation and replay");
    finger.type = SDL_EVENT_FINGER_MOTION;
    SDLTouchInputSource::EventHandler(&source, &finger);
    require(!session.auxiliaryTouches.tryPop(metadata),
            "a stale Move cannot continue an Android cancelled contact");
    finger.type = SDL_EVENT_FINGER_DOWN;
    SDLTouchInputSource::EventHandler(&source, &finger);
    require(waitUntil([&] { return worker.acquireLatestSnapshot()->lanePressed[0]; }),
            "reusing the cancelled finger ID creates a fresh gameplay press");
    require(session.auxiliaryTouches.tryPop(metadata) &&
                metadata.phase == gameplay::RealtimeTouchPhase::Down,
            "reused finger publishes a new presentation contact");
    finger.type = SDL_EVENT_FINGER_UP;
    SDLTouchInputSource::EventHandler(&source, &finger);
    require(waitUntil([&] { return !worker.acquireLatestSnapshot()->lanePressed[0]; }),
            "reused finger releases normally");
    require(worker.acquireLatestSnapshot()->lanePressed[1],
            "cancellation and finger reuse preserve the other contact");
    finger.tfinger.fingerID = 43;
    finger.tfinger.x = .75F;
    SDLTouchInputSource::EventHandler(&source, &finger);
    require(waitUntil([&] { return !worker.acquireLatestSnapshot()->lanePressed[1]; }),
            "the uncancelled finger releases normally");
    source.setRawEventCallback({});
    worker.stop();
    const auto replay = worker.copyAcceptedReplayInputAfterStop();
    require(replay && replay->size() == 6 &&
                (*replay)[0].pressed && (*replay)[1].pressed &&
                !(*replay)[2].pressed && (*replay)[3].pressed &&
                !(*replay)[4].pressed && !(*replay)[5].pressed,
            "cancelled and reused contacts retain balanced replay ownership");
  }
}

void testAndroidSyntheticMouseDoesNotStealPointerZero() {
  FakeClock clock;
  FakeAudio audio;
  clock.nowMicros.store(1'000'000);
  auto config = makeConfig(clock, audio);
  config.clock.mapSteadyToSong = [](void *, std::int64_t) -> std::optional<std::int64_t> {
    return 1'000'000;
  };
  bms_parser::Chart chart;
  chart.Meta.TotalNotes = 2;
  chart.Meta.KeyMode = 4;
  auto *measure = new bms_parser::Measure();
  auto *timeline = addTimeline(*measure, 1'000'000);
  timeline->SetNote(0, new bms_parser::Note(11));
  timeline->SetNote(1, new bms_parser::Note(12));
  chart.Measures.push_back(measure);
  gameplay::RealtimeGameplayWorker worker(gameplay::buildGameplayDefinition(chart, 0), config);
  RealtimeGameplaySession session;
  session.worker = &worker;
  gameplay::RealtimeTouchLayout layout{
      .revision = 1, .bottomLeft = {0, 1}, .bottomRight = {1, 1},
      .topLeft = {0, 0}, .topRight = {1, 0},
      .lanes = {0, 1}, .scratch = {false, false}, .laneCount = 2, .keyMode = 4};
  session.touchRouter = std::make_unique<gameplay::RealtimeTouchInputRouter>(7, layout,
      gameplay::RealtimeTouchInputSink{.context = &session,
                                       .emit = &RealtimeGameplaySession::emitTouchInput});
  require(session.touchHitSnapshots.publish({.layoutRevision = 1,
      .uiTransform = {.renderWidth = 1000, .renderHeight = 500,
                      .uiScaleX = 1, .uiScaleY = 1, .uiWidth = 1000, .uiHeight = 500}}),
          "two-lane input geometry publishes");
  SDLTouchInputSource source(true);
  source.setRawEventCallback([&](const SDL_Event &event, std::uint64_t time) {
    RealtimeGameplaySession::sdlTouchSink(session, event, time);
  });
  require(worker.start(), "multitouch worker starts");
  // SDL sends a touch-synthesized mouse Down before its originating FingerDown.
  SDL_Event mouse{};
  mouse.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
  mouse.button.which = SDL_TOUCH_MOUSEID;
  mouse.button.x = 250;
  mouse.button.y = 250;
  SDLTouchInputSource::EventHandler(&source, &mouse);
  SDL_Event finger{};
  finger.type = SDL_EVENT_FINGER_DOWN;
  finger.tfinger.fingerID = 5;
  finger.tfinger.x = .25F;
  finger.tfinger.y = .5F;
  SDLTouchInputSource::EventHandler(&source, &finger);
  finger.tfinger.fingerID = 0;
  finger.tfinger.x = .75F;
  SDLTouchInputSource::EventHandler(&source, &finger);
  require(waitUntil([&] { return worker.acquireLatestSnapshot()->attempt.judgeCounts[PGreat] == 2; }),
          "synthetic mouse must not steal pointer zero or drop a simultaneous second note");
  finger.type = SDL_EVENT_FINGER_UP;
  SDLTouchInputSource::EventHandler(&source, &finger);
  require(waitUntil([&] {
    auto snapshot = worker.acquireLatestSnapshot();
    return snapshot->lanePressed[0] && !snapshot->lanePressed[1];
  }), "releasing pointer zero must preserve the other physical finger's hold");
  finger.tfinger.fingerID = 5;
  finger.tfinger.x = .25F;
  SDLTouchInputSource::EventHandler(&source, &finger);
  require(waitUntil([&] { return !worker.acquireLatestSnapshot()->lanePressed[0]; }),
          "last physical release clears its own held lane");
  source.setRawEventCallback({});
  worker.stop();
}

void testPreparationSnapshotsDoNotVisitUnchangedLargeChart() {
  bms_parser::Chart chart;
  chart.Meta.TotalNotes = 100'000;
  chart.Meta.KeyMode = 7;
  auto *measure = new bms_parser::Measure();
  for (int index = 0; index < chart.Meta.TotalNotes; ++index) {
    addTimeline(*measure, 1'000'000LL + index * 10'000LL)
        ->SetNote(1, new bms_parser::Note(1));
  }
  chart.Measures.push_back(measure);
  FakeClock clock;
  FakeAudio audio;
  auto config = makeConfig(clock, audio);
  config.activationSongTimeMicros = 1'000'000;
  gameplay::RealtimeGameplayWorker worker(
      gameplay::buildGameplayDefinition(chart, 0), config);
  // Pin an old buffer while the other two repeatedly rotate.
  auto initial = worker.acquireLatestSnapshot();
  const auto revision = initial->noteChanges.revision;
  require(worker.start(), "large sparse worker starts");
  for (int index = 0; index < 8; ++index) {
    require(worker.enqueueInput({.epoch = 7,
        .type = index % 2 == 0 ? gameplay::RealtimeGameplayInputType::Press
                              : gameplay::RealtimeGameplayInputType::Release,
        .lane = 1, .steadyTimestampMicros = 0}), "preparation input queued");
    require(waitUntil([&] {
      auto snapshot = worker.acquireLatestSnapshot();
      return snapshot->transactionSequence >= static_cast<unsigned>(index + 1);
    }), "preparation snapshot published");
  }
  auto latest = worker.acquireLatestSnapshot();
  std::size_t visited = 0;
  latest->noteChanges.forEachSince(revision, latest->noteStates.size(),
      [&](gameplay::NoteId) { ++visited; });
  require(visited == 0, "unchanged 100k-note snapshots require zero note visits");
  require(!latest->noteStates.back().played && !initial->noteStates.front().played,
          "complete compatibility snapshots and pinned lease remain intact");
  worker.stop();
}

void testSparseSnapshotsCatchUpLongNotePairsAcrossSkippedGenerations() {
  FakeClock clock;
  FakeAudio audio;
  gameplay::RealtimeGameplayWorker worker(makeScratchLongDefinition(),
                                           makeConfig(clock, audio));
  auto initial = worker.acquireLatestSnapshot();
  const auto revision = initial->noteChanges.revision;
  require(worker.start(), "long-note sparse worker starts");
  const auto send = [&](gameplay::RealtimeGameplayInputType type,
                        std::int64_t time, std::uint64_t sequence) {
    require(worker.enqueueInput({.epoch = 7, .type = type, .lane = 7,
        .backSpin = true, .steadyTimestampMicros = time}), "long-note input queued");
    require(waitUntil([&] {
      auto current = worker.acquireLatestSnapshot();
      return current->transactionSequence >= sequence;
    }), "long-note sparse snapshot published");
  };
  send(gameplay::RealtimeGameplayInputType::Press, 1'000'000, 1);
  {
    auto held = worker.acquireLatestSnapshot();
    require(held->noteStates[0].holding && held->noteStates[1].holding &&
                held->longNoteHoldingByLane[7], "both pair identities hold");
  }
  // Keep the initial lease, skip consumer application, and rotate both writable
  // buffers through unrelated lane events before releasing the long note.
  for (std::uint64_t sequence = 2; sequence <= 7; ++sequence) {
    require(worker.enqueueInput({.epoch = 7,
        .type = sequence % 2 == 0 ? gameplay::RealtimeGameplayInputType::Press
                                 : gameplay::RealtimeGameplayInputType::Release,
        .lane = 1, .steadyTimestampMicros = 1'100'000}), "unrelated input queued");
    require(waitUntil([&] {
      auto current = worker.acquireLatestSnapshot();
      return current->transactionSequence >= sequence;
    }), "rotating snapshot published");
  }
  send(gameplay::RealtimeGameplayInputType::Release, 2'000'000, 8);
  auto final = worker.acquireLatestSnapshot();
  std::array<bool, 2> changed{};
  final->noteChanges.forEachSince(revision, final->noteStates.size(),
      [&](gameplay::NoteId id) { changed.at(id) = true; });
  require(changed[0] && changed[1] && final->noteStates[0].played &&
              final->noteStates[1].played && !final->noteStates[0].holding &&
              !final->noteStates[1].holding && !final->longNoteHoldingByLane[7] &&
              final->noteStates[1].releaseTimeMicros == 2'000'000,
          "skipped generations retain both pair updates and the release timestamp");
  require(!initial->noteStates[0].played && !initial->noteStates[1].holding,
          "pinned old snapshot is immutable across rotating publications");
  worker.stop();
}

void testSnapshotJournalOverrunResynchronizesCompleteState() {
  constexpr int count = 4200;
  bms_parser::Chart chart;
  chart.Meta.TotalNotes = count;
  chart.Meta.KeyMode = 7;
  auto *measure = new bms_parser::Measure();
  for (int index = 0; index < count; ++index) {
    addTimeline(*measure, 1'000'000)->SetNote(1, new bms_parser::Note(1));
  }
  chart.Measures.push_back(measure);
  FakeClock clock;
  FakeAudio audio;
  auto config = makeConfig(clock, audio);
  config.simulation.attempt.autoPlay = true;
  config.simulation.attempt.replayCapacity = count + 10;
  config.simulation.attempt.gaugeHistoryCapacity = count + 10;
  config.simulation.attempt.automaticResultCapacity = count * 2 + 10;
  gameplay::RealtimeGameplayWorker worker(
      gameplay::buildGameplayDefinition(chart, 0), config);
  auto initial = worker.acquireLatestSnapshot();
  require(worker.start(), "journal-overrun worker starts");
  require(worker.enqueueInput({.epoch = 7,
      .type = gameplay::RealtimeGameplayInputType::Press,
      .lane = 1, .steadyTimestampMicros = 0}), "initialize before autoplay notes");
  require(waitUntil([&] {
    return worker.acquireLatestSnapshot()->transactionSequence != 0;
  }), "initial advance completes before autoplay batch");
  clock.nowMicros.store(1'000'001);
  require(waitUntil([&] {
    auto current = worker.acquireLatestSnapshot();
    return current->attempt.judgeCounts[PGreat] == count;
  }), "entire autoplay batch publishes despite journal overrun");
  auto final = worker.acquireLatestSnapshot();
  std::size_t visited = 0;
  final->noteChanges.forEachSince(initial->noteChanges.revision,
      final->noteStates.size(), [&](gameplay::NoteId id) {
    ++visited;
    require(final->noteStates[id].played, "overrun compatibility snapshot is complete");
  });
  require(visited == count && final->attempt.score == count * 2 &&
              worker.fault() == gameplay::RealtimeGameplayFault::None,
          "slow consumer resynchronizes every note after bounded journal overrun");
  worker.stop();
}

void testPinnedBuffersPublishPendingStateAfterReaderReleases() {
  FakeClock clock;
  FakeAudio audio;
  auto config = makeConfig(clock, audio);
  config.activationSongTimeMicros = 1'000'000;
  gameplay::RealtimeGameplayWorker worker(makeRapidDefinition(), config);
  auto first = worker.acquireLatestSnapshot();
  require(worker.start(), "pinned-buffer worker starts");
  const auto enqueue = [&](gameplay::RealtimeGameplayInputType type) {
    require(worker.enqueueInput({.epoch = 7, .type = type,
        .lane = 1, .steadyTimestampMicros = 0}), "pinned-buffer input queued");
  };
  enqueue(gameplay::RealtimeGameplayInputType::Press);
  require(waitUntil([&] {
    return worker.acquireLatestSnapshot()->transactionSequence == 1;
  }), "second buffer published");
  auto second = worker.acquireLatestSnapshot();
  enqueue(gameplay::RealtimeGameplayInputType::Release);
  require(waitUntil([&] {
    return worker.acquireLatestSnapshot()->transactionSequence == 2;
  }), "third buffer published");
  enqueue(gameplay::RealtimeGameplayInputType::Press);
  require(waitUntil([&] { return audio.commitCount.load() == 2; }),
          "input sound commits even while both writable buffers are pinned");
  require(worker.suspend(), "suspend acknowledges completed input processing");
  require(worker.acquireLatestSnapshot()->transactionSequence == 2,
          "leased snapshots are never overwritten");
  first = {};
  require(worker.resume(), "resume retries pending publication without new input");
  require(waitUntil([&] {
    return worker.acquireLatestSnapshot()->transactionSequence == 3;
  }), "releasing a reader eventually publishes already accepted state");
  worker.stop();
}

void testNoteJournalBoundaryAndNewReader() {
  gameplay::NoteStateChanges changes;
  const auto before = changes.revision;
  for (std::size_t index = 0; index < gameplay::NoteStateChanges::capacity; ++index) {
    changes.record(2);
  }
  std::size_t visited = 0;
  changes.forEachSince(before, 9, [&](gameplay::NoteId id) {
    require(id == 2, "exact-capacity history still contains all sparse changes");
    ++visited;
  });
  require(visited == gameplay::NoteStateChanges::capacity,
          "exact-capacity history does not fall back early");
  changes.record(4);
  for (const auto revision : {before, std::uint64_t{0}, changes.revision + 1}) {
    std::array<bool, 9> visitedIds{};
    changes.forEachSince(revision, 9, [&](gameplay::NoteId id) {
      visitedIds.at(id) = true;
    });
    require(std::all_of(visitedIds.begin(), visitedIds.end(), [](bool value) { return value; }),
            "overrun, new attempt and reset reader revisions resynchronize all notes");
  }
}

#if ASOBMASHOW_ENABLE_PERF_TELEMETRY
void testTouchLatencyPairsOnlySuccessfulTouchSoundCommits() {
  using namespace perf::latency;
  using Source = gameplay::RealtimeGameplayInputSource;
  using Type = gameplay::RealtimeGameplayInputType;
  struct Case {
    Source source;
    Type type;
    bool allowCommit;
    bool lr2 = false;
    bool scratch = false;
  };
  for (const auto test : {Case{Source::Touch, Type::Press, true},
                          Case{Source::Touch, Type::Release, true},
                          Case{Source::Touch, Type::Press, false},
                          Case{Source::Physical, Type::Press, true},
                          Case{Source::Touch, Type::Press, true, true},
                          Case{Source::Touch, Type::Press, true, true, true},
                          Case{Source::Physical, Type::Press, true, true, true}}) {
    const auto ingressBefore = snapshot(Stage::IngressToWorker).count;
    const auto touchWorkerBefore = snapshot(Stage::TouchToWorker).count;
    const auto touchSoundBefore = snapshot(Stage::TouchToSoundCommit).count;
    FakeClock clock;
    FakeAudio audio;
    clock.nowMicros.store(1'000'000);
    audio.allowCommit.store(test.allowCommit);
    auto config = makeConfig(clock, audio);
    if (test.lr2) {
      config.simulation.judge = gameplay::CompiledGameplayJudge::from(
          gameplay::compileGameplayJudgeRules(GameplayRuleset::LR2, 2));
    }
    config.clock.mapSteadyToSong = [](void *, std::int64_t) -> std::optional<std::int64_t> {
      return 1'000'000;
    };
    gameplay::RealtimeGameplayWorker worker(
        test.scratch ? makeScratchLongDefinition() : makeRapidDefinition(), config);
    constexpr std::int64_t minimumSourceAgeMicros = 50'000;
    const auto sourceMicros = nowMicros() - minimumSourceAgeMicros;
    require(sourceMicros > 0, "touch measurement fixture has a known past steady timestamp");
    require(worker.start(), "paired touch measurement worker starts");
    require(worker.enqueueInput({.epoch = 7, .type = test.type, .source = test.source,
        .lane = test.scratch ? 7 : 1, .steadyTimestampMicros = sourceMicros,
        .hasReplayControl = test.scratch,
        .replayControl = {.kind = replay::LogicalControlKind::ScratchClockwise, .player = 1}}),
        "paired touch measurement input queues");
    require(waitUntil([&] {
      return snapshot(Stage::IngressToWorker).count == ingressBefore + 1;
    }), "paired touch measurement input reaches the worker");
    worker.stop();
    const bool touch = test.source == Source::Touch;
    const bool audible = test.type == Type::Press && test.allowCommit;
    require(snapshot(Stage::TouchToWorker).count == touchWorkerBefore + (touch ? 1 : 0),
            "touch-to-worker measurement counts touch inputs without counting physical inputs");
    require(snapshot(Stage::TouchToSoundCommit).count == touchSoundBefore + (touch && audible ? 1 : 0),
            "paired touch-to-sound measurement excludes releases, failed commits and non-touch sounds");
    require(audio.commitCount.load() == (audible ? 1 : 0) &&
                worker.fault() == (test.allowCommit ? gameplay::RealtimeGameplayFault::None
                    : gameplay::RealtimeGameplayFault::AudioCommitFailed),
            "measurement eligibility follows the actual sound commit result");
    if (touch && audible) {
      require(snapshot(Stage::TouchToWorker).maximum >= minimumSourceAgeMicros &&
                  snapshot(Stage::TouchToSoundCommit).maximum >= minimumSourceAgeMicros,
              "paired measurements include time before ingress from the original touch timestamp");
    }
  }
}

void testWorkerRecordsMeasuredIngressAndSoundStages() {
  using namespace perf::latency;
  const auto queueBefore = snapshot(Stage::IngressToWorker).count;
  const auto soundBefore = snapshot(Stage::WorkerToSoundCommit).count;
  FakeClock clock;
  FakeAudio audio;
  auto config = makeConfig(clock, audio);
  config.activationSongTimeMicros = 1'000'000;
  gameplay::RealtimeGameplayWorker worker(makeRapidDefinition(), config);
  require(worker.start(), "measured-stage worker starts");
  for (std::uint64_t sequence = 1; sequence <= 3; ++sequence) {
    require(worker.enqueueInput({.epoch = 7,
        .type = sequence % 2 ? gameplay::RealtimeGameplayInputType::Press
                            : gameplay::RealtimeGameplayInputType::Release,
        .lane = 1, .steadyTimestampMicros = 0}), "measured input queued");
    require(waitUntil([&] {
      return worker.acquireLatestSnapshot()->transactionSequence == sequence;
    }), "measured input publishes");
  }
  worker.stop();
  require(snapshot(Stage::IngressToWorker).count == queueBefore + 3,
          "every accepted input records measured queue residence");
  require(snapshot(Stage::WorkerToSoundCommit).count == soundBefore + 2,
          "successful sound commits record processing duration only for presses");
}
#endif

void testWorkerLaunchFailureReleasesAdmission() {
  FakeClock clock;
  FakeAudio audio;
  gameplay::RealtimeGameplayWorker worker(makeRapidDefinition(),
                                           makeConfig(clock, audio));
  bool threw = false;
  {
    test_support::FailNextAllocation failure;
    try {
      worker.start();
    } catch (const std::bad_alloc &) {
      threw = true;
    }
  }
  require(threw, "worker launch allocation failure propagates to its caller");
  require(!worker.running(), "failed worker launch restores stopped state");
  require(worker.fault() == gameplay::RealtimeGameplayFault::None,
          "worker launch failure does not invent a gameplay fault");
  require(audio.commitCount.load() == 0,
          "failed worker launch does not execute gameplay audio");
  require(worker.start(), "worker launch can retry without explicit stop");
  require(!worker.start(), "successful retry retains single-worker admission");
  require(worker.enqueueInput({.epoch = 7,
                               .type = gameplay::RealtimeGameplayInputType::Press,
                               .lane = 1,
                               .compensateLane = 1,
                               .steadyTimestampMicros = 1'000'000}),
          "retry admits gameplay input");
  require(waitUntil([&] { return audio.commitCount.load() == 1; }),
          "retry processes gameplay input");
  worker.stop();
  require(!worker.running() && audio.commitCount.load() == 1,
          "retry stops after exactly one audio commit");
}

void testRapidInputsCommitStateAndSoundWithoutFramePump() {
  FakeClock clock;
  FakeAudio audio;
  gameplay::RealtimeGameplayWorker worker(makeRapidDefinition(),
                                           makeConfig(clock, audio));
  require(worker.start(), "gameplay worker starts once");

  require(worker.enqueueInput({.epoch = 7,
                               .type = gameplay::RealtimeGameplayInputType::Press,
                               .lane = 1,
                               .compensateLane = 1,
                               .steadyTimestampMicros = 1'000'000}),
          "first press enters fixed ingress");
  require(worker.enqueueInput({.epoch = 7,
                               .type = gameplay::RealtimeGameplayInputType::Release,
                               .lane = 1,
                               .steadyTimestampMicros = 1'010'000}),
          "release enters fixed ingress");
  require(worker.enqueueInput({.epoch = 7,
                               .type = gameplay::RealtimeGameplayInputType::Press,
                               .lane = 1,
                               .compensateLane = 1,
                               .steadyTimestampMicros = 1'100'000}),
          "rapid second press enters fixed ingress");

  require(waitUntil([&] { return audio.commitCount.load() == 2; }),
          "worker commits both keysounds without an engine update");
  auto snapshot = worker.acquireLatestSnapshot();
  // Audio commit happens inside processing, before that batch is published.
  // Wait for the snapshot boundary whose state the assertions inspect.
  require(waitUntil([&] {
    snapshot = worker.acquireLatestSnapshot();
    return snapshot && snapshot->replayEventCount == 3;
  }), "the completed input batch publishes without a frame pump");
  require(snapshot && snapshot->noteStates.size() == 2 &&
              snapshot->noteStates[0].played &&
              snapshot->noteStates[1].played &&
              snapshot->attempt.score == 4 &&
              snapshot->attempt.combo == 2 &&
              snapshot->replayEventCount == 3,
          "the same serial transactions publish notes, score, combo, and "
          "replay progress");
  require(snapshot->transactionCount == 3 &&
              snapshot->transactions[0].result.hasLaneVisual &&
              snapshot->transactions[0].result.laneVisual.action ==
                  gameplay::LaneVisualAction::Press &&
              snapshot->transactions[1].result.hasLaneVisual &&
              snapshot->transactions[1].result.laneVisual.action ==
                  gameplay::LaneVisualAction::Release &&
              snapshot->transactions[2].result.hasLaneVisual &&
              snapshot->transactions[2].result.laneVisual.action ==
                  gameplay::LaneVisualAction::Press,
          "a slow renderer can replay every rapid lane transition in order");
  require(snapshot->transactions[0].result.hasJudge &&
              snapshot->transactions[0].result.hasReplayEvent &&
              snapshot->transactions[0].result.replayEvent.songTimeMicros ==
                  1'000'000 &&
              snapshot->transactions[2].result.hasJudge &&
              snapshot->transactions[2].result.hasReplayEvent &&
              snapshot->transactions[2].result.replayEvent.songTimeMicros ==
                  1'100'000,
          "catch-up judges retain each transaction source time instead of a frame timestamp");
  require(snapshot->skinGameplayGraph.judgementDistribution.size() == 2 &&
              snapshot->skinGameplayGraph.judgementDistribution[1][1] == 2 &&
              snapshot->skinGameplayGraph.earlyLateDistribution[1][1] == 2 &&
              snapshot->skinGameplayGraph.recentJudgeTimingIndex == 2 &&
              snapshot->skinGameplayGraph.recentJudgeTimingsMillis[1] == 0 &&
              snapshot->skinGameplayGraph.recentJudgeTimingsMillis[2] == 0 &&
              snapshot->skinGameplayGraph
                      .gaugeHistories[static_cast<std::size_t>(
                          gaugeTypeIndex(GaugeType::Normal))]
                      .size() == 3 &&
              snapshot->skinGameplayGraph
                      .gaugeHistories[static_cast<std::size_t>(
                          gaugeTypeIndex(GaugeType::Hard))]
                      .front() == 100.0F,
          "realtime snapshots transfer the producer-owned graph authority");
  require(worker.fault() == gameplay::RealtimeGameplayFault::None,
          "normal rapid input remains valid");
  worker.stop();
}

void testRealtimeSnapshotPublishesFlatGaugeSamplesWithoutInput() {
  FakeClock clock;
  FakeAudio audio;
  gameplay::RealtimeGameplayWorker worker(makeRapidDefinition(),
                                           makeConfig(clock, audio));
  require(worker.start(), "flat gauge sampling worker starts");
  const auto initial = worker.acquireLatestSnapshot();
  const auto initialGeneration = initial ? initial->generation : 0;

  clock.nowMicros.store(1'100'000, std::memory_order_release);
  require(waitUntil([&] {
            const auto snapshot = worker.acquireLatestSnapshot();
            return snapshot && snapshot->generation > initialGeneration &&
                   snapshot->skinGameplayGraph
                           .gaugeHistories[static_cast<std::size_t>(
                               gaugeTypeIndex(GaugeType::Normal))] ==
                       std::vector<float>({20.0F, 20.0F, 20.0F}) &&
                   snapshot->skinGameplayGraph
                           .gaugeHistories[static_cast<std::size_t>(
                               gaugeTypeIndex(GaugeType::Hard))] ==
                       std::vector<float>({100.0F, 100.0F, 100.0F});
          }),
          "realtime snapshots publish every flat 500 ms per-type gauge "
          "sample without waiting for a judgement");
  worker.stop();
}

void testRealtimeWorkerJudgesPhysicalLanesBeyondLegacyCapacity() {
  constexpr int keyMode = 130;
  constexpr int lane = 129;
  FakeClock clock;
  FakeAudio audio;
  gameplay::RealtimeGameplayWorker worker(
      makeArbitraryLaneDefinition(keyMode, lane), makeConfig(clock, audio));
  require(worker.start(), "arbitrary-lane worker starts");
  require(worker.enqueueInput(
              {.epoch = 7,
               .type = gameplay::RealtimeGameplayInputType::Press,
               .source = gameplay::RealtimeGameplayInputSource::Physical,
               .lane = lane,
               .compensateLane = lane,
               .steadyTimestampMicros = 1'000'000}),
          "physical lane beyond the legacy capacity enters worker ingress");
  require(waitUntil([&] {
            auto snapshot = worker.acquireLatestSnapshot();
            return snapshot && snapshot->transactionSequence >= 1;
          }),
          "physical lane beyond the legacy capacity reaches judgement");
  const auto snapshot = worker.acquireLatestSnapshot();
  require(snapshot &&
              snapshot->lanePressed[static_cast<std::size_t>(lane)] &&
              snapshot->latestTransaction.hasJudge &&
              snapshot->latestTransaction.judge.judgement == PGreat,
          "arbitrary physical lane publishes held and judged state");
  worker.stop();
}

void testLr2SameKeyBatchUsesLatestEdgeAndRetainsReplayHistory() {
  for (const bool scratch : {false, true}) {
    for (const bool endPressed : {false, true}) {
      FakeClock clock;
      FakeAudio audio;
      auto config = makeConfig(clock, audio);
      config.simulation.judge = gameplay::CompiledGameplayJudge::from(
          gameplay::compileGameplayJudgeRules(GameplayRuleset::LR2, 2));
      const int lane = scratch ? 7 : 1;
      const replay::LogicalControl control = scratch
          ? replay::LogicalControl{.kind = replay::LogicalControlKind::ScratchClockwise, .player = 1}
          : replay::LogicalControl{.kind = replay::LogicalControlKind::Lane, .player = 1, .lane = 2};
      gameplay::RealtimeGameplayWorker worker(
          scratch ? makeScratchLongDefinition() : makeRapidDefinition(), config);
      const int count = endPressed ? 3 : 2;
      for (int edge = 0; edge < count; ++edge) {
        require(worker.enqueueInput({.epoch = 7,
                    .type = edge % 2 == 0 ? gameplay::RealtimeGameplayInputType::Press
                                         : gameplay::RealtimeGameplayInputType::Release,
                    .source = gameplay::RealtimeGameplayInputSource::Physical, .lane = lane,
                    .steadyTimestampMicros = 1'000'000, .hasReplayControl = true,
                    .replayControl = control}),
                "same-key edges enqueue before worker start");
      }
      const auto generation = worker.acquireLatestSnapshot()->generation;
      require(worker.start(), "same-key batch worker starts");
      require(waitUntil([&] {
        const auto snapshot = worker.acquireLatestSnapshot();
        return snapshot && snapshot->generation > generation;
      }), "same-key latest edge is processed");
      const auto snapshot = worker.acquireLatestSnapshot();
      require(snapshot->attempt.judgeCounts[PGreat] == (endPressed ? 1 : 0) &&
                  snapshot->attempt.judgeCounts[Kpoor] == 0 &&
                  snapshot->lanePressed[lane] == endPressed,
              "one physical key judges only its latest same-time state");
      worker.stop();
      const auto raw = worker.copyAcceptedReplayInputAfterStop();
      require(raw && raw->size() == static_cast<std::size_t>(count),
              "same-key snapshot coalescing preserves every raw replay edge");
    }
  }
}

void testLr2ScratchBatchKeepsLatestKeyAndProcessingDirection() {
  FakeClock clock;
  FakeAudio audio;
  auto config = makeConfig(clock, audio);
  config.simulation.judge = gameplay::CompiledGameplayJudge::from(
      gameplay::compileGameplayJudgeRules(GameplayRuleset::LR2, 2));
  gameplay::RealtimeGameplayWorker worker(makeScratchLongDefinition(), config);
  const auto edge = [&](long long time, bool clockwise, bool pressed) {
    require(worker.enqueueInput({.epoch = 7,
        .type = pressed ? gameplay::RealtimeGameplayInputType::Press
                        : gameplay::RealtimeGameplayInputType::Release,
        .lane = 7, .steadyTimestampMicros = time, .hasReplayControl = true,
        .replayControl = {.kind = clockwise ? replay::LogicalControlKind::ScratchClockwise
                                            : replay::LogicalControlKind::ScratchCounterClockwise,
                          .player = 1}}), "scratch key edge enqueues");
  };
  edge(1'000'000, true, true);
  edge(1'000'000, false, true);
  edge(1'000'000, true, false);
  edge(2'000'000, true, true);
  edge(2'010'000, false, false);
  edge(2'020'000, true, false);
  require(worker.start(), "scratch snapshot worker starts");
  require(waitUntil([&] {
    const auto snapshot = worker.acquireLatestSnapshot();
    return snapshot && snapshot->attempt.judgeCounts[PGreat] >= 2;
  }), "scratch latest-key head owner survives until opposite-key tail press");
  const auto snapshot = worker.acquireLatestSnapshot();
  require(snapshot->attempt.judgeCounts[PGreat] == 2 &&
              snapshot->attempt.judgeCounts[Bad] == 0 &&
              snapshot->attempt.score == 4 && !snapshot->lanePressed[7],
          "scratch latest states judge only CCW head then CW charge tail");
  worker.stop();
  const auto raw = worker.copyAcceptedReplayInputAfterStop();
  require(raw && raw->size() == 6,
          "scratch snapshot judgement retains the entire raw key history");
}

void testLr2SimultaneousInputsUseLaneOrderAndOneUpdate() {
  for (const auto source : {gameplay::RealtimeGameplayInputSource::Independent,
                            gameplay::RealtimeGameplayInputSource::Physical}) {
    bms_parser::Chart chart;
    chart.Meta.TotalNotes = 3;
    chart.Meta.KeyMode = 7;
    auto *measure = new bms_parser::Measure();
    addTimeline(*measure, 850'000)->SetNote(0, new bms_parser::Note(1));
    addTimeline(*measure, 1'000'000)->SetNote(1, new bms_parser::Note(2));
    addTimeline(*measure, 3'000'000)->SetNote(2, new bms_parser::Note(3));
    chart.Measures.push_back(measure);
    FakeClock clock;
    FakeAudio audio;
    auto config = makeConfig(clock, audio);
    config.simulation.judge = gameplay::CompiledGameplayJudge::from(
        gameplay::compileGameplayJudgeRules(GameplayRuleset::LR2, 2));
    gameplay::RealtimeGameplayWorker worker(gameplay::buildGameplayDefinition(chart, 0), config);
    for (const int lane : {1, 0}) {
      require(worker.enqueueInput({.epoch = 7, .source = source, .lane = lane,
                  .steadyTimestampMicros = 1'000'000,
                  .hasReplayControl = true,
                  .replayControl = {.kind = replay::LogicalControlKind::Lane,
                                    .player = 1, .lane = lane + 1}}),
              "simultaneous inputs enqueue before the worker starts");
    }
    require(worker.start(), "simultaneous input worker starts");
    require(waitUntil([&] {
      auto snapshot = worker.acquireLatestSnapshot();
      return snapshot && snapshot->transactionSequence >= 2;
    }), "simultaneous lane inputs are processed");
    const auto snapshot = worker.acquireLatestSnapshot();
    require(snapshot->attempt.judgeCounts[Bad] == 1 &&
                snapshot->attempt.judgeCounts[PGreat] == 1 && snapshot->attempt.combo == 1,
            "simultaneous LR2 keys judge BAD on lower lane before PGREAT on higher lane");
    worker.stop();
    const auto raw = worker.copyAcceptedReplayInputAfterStop();
    require(raw && raw->size() == 2 && raw->front().control.lane == 2 &&
                raw->back().control.lane == 1,
            "lane normalization retains every original replay ingress transition");
  }
}

void testWorkerTransfersAcceptedRawReplayInputInOrder() {
  FakeClock clock;
  FakeAudio audio;
  gameplay::RealtimeGameplayWorker worker(makeRapidDefinition(),
                                           makeConfig(clock, audio));
  require(worker.start(), "raw replay worker starts");
  const replay::LogicalControl laneControl{
      .kind = replay::LogicalControlKind::Lane, .player = 1, .lane = 1};
  require(worker.enqueueInput(
              {.epoch = 7,
               .type = gameplay::RealtimeGameplayInputType::Press,
               .lane = 1,
               .compensateLane = 1,
               .steadyTimestampMicros = 1'000'000,
               .hasReplayControl = true,
               .replayControl = laneControl}),
          "raw replay press enters fixed ingress");
  require(worker.enqueueInput(
              {.epoch = 7,
               .type = gameplay::RealtimeGameplayInputType::Release,
               .lane = 1,
               .steadyTimestampMicros = 1'010'000,
               .hasReplayControl = true,
               .replayControl = laneControl}),
          "raw replay release enters fixed ingress");
  require(waitUntil([&] {
            auto snapshot = worker.acquireLatestSnapshot();
            return snapshot && snapshot->transactionSequence >= 2;
          }),
          "raw replay input is accepted by gameplay");
  worker.stop();

  const auto replayInput = worker.copyAcceptedReplayInputAfterStop();
  require(replayInput.has_value() &&
              *replayInput ==
                  std::vector<replay::InputTransition>{
                      {.songTimeMicros = 1'000'000,
                       .control = laneControl,
                       .pressed = true},
                      {.songTimeMicros = 1'010'000,
                       .control = laneControl,
                       .pressed = false}},
          "worker transfers the exact accepted logical stream after stop");
}

void testWorkerRetainsAcceptedReplayInputWithInterleavedTimestamps() {
  FakeClock clock;
  FakeAudio audio;
  gameplay::RealtimeGameplayWorker worker(makeRapidDefinition(),
                                           makeConfig(clock, audio));
  require(worker.start(), "interleaved replay worker starts");
  const auto start = replay::LogicalControl{
      .kind = replay::LogicalControlKind::Start, .player = 1, .lane = -1};
  const auto select = replay::LogicalControl{
      .kind = replay::LogicalControlKind::Select, .player = 1, .lane = -1};
  const auto beforeGeneration = worker.acquireLatestSnapshot()->generation;
  require(worker.enqueueInput(
              {.epoch = 7,
               .type = gameplay::RealtimeGameplayInputType::Press,
               .steadyTimestampMicros = 1'010'000,
               .hasReplayControl = true,
               .replayControl = start}) &&
              worker.enqueueInput(
                  {.epoch = 7,
                   .type = gameplay::RealtimeGameplayInputType::Press,
                   .steadyTimestampMicros = 1'000'000,
                   .hasReplayControl = true,
                   .replayControl = select}),
          "inputs from interleaved timestamp domains enter fixed ingress");
  require(waitUntil([&] {
            auto snapshot = worker.acquireLatestSnapshot();
            return snapshot && snapshot->generation > beforeGeneration;
          }),
          "interleaved replay inputs are processed");
  worker.stop();

  const auto replayInput = worker.copyAcceptedReplayInputAfterStop();
  require(replayInput.has_value() && replayInput->size() == 2,
          "the worker leaves timestamp ordering to capture normalization");
}

void requireOwnedRealtimeOverlapCoalesced(
    gameplay::GameplayDefinition definition, int lane,
    replay::LogicalControl control,
    gameplay::RealtimeGameplayInputSource persistentSource,
    gameplay::RealtimeGameplayInputSource overlappingSource,
    const char *heldMessage, const char *replayMessage) {
  FakeClock clock;
  FakeAudio audio;
  gameplay::RealtimeGameplayWorker worker(std::move(definition),
                                           makeConfig(clock, audio));
  require(worker.start(), "owned-overlap worker starts");

  const auto emit = [&](gameplay::RealtimeGameplayInputType type,
                        gameplay::RealtimeGameplayInputSource source,
                        std::int64_t timestamp) {
    return worker.enqueueInput({.epoch = 7,
                                .type = type,
                                .source = source,
                                .lane = lane,
                                .compensateLane = lane,
                                .steadyTimestampMicros = timestamp,
                                .hasReplayControl = true,
                                .replayControl = control});
  };
  require(emit(gameplay::RealtimeGameplayInputType::Press, persistentSource,
               1'000'000) &&
              emit(gameplay::RealtimeGameplayInputType::Press,
                   overlappingSource, 1'010'000) &&
              emit(gameplay::RealtimeGameplayInputType::Release,
                   overlappingSource, 1'020'000),
          "overlapping realtime owners enter the actual worker ingress");
  require(waitUntil([&] {
            auto snapshot = worker.acquireLatestSnapshot();
            return snapshot && snapshot->transactionSequence >= 1;
          }),
          "the first owned press reaches gameplay");
  {
    auto snapshot = worker.acquireLatestSnapshot();
    require(snapshot && snapshot->lanePressed[static_cast<std::size_t>(lane)] &&
                snapshot->transactionSequence == 1,
            heldMessage);
  }

  require(emit(gameplay::RealtimeGameplayInputType::Release, persistentSource,
               1'030'000),
          "the final realtime owner releases through the worker ingress");
  require(waitUntil([&] {
            auto snapshot = worker.acquireLatestSnapshot();
            return snapshot &&
                   !snapshot->lanePressed[static_cast<std::size_t>(lane)] &&
                   snapshot->transactionSequence >= 2;
          }),
          "the effective lane releases only with the final owner");
  worker.stop();

  const auto replayInput = worker.copyAcceptedReplayInputAfterStop();
  require(replayInput.has_value() &&
              *replayInput ==
                  std::vector<replay::InputTransition>{
                      {.songTimeMicros = 1'000'000,
                       .control = control,
                       .pressed = true},
                      {.songTimeMicros = 1'030'000,
                       .control = control,
                       .pressed = false}},
          replayMessage);
}

void testRealtimeIngressCoalescesTouchAndHardwareLaneOwnership() {
  const replay::LogicalControl laneControl{
      .kind = replay::LogicalControlKind::Lane, .player = 1, .lane = 1};
  requireOwnedRealtimeOverlapCoalesced(
      makeRapidDefinition(), 1, laneControl,
      gameplay::RealtimeGameplayInputSource::Physical,
      gameplay::RealtimeGameplayInputSource::Touch,
      "touch release cannot release a hardware-held realtime lane",
      "hardware-first overlap records one balanced logical lane pair");
  requireOwnedRealtimeOverlapCoalesced(
      makeRapidDefinition(), 1, laneControl,
      gameplay::RealtimeGameplayInputSource::Touch,
      gameplay::RealtimeGameplayInputSource::Physical,
      "hardware release cannot release a touch-held realtime lane",
      "touch-first overlap records one balanced logical lane pair");
}

void testRealtimeIngressCoalescesTouchAndHardwareScratchOwnership() {
  const replay::LogicalControl scratchControl{
      .kind = replay::LogicalControlKind::ScratchClockwise,
      .player = 1,
      .lane = -1};
  requireOwnedRealtimeOverlapCoalesced(
      makeScratchLongDefinition(), 7, scratchControl,
      gameplay::RealtimeGameplayInputSource::Physical,
      gameplay::RealtimeGameplayInputSource::Touch,
      "touch release cannot release a hardware-held realtime scratch",
      "hardware-first scratch overlap records one balanced direction pair");
  requireOwnedRealtimeOverlapCoalesced(
      makeScratchLongDefinition(), 7, scratchControl,
      gameplay::RealtimeGameplayInputSource::Touch,
      gameplay::RealtimeGameplayInputSource::Physical,
      "hardware release cannot release a touch-held realtime scratch",
      "touch-first scratch overlap records one balanced direction pair");
}

void testRealtimeIngressHandsOffOppositeScratchDirectionsWithoutLaneEdges() {
  FakeClock clock;
  FakeAudio audio;
  gameplay::RealtimeGameplayWorker worker(makeScratchLongDefinition(),
                                           makeConfig(clock, audio));
  require(worker.start(), "directional scratch ownership worker starts");
  const replay::LogicalControl clockwise{
      .kind = replay::LogicalControlKind::ScratchClockwise,
      .player = 1,
      .lane = -1};
  const replay::LogicalControl counterClockwise{
      .kind = replay::LogicalControlKind::ScratchCounterClockwise,
      .player = 1,
      .lane = -1};
  const auto emit = [&](gameplay::RealtimeGameplayInputType type,
                        gameplay::RealtimeGameplayInputSource source,
                        replay::LogicalControl control,
                        std::int64_t timestamp) {
    return worker.enqueueInput({.epoch = 7,
                                .type = type,
                                .source = source,
                                .lane = 7,
                                .compensateLane = 7,
                                .steadyTimestampMicros = timestamp,
                                .hasReplayControl = true,
                                .replayControl = control});
  };
  require(emit(gameplay::RealtimeGameplayInputType::Press,
               gameplay::RealtimeGameplayInputSource::Physical, clockwise,
               1'000'000) &&
              emit(gameplay::RealtimeGameplayInputType::Press,
                   gameplay::RealtimeGameplayInputSource::Touch,
                   counterClockwise, 1'010'000) &&
              emit(gameplay::RealtimeGameplayInputType::Release,
                   gameplay::RealtimeGameplayInputSource::Touch,
                   counterClockwise, 1'020'000),
          "opposite scratch owners enter the actual worker ingress");
  require(waitUntil([&] {
            auto snapshot = worker.acquireLatestSnapshot();
            return snapshot && snapshot->transactionSequence >= 1;
          }),
          "the first directional scratch press reaches gameplay");
  {
    auto snapshot = worker.acquireLatestSnapshot();
    require(snapshot && snapshot->lanePressed[7] &&
                snapshot->transactionSequence == 1,
            "direction handoffs change replay identity without duplicating "
            "the held gameplay lane");
  }
  require(emit(gameplay::RealtimeGameplayInputType::Release,
               gameplay::RealtimeGameplayInputSource::Physical, clockwise,
               1'030'000),
          "final scratch owner releases");
  require(waitUntil([&] {
            auto snapshot = worker.acquireLatestSnapshot();
            return snapshot && !snapshot->lanePressed[7];
          }),
          "scratch lane releases after its final owner");
  worker.stop();

  const auto replayInput = worker.copyAcceptedReplayInputAfterStop();
  require(replayInput.has_value() && replayInput->size() == 6 &&
              (*replayInput)[0] == replay::InputTransition{
                                       .songTimeMicros = 1'000'000,
                                       .control = clockwise,
                                       .pressed = true} &&
              (*replayInput)[1] == replay::InputTransition{
                                       .songTimeMicros = 1'010'000,
                                       .control = clockwise,
                                       .pressed = false,
                                       .replayOnly = true} &&
              (*replayInput)[2] == replay::InputTransition{
                                       .songTimeMicros = 1'010'000,
                                       .control = counterClockwise,
                                       .pressed = true,
                                       .replayOnly = true} &&
              (*replayInput)[3] == replay::InputTransition{
                                       .songTimeMicros = 1'020'000,
                                       .control = counterClockwise,
                                       .pressed = false,
                                       .replayOnly = true} &&
              (*replayInput)[4] == replay::InputTransition{
                                       .songTimeMicros = 1'020'000,
                                       .control = clockwise,
                                       .pressed = true,
                                       .replayOnly = true} &&
              (*replayInput)[5] == replay::InputTransition{
                                       .songTimeMicros = 1'030'000,
                                       .control = clockwise,
                                       .pressed = false},
          "opposite scratch owners produce canonical same-time replay-only "
          "handoffs");
}

class LegacyBridgeControl final : public IRhythmControl {
public:
  explicit LegacyBridgeControl(
      gameplay::RealtimeGameplayInputBridge &bridge) noexcept
      : bridge_(bridge) {}

  bms_parser::Note *pressLane(int mainLane, int compensateLane,
                              double inputDelay) override {
    prepared_ = bridge_.prepare(
                    gameplay::RealtimeGameplayInputType::Press, mainLane,
                    compensateLane, false, nextTimestamp(),
                    static_cast<std::int64_t>(inputDelay * 1'000'000.0)) &&
                prepared_;
    return nullptr;
  }

  bms_parser::Note *pressLane(int lane, double inputDelay) override {
    return pressLane(lane, lane, inputDelay);
  }

  bms_parser::Note *releaseLane(int lane, double inputDelay,
                                bool backSpin) override {
    prepared_ = bridge_.prepare(
                    gameplay::RealtimeGameplayInputType::Release, lane, lane,
                    backSpin, nextTimestamp(),
                    static_cast<std::int64_t>(inputDelay * 1'000'000.0)) &&
                prepared_;
    return nullptr;
  }

  [[nodiscard]] std::int64_t nextTimestamp() noexcept {
    const auto result = timestamp_;
    timestamp_ += 1'000;
    return result;
  }

  [[nodiscard]] bool prepared() const noexcept { return prepared_; }

private:
  gameplay::RealtimeGameplayInputBridge &bridge_;
  std::int64_t timestamp_ = 1'000'000;
  bool prepared_ = true;
};

void testLegacyBridgeKeepsUnmatchedCallbacksReplayOnly() {
  std::vector<gameplay::RealtimeGameplayInput> captured;
  gameplay::RealtimeGameplayInputBridge bridge(
      7,
      {.context = &captured,
       .emit = [](void *context,
                  const gameplay::RealtimeGameplayInput &input) {
         static_cast<std::vector<gameplay::RealtimeGameplayInput> *>(context)
             ->push_back(input);
         return true;
       }});
  const replay::LogicalControl laneControl{
      .kind = replay::LogicalControlKind::Lane, .player = 1, .lane = 4};

  require(bridge.emitApplied(4, laneControl, true, true, false, 1'000'000),
          "unmatched legacy callback reaches the realtime bridge");
  require(captured.size() == 1 && captured.front().lane == 4 &&
              captured.front().hasReplayControl &&
              captured.front().replayControl == laneControl &&
              captured.front().replayOnly,
          "unmatched legacy callback cannot become delayed gameplay input");
}

void testLegacyBridgeCapturesScratchlessModesAsBrdControls() {
  for (const int keyMode : {4, 6, 8}) {
    FakeClock clock;
    FakeAudio audio;
    gameplay::RealtimeGameplayWorker worker(makeScratchlessDefinition(keyMode),
                                             makeConfig(clock, audio));
    gameplay::RealtimeGameplayInputBridge bridge(
        7,
        {.context = &worker,
         .emit = [](void *context,
                    const gameplay::RealtimeGameplayInput &input) {
           return static_cast<gameplay::RealtimeGameplayWorker *>(context)
               ->enqueueInput(input);
         }});
    LegacyBridgeControl control(bridge);
    LogicalGameplayInputAdapter adapter(
        control, {}, [&](const auto &applied) {
          require(bridge.emitApplied(
                      applied.physicalLane, applied.control,
                      applied.hasReplayControl, applied.pressed,
                      applied.replayOnly, control.nextTimestamp()),
                  "scratchless legacy callback reaches the realtime bridge");
        });
    require(worker.start(), "scratchless legacy worker starts");

    const auto down = input::LogicalInputTransition{
        .scope = {.player = 1, .keyMode = keyMode},
        .action = {.kind = input::LogicalActionKind::Lane, .lane = 0},
        .pressed = true,
        .value = 1.0F};
    const auto up = input::LogicalInputTransition{
        .scope = {.player = 1, .keyMode = keyMode},
        .action = {.kind = input::LogicalActionKind::Lane, .lane = 0},
        .pressed = false,
        .value = 0.0F};
    adapter.apply(std::span(&down, 1));
    adapter.apply(std::span(&up, 1));

    require(waitUntil([&] {
              auto snapshot = worker.acquireLatestSnapshot();
              return snapshot && snapshot->attempt.score == 2 &&
                     !snapshot->lanePressed[0];
            }),
            "scratchless legacy input judges through the realtime worker");
    worker.stop();
    const auto replayInput = worker.copyAcceptedReplayInputAfterStop();
    const replay::LogicalControl laneControl{
        .kind = replay::LogicalControlKind::Lane, .player = 1, .lane = 0};
    require(replayInput.has_value() &&
                *replayInput ==
                    std::vector<replay::InputTransition>{
                        {.songTimeMicros = 1'000'000,
                         .control = laneControl,
                         .pressed = true},
                        {.songTimeMicros = 1'002'000,
                         .control = laneControl,
                         .pressed = false}},
            "scratchless gameplay preserves its BMS channel lane in BRD");
  }
}

void testLegacyAdapterScratchHandoffsValidateAsOneReplayTransaction() {
  FakeClock clock;
  FakeAudio audio;
  gameplay::RealtimeGameplayWorker worker(makeScratchLongDefinition(),
                                           makeConfig(clock, audio));
  gameplay::RealtimeGameplayInputBridge bridge(
      7,
      {.context = &worker,
       .emit = [](void *context,
                  const gameplay::RealtimeGameplayInput &input) {
         return static_cast<gameplay::RealtimeGameplayWorker *>(context)
             ->enqueueInput(input);
       }});
  LegacyBridgeControl control(bridge);
  LogicalGameplayInputAdapter adapter(
      control, {}, [&](const auto &applied) {
        require(bridge.emitApplied(
                    applied.physicalLane, applied.control,
                    applied.hasReplayControl, applied.pressed,
                    applied.replayOnly, control.nextTimestamp()),
                "legacy adapter callback reaches the realtime bridge");
      });
  require(worker.start(), "legacy adapter replay worker starts");

  const auto digitalDown = input::LogicalInputTransition{
      .scope = {.player = 1, .keyMode = 7},
      .action = {.kind = input::LogicalActionKind::Lane, .lane = 7},
      .pressed = true,
      .value = 1.0F};
  const auto digitalUp = input::LogicalInputTransition{
      .scope = {.player = 1, .keyMode = 7},
      .action = {.kind = input::LogicalActionKind::Lane, .lane = 7},
      .pressed = false,
      .value = 0.0F};
  const auto counterClockwiseDown = input::LogicalInputTransition{
      .scope = {.player = 1, .keyMode = 7},
      .action = {.kind =
                     input::LogicalActionKind::ScratchCounterClockwise},
      .pressed = true,
      .value = 1.0F};
  const auto counterClockwiseUp = input::LogicalInputTransition{
      .scope = {.player = 1, .keyMode = 7},
      .action = {.kind =
                     input::LogicalActionKind::ScratchCounterClockwise},
      .pressed = false,
      .value = 0.0F};

  adapter.apply(std::span(&digitalDown, 1));
  adapter.apply(std::span(&counterClockwiseDown, 1));
  adapter.apply(std::span(&counterClockwiseUp, 1));
  adapter.apply(std::span(&digitalUp, 1));
  require(control.prepared(),
          "every legacy physical edge is staged for its applied callback");
  require(waitUntil([&] {
            auto snapshot = worker.acquireLatestSnapshot();
            return snapshot && !snapshot->lanePressed[7] &&
                   snapshot->transactionSequence >= 2;
          }),
          "legacy adapter input drains through the realtime worker");
  worker.stop();

  const auto replayInput = worker.copyAcceptedReplayInputAfterStop();
  require(replayInput.has_value() && replayInput->size() == 6,
          "legacy adapter produces a balanced directional scratch stream");
  replay::ReplayPlaybackData playback;
  playback.setup.chart.md5 = std::string(32, 'b');
  playback.setup.chart.sha256 = std::string(64, 'a');
  playback.setup.chart.keyMode = 7;
  playback.setup.longNoteMode = 1;
  playback.input = *replayInput;
  const auto validation = replay::validateReplayPlayback(
      playback, replay::ReplaySetupSource::LocalCapture,
      {.completionSongTimeMicros = 5'000'000});
  require(validation.valid(),
          "adapter scratch ownership handoffs satisfy replay validation");
  require((*replayInput)[1].songTimeMicros ==
              (*replayInput)[2].songTimeMicros &&
              (*replayInput)[3].songTimeMicros ==
                  (*replayInput)[4].songTimeMicros,
          "each replay-only release and opposite press shares one timestamp");
}

void testLegacyStartSelectCommandsRemainStockReplayInput() {
  FakeClock clock;
  FakeAudio audio;
  gameplay::RealtimeGameplayWorker worker(makeRapidDefinition(),
                                           makeConfig(clock, audio));
  gameplay::RealtimeGameplayInputBridge bridge(
      7,
      {.context = &worker,
       .emit = [](void *context,
                  const gameplay::RealtimeGameplayInput &input) {
         return static_cast<gameplay::RealtimeGameplayWorker *>(context)
             ->enqueueInput(input);
       }});
  LegacyBridgeControl control(bridge);
  LogicalGameplayInputAdapter adapter(
      control, [](const auto &) {}, [&](const auto &applied) {
        require(bridge.emitApplied(
                    applied.physicalLane, applied.control,
                    applied.hasReplayControl, applied.pressed,
                    applied.replayOnly, control.nextTimestamp()),
                "legacy command callback reaches the realtime bridge");
      });
  require(worker.start(), "legacy command replay worker starts");
  const auto before = worker.acquireLatestSnapshot();
  const auto beforeGeneration = before ? before->generation : 0;

  const auto startDown = input::LogicalInputTransition{
      .scope = {.player = 1, .keyMode = 7},
      .action = {.kind = input::LogicalActionKind::Start},
      .pressed = true,
      .value = 1.0F};
  const auto startUp = input::LogicalInputTransition{
      .scope = {.player = 1, .keyMode = 7},
      .action = {.kind = input::LogicalActionKind::Start},
      .pressed = false,
      .value = 0.0F};
  const auto selectDown = input::LogicalInputTransition{
      .scope = {.player = 1, .keyMode = 7},
      .action = {.kind = input::LogicalActionKind::Select},
      .pressed = true,
      .value = 1.0F};
  const auto selectUp = input::LogicalInputTransition{
      .scope = {.player = 1, .keyMode = 7},
      .action = {.kind = input::LogicalActionKind::Select},
      .pressed = false,
      .value = 0.0F};
  adapter.apply(std::span(&startDown, 1));
  adapter.apply(std::span(&startUp, 1));
  adapter.apply(std::span(&selectDown, 1));
  adapter.apply(std::span(&selectUp, 1));
  require(waitUntil([&] {
            auto snapshot = worker.acquireLatestSnapshot();
            return snapshot && snapshot->generation > beforeGeneration;
          }),
          "legacy commands drain through the realtime worker");
  worker.stop();

  const auto replayInput = worker.copyAcceptedReplayInputAfterStop();
  require(replayInput.has_value() && replayInput->size() == 4,
          "Start and Select press and release survive live replay capture");
  replay::ReplayPlaybackData playback;
  playback.setup.chart.md5 = std::string(32, 'b');
  playback.setup.chart.sha256 = std::string(64, 'a');
  playback.setup.chart.keyMode = 7;
  playback.setup.longNoteMode = 1;
  playback.input = *replayInput;
  const auto validation = replay::validateReplayPlayback(
      playback, replay::ReplaySetupSource::LocalCapture,
      {.completionSongTimeMicros = 5'000'000});
  require(validation.valid(),
          "Start and Select remain stock BRD commands, not replay-only "
          "scratch handoffs");
}

void testReplayCaptureOverflowDoesNotInvalidateGameplay() {
  FakeClock clock;
  FakeAudio audio;
  auto config = makeConfig(clock, audio);
  config.maximumReplayInputTransitions = 1;
  gameplay::RealtimeGameplayWorker worker(makeRapidDefinition(),
                                           std::move(config));
  require(worker.start(), "bounded replay worker starts");
  const replay::LogicalControl laneControl{
      .kind = replay::LogicalControlKind::Lane, .player = 1, .lane = 1};
  require(worker.enqueueInput(
              {.epoch = 7,
               .type = gameplay::RealtimeGameplayInputType::Press,
               .lane = 1,
               .compensateLane = 1,
               .steadyTimestampMicros = 1'000'000,
               .hasReplayControl = true,
               .replayControl = laneControl}) &&
              worker.enqueueInput(
                  {.epoch = 7,
                   .type = gameplay::RealtimeGameplayInputType::Release,
                   .lane = 1,
                   .steadyTimestampMicros = 1'010'000,
                   .hasReplayControl = true,
                   .replayControl = laneControl}),
          "overflow fixture enqueues both gameplay edges");
  require(waitUntil([&] {
            auto snapshot = worker.acquireLatestSnapshot();
            return snapshot && snapshot->transactionSequence >= 2;
          }),
          "gameplay still accepts edges after replay capture overflow");
  worker.stop();
  require(worker.fault() == gameplay::RealtimeGameplayFault::None &&
              !worker.copyAcceptedReplayInputAfterStop().has_value(),
          "replay overflow drops only the attachment, not the result");
}

void testInputDelayCompensationPrecedesWorkerAutomaticDeadline() {
  FakeClock clock;
  FakeAudio audio;
  gameplay::RealtimeGameplayWorker worker(makeRapidDefinition(),
                                           makeConfig(clock, audio));
  const auto judge = gameplay::CompiledGameplayJudge::from(Judge(1));
  const std::int64_t inputDelayMicros = judge.latePoorTimingMicros() + 1;
  require(worker.start(), "compensated-input worker starts");
  require(worker.enqueueInput(
              {.epoch = 7,
               .type = gameplay::RealtimeGameplayInputType::Press,
               .lane = 1,
               .compensateLane = 1,
               .steadyTimestampMicros = 1'000'000 + inputDelayMicros,
               .inputDelayMicros = inputDelayMicros}),
          "compensated press enters fixed ingress");
  require(waitUntil([&] {
            auto snapshot = worker.acquireLatestSnapshot();
            return snapshot && snapshot->transactionSequence >= 1;
          }),
          "compensated press publishes a worker transaction");
  auto snapshot = worker.acquireLatestSnapshot();
  require(snapshot && snapshot->noteStates[0].played &&
              !snapshot->noteStates[0].dead &&
              snapshot->attempt.judgeCounts[PGreat] == 1 &&
              snapshot->attempt.judgeCounts[Poor] == 0,
          "worker judges compensated input before raw-time expiration");
  worker.stop();
}

void testInputPreadvancePublishesAutomaticTransactions() {
  FakeClock clock;
  FakeAudio audio;
  gameplay::RealtimeGameplayWorker worker(makeRapidDefinition(),
                                           makeConfig(clock, audio));
  clock.nowMicros.store(2'000'000, std::memory_order_release);
  require(worker.enqueueInput(
              {.epoch = 7,
               .type = gameplay::RealtimeGameplayInputType::Press,
               .lane = 2,
               .compensateLane = 2,
               .steadyTimestampMicros = 2'000'000}),
          "late input is queued before the worker's first wake");
  require(worker.start(), "input-preadvance fixture starts");
  require(waitUntil([&] {
            auto snapshot = worker.acquireLatestSnapshot();
            return snapshot && snapshot->attempt.judgeCounts[Poor] > 0;
          }),
          "input preadvance resolves expired notes");
  auto snapshot = worker.acquireLatestSnapshot();
  bool publishedPoor = false;
  for (std::size_t index = 0; snapshot && index < snapshot->transactionCount;
       ++index) {
    const auto &result = snapshot->transactions[index].result;
    publishedPoor = publishedPoor ||
                    (result.hasJudge && result.judge.judgement == Poor);
  }
  require(publishedPoor,
          "automatic misses produced before an input remain in transaction "
          "history");
  worker.stop();
}

void testSnapshotPublishesHeldLongNoteByLane() {
  FakeClock clock;
  FakeAudio audio;
  gameplay::RealtimeGameplayWorker worker(makeScratchLongDefinition(),
                                           makeConfig(clock, audio));
  gameplay::RealtimeGameplayInputBridge bridge(
      7, {.context = &worker,
          .emit = [](void *context, const gameplay::RealtimeGameplayInput &input) {
            return static_cast<gameplay::RealtimeGameplayWorker *>(context)->enqueueInput(input);
          }});
  const replay::LogicalControl control{
      .kind = replay::LogicalControlKind::ScratchClockwise, .player = 1};
  require(worker.start(), "long-note snapshot worker starts");
  require(bridge.prepare(gameplay::RealtimeGameplayInputType::Press, 7, 7,
                         false, 1'000'000, 0) &&
              bridge.emitApplied(7, control, true, true, false, 1'000'000),
          "legacy scratch head enters the canonical worker through its applied callback");
  require(waitUntil([&] {
            auto snapshot = worker.acquireLatestSnapshot();
            return snapshot && snapshot->longNoteHoldingByLane[7] &&
                   snapshot->attempt.score == 2;
          }),
          "worker snapshot publishes held long-note state for scratch routing");
  require(bridge.prepare(gameplay::RealtimeGameplayInputType::Release, 7, 7,
                         true, 2'000'000, 0) &&
              bridge.emitApplied(7, control, true, false, false, 2'000'000),
          "legacy backspin tail enters the same canonical worker");
  require(waitUntil([&] {
            auto snapshot = worker.acquireLatestSnapshot();
            return snapshot && !snapshot->longNoteHoldingByLane[7] &&
                   snapshot->attempt.score == 4 && snapshot->attempt.combo == 2;
          }),
          "legacy bridge scores both charge identities and clears the published scratch hold");
  worker.stop();
}

void testHeldLongNoteRecoveryDoesNotReserveAnotherKeysound() {
  for (bool scratch : {false, true}) {
    for (const auto ruleset : {GameplayRuleset::Beatoraja, GameplayRuleset::LR2}) {
      if (!scratch && ruleset == GameplayRuleset::LR2) continue;
      bms_parser::Chart chart;
      chart.Meta.KeyMode = scratch ? 7 : 9;
      chart.Meta.TotalNotes = scratch ? 2 : 1;
      const int lane = scratch ? 7 : 1;
      const auto type = scratch ? bms_parser::LongNoteType::ChargeNote
                                : bms_parser::LongNoteType::LongNote;
      auto *measure = new bms_parser::Measure();
      auto *head = new bms_parser::LongNote(41, type);
      auto *tail = new bms_parser::LongNote(41, type);
      head->Tail = tail;
      tail->Head = head;
      addTimeline(*measure, 1'000'000)->SetNote(lane, head);
      addTimeline(*measure, 2'000'000)->SetNote(lane, tail);
      chart.Measures.push_back(measure);
      FakeClock clock;
      FakeAudio audio;
      auto config = makeConfig(clock, audio);
      config.simulation.judge = gameplay::CompiledGameplayJudge::from(
          gameplay::compileGameplayJudgeRules(ruleset, 3, 100, 100,
              CourseJudgementConstraint::None,
              gameplay::CandidateSelectionMode::Lowest, chart.Meta.KeyMode));
      gameplay::RealtimeGameplayWorker worker(
          gameplay::buildGameplayDefinition(chart, 0), config);
      require(worker.start(), "long-note recovery worker starts");
      require(worker.enqueueInput({.epoch = 7,
          .type = gameplay::RealtimeGameplayInputType::Press,
          .lane = lane, .compensateLane = lane,
          .steadyTimestampMicros = 1'000'000}), "recovery head enters worker");
      require(waitUntil([&] { return audio.commitCount.load() == 1; }),
              "long-note head commits its input-triggered keysound");
      const long long releaseTime = scratch ? 1'950'000 : 1'500'000;
      require(worker.enqueueInput({.epoch = 7,
          .type = gameplay::RealtimeGameplayInputType::Release,
          .lane = lane, .steadyTimestampMicros = releaseTime}) &&
          worker.enqueueInput({.epoch = 7,
          .type = gameplay::RealtimeGameplayInputType::Press,
          .lane = lane, .compensateLane = lane,
          .steadyTimestampMicros = releaseTime + 10'000}),
          "held-tail release and recovery enter worker");
      require(waitUntil([&] {
        const auto snapshot = worker.acquireLatestSnapshot();
        return worker.fault() != gameplay::RealtimeGameplayFault::None ||
               (snapshot && snapshot->transactionSequence >= 3);
      }), "worker processes recovery");
      require(worker.fault() == gameplay::RealtimeGameplayFault::None &&
                  audio.reserveCount.load() == 1 && audio.commitCount.load() == 1,
              "held-tail recovery must not reserve a keysound or fault live play");
      const auto snapshot = worker.acquireLatestSnapshot();
      require(snapshot && snapshot->longNoteHoldingByLane[lane] &&
                  snapshot->replayEventCount == 3,
              "successful recovery preserves the hold and records its input edge");
      worker.stop();
    }
  }
}

void testAudioCapacityFailureDoesNotClaimTheNote() {
  FakeClock clock;
  FakeAudio audio;
  audio.allowReserve.store(false);
  gameplay::RealtimeGameplayWorker worker(makeRapidDefinition(),
                                           makeConfig(clock, audio));
  require(worker.start(), "fault fixture starts");
  require(worker.enqueueInput({.epoch = 7,
                               .type = gameplay::RealtimeGameplayInputType::Press,
                               .lane = 1,
                               .compensateLane = 1,
                               .steadyTimestampMicros = 1'000'000}),
          "sound-triggering press enters ingress before exhaustion is known");
  require(waitUntil([&] {
            return worker.fault() ==
                   gameplay::RealtimeGameplayFault::AudioCapacityUnavailable;
          }),
          "audio exhaustion latches an integrity fault");
  auto snapshot = worker.acquireLatestSnapshot();
  require(snapshot && !snapshot->noteStates[0].played &&
              snapshot->attempt.score == 0 &&
              snapshot->replayEventCount == 0 &&
              audio.commitCount.load() == 0,
          "failed reservation produces neither a claimed note nor a sound");
  worker.stop();
}

void testIngressOverflowFailsClosed() {
  FakeClock clock;
  FakeAudio audio;
  gameplay::RealtimeGameplayWorker worker(makeRapidDefinition(),
                                           makeConfig(clock, audio));
  for (std::size_t index = 0; index < gameplay::kRealtimeGameplayIngressSize;
       ++index) {
    require(worker.enqueueInput({.epoch = 7,
                                 .type = gameplay::RealtimeGameplayInputType::Release,
                                 .lane = 1,
                                 .steadyTimestampMicros =
                                     static_cast<long long>(index)}),
            "every fixed ingress slot accepts one digital edge");
  }
  require(!worker.enqueueInput({.epoch = 7,
                                .type = gameplay::RealtimeGameplayInputType::Release,
                                .lane = 1}),
          "the first overflowing digital edge is rejected");
  require(worker.fault() == gameplay::RealtimeGameplayFault::IngressOverflow,
          "digital overflow invalidates the attempt instead of dropping "
          "silently");
}

void testSuspendFreezesAutomaticDeadlinesUntilResume() {
  FakeClock clock;
  FakeAudio audio;
  gameplay::RealtimeGameplayWorker worker(makeRapidDefinition(),
                                           makeConfig(clock, audio));
  require(worker.start(), "suspend fixture starts");
  require(worker.suspend(), "worker acknowledges suspension");

  clock.nowMicros.store(2'000'000, std::memory_order_release);
  std::this_thread::sleep_for(10ms);
  {
    auto snapshot = worker.acquireLatestSnapshot();
    require(snapshot && !snapshot->noteStates[0].dead &&
                !snapshot->noteStates[1].dead &&
                snapshot->attempt.judgeCounts[Poor] == 0,
            "paused wall time cannot advance misses or note state");
  }

  worker.resume();
  require(waitUntil([&] {
            auto snapshot = worker.acquireLatestSnapshot();
            return snapshot && snapshot->attempt.judgeCounts[Poor] == 2;
          }),
          "automatic deadlines continue after resume");
  worker.stop();
}

template <typename Worker> bool requestSuspension(Worker &worker) {
  if constexpr (requires { worker.requestSuspend(); }) {
    return worker.requestSuspend();
  }
  return false;
}

void testProducerSuspensionProtectsHeldNotesDuringMainThreadStall() {
  FakeClock clock;
  FakeAudio audio;
  gameplay::RealtimeGameplayWorker worker(makeScratchLongDefinition(),
                                           makeConfig(clock, audio));
  require(worker.start(), "interruption suspension fixture starts");
  clock.nowMicros = 1'000'000;
  require(worker.enqueueInput({.epoch = 7,
      .type = gameplay::RealtimeGameplayInputType::Press, .lane = 7,
      .steadyTimestampMicros = 1'000'000}), "long head enters worker");
  require(waitUntil([&] {
    return worker.acquireLatestSnapshot()->noteStates.front().holding;
  }), "head holds before native interruption");
  require(requestSuspension(worker),
          "native producer can request suspension without waiting for the main thread");
  clock.nowMicros = 4'000'000;
  std::this_thread::sleep_for(30ms);
  require(worker.suspend() && worker.suspend(),
          "main thread joins an existing suspension request idempotently");
  {
    const auto snapshot = worker.acquireLatestSnapshot();
    require(snapshot->noteStates.front().holding &&
                !snapshot->noteStates.back().played &&
                snapshot->attempt.judgeCounts[Poor] == 0,
            "a stalled main thread cannot release held notes or advance automatic misses after interruption");
  }
  clock.nowMicros = 1'000'000;
  require(worker.resume(), "explicit resume returns to the frozen chart clock");
  require(worker.enqueueInput({.epoch = 7,
      .type = gameplay::RealtimeGameplayInputType::Release, .lane = 7,
      .backSpin = true, .steadyTimestampMicros = 2'000'000}),
      "post-resume release is accepted");
  require(waitUntil([&] {
    return worker.acquireLatestSnapshot()->noteStates.back().played;
  }), "the same attempt completes the held note after resume");
  require(worker.fault() == gameplay::RealtimeGameplayFault::None,
          "recoverable suspension keeps scoring and replay authority valid");
  worker.stop();
}

void testActivationGateAllowsPreparationFeedbackButNoGameplay() {
  FakeClock clock;
  FakeAudio audio;
  auto config = makeConfig(clock, audio);
  config.activationSongTimeMicros = 1'000'000;
  gameplay::RealtimeGameplayWorker worker(makeRapidDefinition(),
                                           std::move(config));
  const auto initial = worker.acquireLatestSnapshot()->attempt;
  require(worker.start(), "activation-gate fixture starts");
  require(worker.enqueueInput({.epoch = 7,
                               .type = gameplay::RealtimeGameplayInputType::Press,
                               .lane = 1,
                               .compensateLane = 1,
                               .steadyTimestampMicros = 900'000}),
          "preparation press reaches the serial authority");
  require(waitUntil([&] {
            auto snapshot = worker.acquireLatestSnapshot();
            return audio.commitCount.load(std::memory_order_acquire) == 1 &&
                   snapshot && snapshot->transactionSequence >= 1;
          }),
          "preparation keysound and visual transaction commit without a "
          "frame pump");
  {
    auto snapshot = worker.acquireLatestSnapshot();
    const auto &transaction = snapshot->latestTransaction;
    require(snapshot->lanePressed[1] &&
                audio.lastCommitted.load(std::memory_order_acquire) == 0 &&
                transaction.soundNoteId == 0 &&
                transaction.noteId == gameplay::kInvalidNoteId &&
                !transaction.hasJudge && transaction.hasLaneVisual &&
                transaction.laneVisual.action ==
                    gameplay::LaneVisualAction::Press &&
                transaction.hasReplayEvent &&
                !snapshot->noteStates[0].played &&
                !snapshot->noteStates[0].dead &&
                sameAttemptSnapshot(initial, snapshot->attempt),
            "preparation feedback leaves note, judgement, score, combo, and "
            "gauge untouched");
  }

  clock.nowMicros.store(999'999, std::memory_order_release);
  std::this_thread::sleep_for(10ms);
  {
    auto snapshot = worker.acquireLatestSnapshot();
    require(snapshot && !snapshot->noteStates[0].played &&
                !snapshot->noteStates[0].dead &&
                snapshot->attempt.judgeCounts[Poor] == 0,
            "automatic miss deadlines remain blocked before activation");
  }

  require(worker.enqueueInput({.epoch = 7,
                               .type = gameplay::RealtimeGameplayInputType::Press,
                               .lane = 1,
                               .compensateLane = 1,
                               .steadyTimestampMicros = 1'000'000}),
          "held-lane active press reaches the serial authority");
  std::this_thread::sleep_for(10ms);
  require(audio.commitCount.load() == 1,
          "held input crossing activation cannot retrigger sound or "
          "judgement");
  {
    auto snapshot = worker.acquireLatestSnapshot();
    require(snapshot && !snapshot->noteStates[0].played,
            "held input crossing activation leaves the note unresolved");
  }

  require(worker.enqueueInput({.epoch = 7,
                               .type = gameplay::RealtimeGameplayInputType::Release,
                               .lane = 1,
                               .steadyTimestampMicros = 1'001'000}),
          "post-activation release reaches the authority");
  require(worker.enqueueInput({.epoch = 7,
                               .type = gameplay::RealtimeGameplayInputType::Press,
                               .lane = 1,
                               .compensateLane = 1,
                               .steadyTimestampMicros = 1'005'000}),
          "post-activation repress reaches the authority");
  require(waitUntil([&] {
            auto snapshot = worker.acquireLatestSnapshot();
            return audio.commitCount.load(std::memory_order_acquire) == 2 &&
                   snapshot && snapshot->noteStates[0].played;
          }),
          "release and repress commits the ordinary judged keysound");
  {
    auto snapshot = worker.acquireLatestSnapshot();
    require(snapshot && snapshot->attempt.judgeCounts[PGreat] == 1,
            "post-activation repress judges normally");
  }
  worker.stop();
}

void testPreparationReleasePublishesOrderedVisualTransaction() {
  FakeClock clock;
  FakeAudio audio;
  auto config = makeConfig(clock, audio);
  config.activationSongTimeMicros = 1'000'000;
  gameplay::RealtimeGameplayWorker worker(makeRapidDefinition(),
                                           std::move(config));
  require(worker.start(), "preparation release fixture starts");
  require(worker.enqueueInput({.epoch = 7,
                               .type = gameplay::RealtimeGameplayInputType::Press,
                               .lane = 1,
                               .compensateLane = 1,
                               .steadyTimestampMicros = 900'000}) &&
              worker.enqueueInput({.epoch = 7,
                                   .type = gameplay::RealtimeGameplayInputType::Release,
                                   .lane = 1,
                                   .steadyTimestampMicros = 910'000}),
          "preparation press and release enter the serial authority");
  require(waitUntil([&] {
            auto snapshot = worker.acquireLatestSnapshot();
            return snapshot && snapshot->transactionSequence >= 2;
          }),
          "both preparation transactions publish without a frame pump");
  auto snapshot = worker.acquireLatestSnapshot();
  require(snapshot && !snapshot->lanePressed[1] &&
              snapshot->transactionCount >= 2 &&
              snapshot->transactions[0].result.laneVisual.action ==
                  gameplay::LaneVisualAction::Press &&
              snapshot->transactions[1].result.laneVisual.action ==
                  gameplay::LaneVisualAction::Release &&
              !snapshot->transactions[1].result.hasJudge,
          "preparation lane visuals preserve press-release order");
  worker.stop();
}

void testPreparationAudioReservationFailureDoesNotClaimLane() {
  FakeClock clock;
  FakeAudio audio;
  audio.allowReserve.store(false, std::memory_order_release);
  auto config = makeConfig(clock, audio);
  config.activationSongTimeMicros = 1'000'000;
  gameplay::RealtimeGameplayWorker worker(makeRapidDefinition(),
                                           std::move(config));
  require(worker.start(), "preparation audio-fault fixture starts");
  require(worker.enqueueInput({.epoch = 7,
                               .type = gameplay::RealtimeGameplayInputType::Press,
                               .lane = 1,
                               .compensateLane = 1,
                               .steadyTimestampMicros = 900'000}),
          "preparation audio-fault press enters the authority");
  require(waitUntil([&] {
            return worker.fault() ==
                   gameplay::RealtimeGameplayFault::AudioCapacityUnavailable;
          }),
          "preparation audio reservation failure faults the attempt");
  auto snapshot = worker.acquireLatestSnapshot();
  require(snapshot && !snapshot->lanePressed[1] &&
              !snapshot->noteStates[0].played &&
              snapshot->transactionSequence == 0,
          "failed preparation reservation commits no lane or gameplay state");
  worker.stop();
}

void testPracticeCountInPressJudgesFirstInRangeNote() {
  FakeClock clock;
  FakeAudio audio;
  auto config = makeConfig(clock, audio);
  config.simulation.allowedNoteRange = gameplay::GameplayTimeRange{
      .startMicros = 1'000'000, .endMicros = 1'100'000};
  config.practiceCompletionSongTimeMicros = 1'100'000;
  gameplay::RealtimeGameplayWorker worker(makePracticeDefinition(),
                                           std::move(config));
  require(worker.start(), "practice count-in worker starts");
  require(worker.enqueueInput({.epoch = 7,
                               .type = gameplay::RealtimeGameplayInputType::Press,
                               .lane = 1,
                               .compensateLane = 1,
                               .steadyTimestampMicros = 999'999}),
          "count-in press reaches the practice authority");
  require(waitUntil([&] {
    const auto published = worker.acquireLatestSnapshot();
    return audio.commitCount.load() == 1 && published &&
           published->transactionSequence != 0;
  }), "valid early count-in hit commits and publishes its keysound transaction");
  auto snapshot = worker.acquireLatestSnapshot();
  require(snapshot && snapshot->noteStates[0].played &&
              snapshot->attempt.judgeCounts[PGreat] == 1,
          "valid early count-in hit judges the first in-range note");
  worker.stop();
}

void testPracticeCountInPressOutsideJudgeWindowStaysUnjudged() {
  FakeClock clock;
  FakeAudio audio;
  auto config = makeConfig(clock, audio);
  config.simulation.allowedNoteRange = gameplay::GameplayTimeRange{
      .startMicros = 1'000'000, .endMicros = 1'100'000};
  config.practiceCompletionSongTimeMicros = 1'100'000;
  gameplay::RealtimeGameplayWorker worker(makePracticeDefinition(),
                                           std::move(config));
  require(worker.start(), "early count-in rejection worker starts");
  require(worker.enqueueInput({.epoch = 7,
                               .type = gameplay::RealtimeGameplayInputType::Press,
                               .lane = 1,
                               .compensateLane = 1,
                               .steadyTimestampMicros = 499'999}),
          "far-early count-in press reaches the practice authority");
  require(waitUntil([&] {
            return audio.commitCount.load(std::memory_order_acquire) == 1;
          }),
          "far-early count-in press commits the first in-range manual "
          "keysound");
  auto snapshot = worker.acquireLatestSnapshot();
  require(snapshot && !snapshot->noteStates[0].played &&
              snapshot->attempt.judgeCounts ==
                  std::array<int, JudgementCount>{} &&
              audio.commitCount.load() == 1,
          "count-in press outside every judge window sounds but stays "
          "unjudged");
  worker.stop();
}

void testPracticeCompletesFromAudioClockWithoutFramePump() {
  FakeClock clock;
  FakeAudio audio;
  auto config = makeConfig(clock, audio);
  config.simulation.allowedNoteRange = gameplay::GameplayTimeRange{
      .startMicros = 1'000'000, .endMicros = 1'100'000};
  config.practiceCompletionSongTimeMicros = 1'100'000;
  gameplay::RealtimeGameplayWorker worker(makePracticeDefinition(),
                                           std::move(config));
  require(worker.start(), "bounded practice worker starts");
  clock.nowMicros.store(1'100'000, std::memory_order_release);
  require(waitUntil([&] {
            auto snapshot = worker.acquireLatestSnapshot();
            return snapshot && snapshot->terminalReason ==
                                   gameplay::GameplayTerminalReason::PracticeComplete;
          }),
          "audio clock publishes PracticeComplete without a frame pump");
  auto snapshot = worker.acquireLatestSnapshot();
  require(snapshot && snapshot->noteStates[0].dead &&
              !snapshot->noteStates[1].played &&
              snapshot->attempt.judgeCounts[Poor] == 1 &&
              snapshot->replayEventCount == 1,
          "practice finalization misses only unresolved in-range identities");
  worker.stop();
}

void testPracticeAutoplayCompletesWithoutFramePump() {
  FakeClock clock;
  FakeAudio audio;
  auto config = makeConfig(clock, audio);
  config.simulation.allowedNoteRange = gameplay::GameplayTimeRange{
      .startMicros = 1'000'000, .endMicros = 1'100'000};
  config.simulation.attempt.autoPlay = true;
  config.practiceCompletionSongTimeMicros = 1'100'000;
  gameplay::RealtimeGameplayWorker worker(makePracticeDefinition(),
                                           std::move(config));
  require(worker.start(), "bounded practice autoplay starts");
  clock.nowMicros.store(1'000'000, std::memory_order_release);
  require(waitUntil([&] { return audio.commitCount.load() == 1; }),
          "practice autoplay commits its in-range keysound");
  clock.nowMicros.store(1'100'000, std::memory_order_release);
  require(waitUntil([&] {
            auto snapshot = worker.acquireLatestSnapshot();
            return snapshot && snapshot->terminalReason ==
                                   gameplay::GameplayTerminalReason::PracticeComplete;
          }),
          "practice autoplay completes from audio time");
  auto snapshot = worker.acquireLatestSnapshot();
  require(snapshot && snapshot->noteStates[0].played &&
              !snapshot->noteStates[1].played &&
              snapshot->attempt.judgeCounts[PGreat] == 1,
          "practice autoplay resolves only the selected half-open range");
  worker.stop();
}

void testAutoplayCommitsGameplayAndKeysoundWithoutFramePump() {
  FakeClock clock;
  FakeAudio audio;
  auto config = makeConfig(clock, audio);
  config.simulation.attempt.autoPlay = true;
  gameplay::RealtimeGameplayWorker worker(makeRapidDefinition(),
                                           std::move(config));
  require(worker.start(), "autoplay worker starts");

  clock.nowMicros.store(1'000'000, std::memory_order_release);
  require(waitUntil([&] { return audio.commitCount.load() == 1; }),
          "autoplay commits its keysound without a frame pump");
  require(waitUntil([&] {
            auto snapshot = worker.acquireLatestSnapshot();
            return snapshot && snapshot->noteStates[0].played &&
                   snapshot->attempt.judgeCounts[PGreat] == 1;
          }),
          "autoplay commits note and judgement from audio time");

  clock.nowMicros.store(2'000'000, std::memory_order_release);
  require(waitUntil([&] {
            auto snapshot = worker.acquireLatestSnapshot();
            return snapshot && snapshot->terminalReason ==
                                   gameplay::GameplayTerminalReason::ChartComplete;
          }),
          "autoplay completes from audio time without a frame pump");
  worker.stop();
}

void testStoppedWorkerTransfersCompleteGaugeHistory() {
  FakeClock clock;
  FakeAudio audio;
  auto config = makeConfig(clock, audio);
  config.simulation.attempt.initialGaugeType = GaugeType::Hard;
  config.simulation.attempt.gaugeAutoShift = GaugeAutoShiftMode::BestClear;
  gameplay::RealtimeGameplayWorker worker(makeRapidDefinition(),
                                           std::move(config));
  require(worker.start(), "gauge-history fixture starts");
  require(worker.enqueueInput({.epoch = 7,
                               .type = gameplay::RealtimeGameplayInputType::Press,
                               .lane = 1,
                               .compensateLane = 1,
                               .steadyTimestampMicros = 1'000'000}),
          "gauge-history press reaches the worker");
  require(waitUntil([&] { return audio.commitCount.load() == 1; }),
          "gauge-history transaction commits");
  worker.stop();

  const auto history = worker.copyGaugeHistoryAfterStop();
  require(history.size() == 1,
          "stopped authority exposes every committed gauge sample");
  const auto histories = worker.copyGaugeHistoriesAfterStop();
  for (const auto &gaugeHistory : histories) {
    require(gaugeHistory.size() == 1,
            "stopped authority transfers every GAS candidate series");
  }
}

void testLr2MultiBadPublishesEveryTransactionWithOneKeysound() {
  FakeClock clock;
  FakeAudio audio;
  auto config = makeConfig(clock, audio);
  config.simulation.judge = gameplay::CompiledGameplayJudge::from(
      gameplay::compileGameplayJudgeRules(GameplayRuleset::LR2, 2));
  gameplay::RealtimeGameplayWorker worker(makeMultiBadDefinition(),
                                           std::move(config));
  require(worker.start(), "LR2 multi-BAD worker starts");
  require(worker.enqueueInput(
              {.epoch = 7,
               .type = gameplay::RealtimeGameplayInputType::Press,
               .lane = 1,
               .compensateLane = 2,
               .steadyTimestampMicros = 1'000'000}),
          "LR2 compensated press enters fixed ingress");
  require(waitUntil([&] {
            auto snapshot = worker.acquireLatestSnapshot();
            return snapshot && snapshot->transactionSequence >= 2;
          }),
          "worker publishes multi-BAD and selected transactions");
  const auto snapshot = worker.acquireLatestSnapshot();
  require(snapshot && snapshot->transactionCount >= 2 &&
              snapshot->transactions[snapshot->transactionCount - 2]
                      .result.judge.judgement == Bad &&
              snapshot->transactions[snapshot->transactionCount - 2]
                      .result.hasReplayEvent &&
              snapshot->transactions[snapshot->transactionCount - 2]
                      .result.replayEvent.action ==
                  gameplay::GameplayReplayAction::MultiBad &&
              snapshot->transactions[snapshot->transactionCount - 2]
                      .result.soundNoteId == gameplay::kInvalidNoteId &&
              snapshot->transactions[snapshot->transactionCount - 1]
                      .result.judge.judgement == Good &&
              snapshot->transactions[snapshot->transactionCount - 1]
                      .result.hasReplayEvent &&
              snapshot->transactions[snapshot->transactionCount - 1]
                      .result.replayEvent.action ==
                  gameplay::GameplayReplayAction::Press &&
              snapshot->transactions[snapshot->transactionCount - 1]
                      .result.soundNoteId == 1 &&
              audio.reserveCount.load() == 1 &&
              audio.commitCount.load() == 1 &&
              audio.lastCommitted.load() == 1 &&
              worker.fault() == gameplay::RealtimeGameplayFault::None,
          "only the selected LR2 transaction is a press and owns the "
          "reserved keysound");
  worker.stop();
}

void testWorkerSettlesExactTimeMineInputBeforeAutomaticAdvance() {
  for (bool initiallyPressed : {false, true}) {
    bms_parser::Chart chart;
    chart.Meta.KeyMode = 7;
    chart.Meta.TotalNotes = 1;
    auto *measure = new bms_parser::Measure();
    addTimeline(*measure, 1'000'000)->SetLandmineNote(1, new bms_parser::LandmineNote(4.0F));
    addTimeline(*measure, 2'000'000)->SetNote(2, new bms_parser::Note(1));
    chart.Measures.push_back(measure);
    FakeClock clock;
    FakeAudio audio;
    clock.nowMicros.store(1'000'000);
    gameplay::RealtimeGameplayWorker worker(gameplay::buildGameplayDefinition(chart, 0),
                                             makeConfig(clock, audio));
    if (initiallyPressed) {
      require(worker.enqueueInput({.epoch = 7,
          .type = gameplay::RealtimeGameplayInputType::Press,
          .lane = 1, .steadyTimestampMicros = 0}), "initial mine lane press is queued");
    }
    require(worker.enqueueInput({.epoch = 7,
        .type = initiallyPressed ? gameplay::RealtimeGameplayInputType::Release
                                 : gameplay::RealtimeGameplayInputType::Press,
        .lane = 1, .steadyTimestampMicros = 1'000'000}), "boundary input is queued");
    require(worker.start(), "mine boundary worker starts");
    require(waitUntil([&] {
      auto snapshot = worker.acquireLatestSnapshot();
      return snapshot && snapshot->transactionSequence >= (initiallyPressed ? 2 : 1);
    }), "worker processes input after its automatic preadvance");
    {
      const auto snapshot = worker.acquireLatestSnapshot();
      require(snapshot && !snapshot->noteStates[0].dead,
              "worker preadvance leaves exact-time mine pending for input");
    }
    clock.nowMicros.store(1'000'001);
    require(waitUntil([&] {
      auto snapshot = worker.acquireLatestSnapshot();
      return snapshot && snapshot->noteStates[0].dead;
    }), "worker resolves mine when clock passes the boundary");
    {
      const auto snapshot = worker.acquireLatestSnapshot();
      require(snapshot->noteStates[0].played == !initiallyPressed &&
                  snapshot->attempt.gauge == (initiallyPressed ? 20.0F : 16.0F),
              "worker mine damage uses the settled exact-time lane state");
    }
    worker.stop();
  }
}

} // namespace

int main() {
  testWakeTimeoutPreservesConcurrentSignal();
#if ASOBMASHOW_ENABLE_PERF_TELEMETRY
  // Run the source-age check before synthetic timestamps in other fixtures.
  testTouchLatencyPairsOnlySuccessfulTouchSoundCommits();
#endif
  testAndroidDedicatedTouchBypassesBlockedSdlWatch();
  testAndroidSpinScratchExpiresWithoutRenderDrain();
  testCommandOnlyTouchScratchBypassesGameplayAndReplay();
  testAndroidTouchReachesWorkerWithoutRenderDrain();
  testAndroidCancellationReleasesBeforeRenderAndAllowsFingerReuse();
  testAndroidSyntheticMouseDoesNotStealPointerZero();
  testLr2SameKeyBatchUsesLatestEdgeAndRetainsReplayHistory();
  testLr2ScratchBatchKeepsLatestKeyAndProcessingDirection();
  testLr2SimultaneousInputsUseLaneOrderAndOneUpdate();
  testProducerSuspensionProtectsHeldNotesDuringMainThreadStall();
#if ASOBMASHOW_ENABLE_PERF_TELEMETRY
  testWorkerRecordsMeasuredIngressAndSoundStages();
#endif
  testPreparationSnapshotsDoNotVisitUnchangedLargeChart();
  testSparseSnapshotsCatchUpLongNotePairsAcrossSkippedGenerations();
  testSnapshotJournalOverrunResynchronizesCompleteState();
  testNoteJournalBoundaryAndNewReader();
  testPinnedBuffersPublishPendingStateAfterReaderReleases();
  testHeldLongNoteRecoveryDoesNotReserveAnotherKeysound();
  testWorkerSettlesExactTimeMineInputBeforeAutomaticAdvance();
  testWorkerLaunchFailureReleasesAdmission();
  testRapidInputsCommitStateAndSoundWithoutFramePump();
  testRealtimeSnapshotPublishesFlatGaugeSamplesWithoutInput();
  testRealtimeWorkerJudgesPhysicalLanesBeyondLegacyCapacity();
  testWorkerTransfersAcceptedRawReplayInputInOrder();
  testWorkerRetainsAcceptedReplayInputWithInterleavedTimestamps();
  testRealtimeIngressCoalescesTouchAndHardwareLaneOwnership();
  testRealtimeIngressCoalescesTouchAndHardwareScratchOwnership();
  testRealtimeIngressHandsOffOppositeScratchDirectionsWithoutLaneEdges();
  testLegacyBridgeKeepsUnmatchedCallbacksReplayOnly();
  testLegacyBridgeCapturesScratchlessModesAsBrdControls();
  testLegacyAdapterScratchHandoffsValidateAsOneReplayTransaction();
  testLegacyStartSelectCommandsRemainStockReplayInput();
  testReplayCaptureOverflowDoesNotInvalidateGameplay();
  testInputDelayCompensationPrecedesWorkerAutomaticDeadline();
  testInputPreadvancePublishesAutomaticTransactions();
  testSnapshotPublishesHeldLongNoteByLane();
  testAudioCapacityFailureDoesNotClaimTheNote();
  testIngressOverflowFailsClosed();
  testSuspendFreezesAutomaticDeadlinesUntilResume();
  testActivationGateAllowsPreparationFeedbackButNoGameplay();
  testPreparationReleasePublishesOrderedVisualTransaction();
  testPreparationAudioReservationFailureDoesNotClaimLane();
  testPracticeCountInPressJudgesFirstInRangeNote();
  testPracticeCountInPressOutsideJudgeWindowStaysUnjudged();
  testPracticeCompletesFromAudioClockWithoutFramePump();
  testPracticeAutoplayCompletesWithoutFramePump();
  testAutoplayCommitsGameplayAndKeysoundWithoutFramePump();
  testStoppedWorkerTransfersCompleteGaugeHistory();
  testLr2MultiBadPublishesEveryTransactionWithOneKeysound();
  return 0;
}
