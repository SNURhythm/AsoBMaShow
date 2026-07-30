#include "scene/play/RealtimeGameplayWorker.h"

#include "bms_parser.hpp"
#include "input/LogicalGameplayInputAdapter.h"
#include "scene/play/RealtimeGameplayInputBridge.h"
#include "scene/play/GameplayJudgeRules.h"
#include "scene/play/Judge.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace {

using namespace std::chrono_literals;

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
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
  require(worker.fault() == gameplay::RealtimeGameplayFault::None,
          "normal rapid input remains valid");
  worker.stop();
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

void testLegacyBridgeJudgesScratchlessModesWithoutBrdControls() {
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
    require(replayInput.has_value() && replayInput->empty(),
            "scratchless gameplay does not invent stock BRD input");
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
  require(worker.start(), "long-note snapshot worker starts");
  require(worker.enqueueInput(
              {.epoch = 7,
               .type = gameplay::RealtimeGameplayInputType::Press,
               .lane = 7,
               .compensateLane = 7,
               .steadyTimestampMicros = 1'000'000}),
          "scratch long-note head enters fixed ingress");
  require(waitUntil([&] {
            auto snapshot = worker.acquireLatestSnapshot();
            return snapshot && snapshot->longNoteHoldingByLane[7];
          }),
          "worker snapshot publishes held long-note state for scratch routing");
  worker.stop();
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
  require(waitUntil([&] { return audio.commitCount.load() == 1; }),
          "valid early count-in hit commits its keysound");
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

} // namespace

int main() {
  testRapidInputsCommitStateAndSoundWithoutFramePump();
  testWorkerTransfersAcceptedRawReplayInputInOrder();
  testWorkerRetainsAcceptedReplayInputWithInterleavedTimestamps();
  testRealtimeIngressCoalescesTouchAndHardwareLaneOwnership();
  testRealtimeIngressCoalescesTouchAndHardwareScratchOwnership();
  testRealtimeIngressHandsOffOppositeScratchDirectionsWithoutLaneEdges();
  testLegacyBridgeJudgesScratchlessModesWithoutBrdControls();
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
