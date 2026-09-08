#include "graph_allocation_guard.h"
#include "bms_parser.hpp"
#include "scene/play/GameplayDefinition.h"
#include "scene/play/GameplaySimulation.h"
#include "scene/play/Judge.h"

#include <cstdlib>
#include <iostream>
#include <limits>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

gameplay::GameplayDefinition makeDefinition(long long timingMicros) {
  bms_parser::Chart chart;
  chart.Meta.KeyMode = 7;
  chart.Meta.TotalNotes = 1;
  chart.Meta.Bpm = 120.0;
  auto *measure = new bms_parser::Measure();
  chart.Measures.push_back(measure);
  auto *timeline = new bms_parser::TimeLine(8, false);
  timeline->Timing = timingMicros;
  timeline->Bpm = 120.0;
  timeline->BeatPosition = 4.0;
  timeline->SetNote(1, new bms_parser::Note(1));
  measure->TimeLines.push_back(timeline);
  return gameplay::buildGameplayDefinition(chart, 1);
}

void testSimulation(long long noteMicros, std::size_t capacity, bool distant) {
  const auto definition = makeDefinition(noteMicros);
  const auto judge = gameplay::CompiledGameplayJudge::from(Judge(3));
  gameplay::GameplaySimulation simulation(
      definition, {.judge = judge,
                   .attempt = {.replayCapacity = 16,
                               .automaticResultCapacity = 16,
                               .gaugeHistoryCapacity = capacity}});
  const auto graphNotes = gameplay::makeSkinGameplayGraphNotes(definition);
  require(graphNotes.size() == 1 &&
              graphNotes.front().second == noteMicros / 1'000'000,
          "actual simulation producer must not narrow distant seconds to int");
  (void)simulation.advanceTo(noteMicros, noteMicros);
  const auto beforeGauge = simulation.snapshot().gauge;
  const auto pressed = simulation.pressLane(
      1, {.songTimeMicros = noteMicros, .laneBeamTimeMicros = noteMicros});
  require(pressed.hasJudge && pressed.judge.judgement == PGreat &&
              simulation.snapshot().score == 2 &&
              simulation.snapshot().gauge >= beforeGauge,
          "graph admission must not prevent actual late gameplay judgement");
  const auto &graph = simulation.skinGameplayGraphState();
  require(graph.recentJudgeTimingIndex == 1 &&
              graph.recentJudgeTimingsMillis[1] == 0 &&
              graph.gaugeSupported,
          "late simulation judgement retains timing and live gauge properties");
  if (distant) {
    require(graph.judgementDistribution.empty() &&
                graph.earlyLateDistribution.empty(),
            "actual simulation must omit both overlong distributions");
    for (const auto &history : graph.gaugeHistories) {
      require(history.empty(), "overlong skin graph must not show a prefix");
    }
  } else {
    require(graph.judgementDistribution[1][1] == 1 &&
                graph.earlyLateDistribution[1][1] == 1,
            "ordinary simulation must preserve exact judgement buckets");
  }
  const auto &scoreState = simulation.scoreState();
  require(!scoreState.gaugeHistoryOverflowed() &&
              scoreState.gaugeHistory.size() == 1 &&
              scoreState.gaugeHistory.back() == scoreState.currentGauge &&
              scoreState.gaugeHistoryFor(scoreState.gaugeType).back() ==
                  scoreState.currentGauge &&
              simulation.terminalReason() !=
                  gameplay::GameplayTerminalReason::GaugeHistoryCapacityExceeded,
          "duration admission must preserve event-based durable gauge history");
}

void testEventHistoryReserveAndSemantics() {
  GameplayScoreState state(GameplayScoreConfig{});
  state.configureBoundedGaugeHistory(std::numeric_limits<std::size_t>::max());
  require(state.gaugeHistory.capacity() <= 4096,
          "caller capacity must not trigger unbounded eager score reserve");
  for (const auto &history : state.gaugeHistories) {
    require(history.capacity() <= 4096,
            "every event-history channel must bound its eager reserve");
  }
  for (int index = 0; index < 4100; ++index) {
    state.applyGaugeJudgement(PGreat);
  }
  require(state.gaugeHistory.size() == 4100 &&
              !state.gaugeHistoryOverflowed() &&
              state.gaugeHistoryFor(state.gaugeType).size() == 4100 &&
              state.gaugeHistory.back() == state.currentGauge,
          "reserve admission must not truncate durable event samples at 4096");

  GameplayScoreState small(GameplayScoreConfig{});
  small.configureBoundedGaugeHistory(2);
  small.applyGaugeJudgement(PGreat);
  small.applyGaugeJudgement(PGreat);
  require(!small.gaugeHistoryOverflowed(),
          "explicit small event capacity admits its exact boundary");
  small.applyGaugeJudgement(PGreat);
  require(small.gaugeHistoryOverflowed() && small.gaugeHistory.size() == 2,
          "existing explicit event-capacity overflow contract remains intact");
}

}

int main() {
  try {
    {
      graph_test::AllocationGuard guard;
      testSimulation(1'000'000, 8, false);
    }
    {
      graph_test::AllocationGuard guard;
      testSimulation(100'000'000'000'000LL, 8, true);
    }
    {
      graph_test::AllocationGuard guard;
      testSimulation(2'147'483'664'000'000LL,
                     std::numeric_limits<std::size_t>::max(), true);
    }
    {
      graph_test::AllocationGuard guard;
      testEventHistoryReserveAndSemantics();
    }
  } catch (const std::exception &error) {
    std::cerr << "FAIL graph simulation: " << error.what()
              << "; rejected bytes=" << graph_test::rejectedAllocationBytes << '\n';
    return 1;
  }
}
