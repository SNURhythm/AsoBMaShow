#include "scene/play/RealtimeGameplayWorker.h"

#include "bms_parser.hpp"
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

void testActivationGateRejectsPreparationInputAndDeadlines() {
  FakeClock clock;
  FakeAudio audio;
  auto config = makeConfig(clock, audio);
  config.activationSongTimeMicros = 1'000'000;
  gameplay::RealtimeGameplayWorker worker(makeRapidDefinition(),
                                           std::move(config));
  require(worker.start(), "activation-gate fixture starts");
  require(worker.enqueueInput({.epoch = 7,
                               .type = gameplay::RealtimeGameplayInputType::Press,
                               .lane = 1,
                               .compensateLane = 1,
                               .steadyTimestampMicros = 900'000}),
          "preparation press reaches the serial authority");
  clock.nowMicros.store(999'999, std::memory_order_release);
  std::this_thread::sleep_for(10ms);
  {
    auto snapshot = worker.acquireLatestSnapshot();
    require(snapshot && !snapshot->noteStates[0].played &&
                !snapshot->noteStates[0].dead &&
                audio.commitCount.load(std::memory_order_acquire) == 0,
            "preparation input causes no note, judgement, or keysound");
  }

  require(worker.enqueueInput({.epoch = 7,
                               .type = gameplay::RealtimeGameplayInputType::Press,
                               .lane = 1,
                               .compensateLane = 1,
                               .steadyTimestampMicros = 1'000'000}),
          "first active press reaches the serial authority");
  require(waitUntil([&] { return audio.commitCount.load() == 1; }),
          "input becomes live exactly at the activation boundary");
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
  std::this_thread::sleep_for(10ms);
  auto snapshot = worker.acquireLatestSnapshot();
  require(snapshot && !snapshot->noteStates[0].played &&
              snapshot->attempt.judgeCounts[PGreat] == 0 &&
              snapshot->attempt.judgeCounts[Poor] == 0 &&
              audio.commitCount.load() == 0,
          "count-in press outside every judge window stays unjudged");
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
  gameplay::RealtimeGameplayWorker worker(makeRapidDefinition(),
                                           makeConfig(clock, audio));
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
}

} // namespace

int main() {
  testRapidInputsCommitStateAndSoundWithoutFramePump();
  testAudioCapacityFailureDoesNotClaimTheNote();
  testIngressOverflowFailsClosed();
  testSuspendFreezesAutomaticDeadlinesUntilResume();
  testActivationGateRejectsPreparationInputAndDeadlines();
  testPracticeCountInPressJudgesFirstInRangeNote();
  testPracticeCountInPressOutsideJudgeWindowStaysUnjudged();
  testPracticeCompletesFromAudioClockWithoutFramePump();
  testPracticeAutoplayCompletesWithoutFramePump();
  testAutoplayCommitsGameplayAndKeysoundWithoutFramePump();
  testStoppedWorkerTransfersCompleteGaugeHistory();
  return 0;
}
