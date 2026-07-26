#include "scene/play/RealtimeGameplayWorker.h"

#include "bms_parser.hpp"
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

void testStoppedWorkerTransfersEveryAcceptedDirectedInput() {
  FakeClock clock;
  FakeAudio audio;
  auto config = makeConfig(clock, audio);
  config.inputTriggeredKeysounds = false;
  config.activationSongTimeMicros = 10'000'000;
  gameplay::RealtimeGameplayWorker worker(makeRapidDefinition(),
                                           std::move(config));
  require(worker.start(), "raw-input fixture starts");
  for (int index = 0; index < 300; ++index) {
    require(worker.enqueueInput(
                {.epoch = 7,
                 .type = index % 2 == 0
                             ? gameplay::RealtimeGameplayInputType::Press
                             : gameplay::RealtimeGameplayInputType::Release,
                 .lane = 7,
                 .compensateLane = 7,
                 .steadyTimestampMicros = 1'000 + index,
                 .replayControl =
                     gameplay::RealtimeLogicalControlKind::ScratchCounterClockwise}),
            "directed raw input enters the worker");
  }
  require(waitUntil([&] {
            auto snapshot = worker.acquireLatestSnapshot();
            return snapshot && snapshot->transactionSequence >= 300;
          }),
          "worker processes every input beyond snapshot history capacity");
  worker.stop();

  const auto replayInput = worker.copyAcceptedReplayInputAfterStop();
  require(replayInput.size() == 300 &&
              replayInput.front().songTimeMicros == 1'000 &&
              replayInput.back().songTimeMicros == 1'299 &&
              replayInput.front().control.kind == replay::LogicalControlKind::
                                                        ScratchCounterClockwise &&
              replayInput.front().pressed && !replayInput.back().pressed,
          "stopped worker retains the full accepted directed input stream");
}

void testReplayOnlyScratchHandoffDoesNotTouchThePhysicalLane() {
  FakeClock clock;
  FakeAudio audio;
  gameplay::RealtimeGameplayWorker worker(makeScratchLongDefinition(),
                                          makeConfig(clock, audio));
  require(worker.start(), "replay-only scratch handoff fixture starts");
  require(waitUntil([&] {
            const auto snapshot = worker.acquireLatestSnapshot();
            return snapshot && snapshot->generation > 0;
          }),
          "replay-only scratch handoff fixture publishes an initial snapshot");
  require(
      worker.enqueueInput(
          {.epoch = 7,
           .type = gameplay::RealtimeGameplayInputType::Release,
           .lane = 7,
           .steadyTimestampMicros = 1'500'000,
           .replayControl =
               gameplay::RealtimeLogicalControlKind::ScratchCounterClockwise,
           .replayOnly = true}) &&
          worker.enqueueInput(
              {.epoch = 7,
               .type = gameplay::RealtimeGameplayInputType::Press,
               .lane = 7,
               .compensateLane = 7,
               .steadyTimestampMicros = 1'500'000,
               .replayControl =
                   gameplay::RealtimeLogicalControlKind::ScratchClockwise,
               .replayOnly = true}),
      "logical-only scratch handoff enters the authority");
  require(waitUntil([&] {
            const auto snapshot = worker.acquireLatestSnapshot();
            return snapshot && snapshot->generation > 1;
          }),
          "logical-only scratch handoff publishes without a frame pump");
  const auto snapshot = worker.acquireLatestSnapshot();
  worker.stop();

  const auto replayInput = worker.copyAcceptedReplayInputAfterStop();
  require(snapshot && !snapshot->lanePressed[7] &&
              snapshot->transactionSequence == 0 &&
              snapshot->attempt.judgeCounts ==
                  std::array<int, JudgementCount>{} &&
              audio.reserveCount.load() == 0 && audio.commitCount.load() == 0 &&
              replayInput.size() == 2 && !replayInput[0].pressed &&
              replayInput[0].replayOnly &&
              replayInput[0].control.kind ==
                  replay::LogicalControlKind::ScratchCounterClockwise &&
              replayInput[1].pressed && replayInput[1].replayOnly &&
              replayInput[1].control.kind ==
                  replay::LogicalControlKind::ScratchClockwise,
          "replay-only handoff is captured without lane, judgement, or audio "
          "side effects");
}

void testReplayInputCapacityFailsClosed() {
  FakeClock clock;
  FakeAudio audio;
  auto config = makeConfig(clock, audio);
  config.inputTriggeredKeysounds = false;
  config.activationSongTimeMicros = 10'000'000;
  config.maximumReplayInputTransitions = 2;
  gameplay::RealtimeGameplayWorker worker(makeRapidDefinition(),
                                          std::move(config));
  for (int index = 0; index < 3; ++index) {
    require(worker.enqueueInput(
                {.epoch = 7,
                 .type = index % 2 == 0
                             ? gameplay::RealtimeGameplayInputType::Press
                             : gameplay::RealtimeGameplayInputType::Release,
                 .lane = 1,
                 .compensateLane = 1,
                 .steadyTimestampMicros = 1'000 + index,
                 .replayOnly = index == 2}),
            "bounded replay input enters ingress before the worker faults");
  }
  require(worker.start(), "bounded replay-input fixture starts");
  require(waitUntil([&] {
            return worker.fault() ==
                   gameplay::RealtimeGameplayFault::ReplayCapacityExceeded;
          }),
          "accepted raw replay input latches a capacity fault at its bound");
  const auto snapshot = worker.acquireLatestSnapshot();
  worker.stop();
  require(worker.copyAcceptedReplayInputAfterStop().size() == 2 && snapshot &&
              !snapshot->lanePressed[1] && snapshot->transactionSequence == 2,
          "the worker never stores or applies replay-only input beyond the "
          "replay transition cap");
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
  testStoppedWorkerTransfersEveryAcceptedDirectedInput();
  testReplayOnlyScratchHandoffDoesNotTouchThePhysicalLane();
  testReplayInputCapacityFailsClosed();
  testLr2MultiBadPublishesEveryTransactionWithOneKeysound();
  return 0;
}
