#include "bms_parser.hpp"
#include "scene/play/GameplaySimulation.h"
#include <array>
#include <iostream>
#include <map>
#include <memory>

int main() {
  using namespace gameplay;
  int count;
  while (std::cin >> count) {
    int survival;
    std::cin >> survival;
    bms_parser::Chart chart;
    chart.Meta.KeyMode = 7;
    chart.Meta.TotalNotes = 1000;
    chart.Meta.HasTotal = true;
    chart.Meta.Total = 239.9;
    auto *measure = new bms_parser::Measure();
    std::map<std::int64_t, bms_parser::TimeLine *> timelines;
    const auto timeline = [&](std::int64_t time) {
      auto [entry, inserted] = timelines.try_emplace(time);
      if (inserted) {
        entry->second = new bms_parser::TimeLine(8, false);
        entry->second->Timing = time;
      }
      return entry->second;
    };
    for (int index = 0; index < count; ++index) {
      int lane, kind;
      std::int64_t time, tailTime;
      std::cin >> lane >> kind >> time >> tailTime;
      if (kind == 0) timeline(time)->SetNote(lane, new bms_parser::Note(1));
      else if (kind == 4) timeline(time)->SetLandmineNote(lane, new bms_parser::LandmineNote(5));
      else {
        const auto type = static_cast<bms_parser::LongNoteType>(kind);
        auto *head = new bms_parser::LongNote(1, type);
        auto *tail = new bms_parser::LongNote(1, type);
        head->Tail = tail;
        tail->Head = head;
        timeline(time)->SetNote(lane, head);
        timeline(tailTime)->SetNote(lane, tail);
      }
    }
    for (const auto &[time, value] : timelines) measure->TimeLines.push_back(value);
    chart.Measures.push_back(measure);
    const auto definition = buildGameplayDefinition(chart, 0);
    const auto judge = CompiledGameplayJudge::from(compileGameplayJudgeRules(GameplayRuleset::LR2, 2));
    GameplaySimulationConfig config{.judge = judge};
    if (survival) {
      config.attempt.initialGaugeType = GaugeType::Hard;
      config.attempt.startingGaugePercent = 1;
    }
    GameplaySimulation simulation(definition, config);
    std::array<bool, 2> scratchKeyStates{};
    std::array<std::optional<std::int64_t>, 2> scratchChangedTimes{};
    int edges;
    std::cin >> edges;
    for (int index = 0; index < edges; ++index) {
      int action, lane;
      std::int64_t time;
      std::cin >> action >> lane >> time;
      const GameplayInputContext context{.songTimeMicros = time, .laneBeamTimeMicros = time};
      if (action >= 4 && action <= 7) {
        const int key = action <= 5 ? 0 : 1;
        scratchKeyStates[key] = action == 4 || action == 6;
        scratchChangedTimes[key] = time;
        continue;
      }
      if (action == 0 && (scratchChangedTimes[0] || scratchChangedTimes[1])) {
        const std::array states{GameplayLaneInputState{
            7, scratchKeyStates[0] || scratchKeyStates[1]}};
        (void)simulation.beginInputUpdate(states, context);
        for (int key = 0; key < 2; ++key) {
          if (!scratchChangedTimes[key]) continue;
          auto keyContext = context;
          keyContext.inputDelayMicros = time - *scratchChangedTimes[key];
          if (scratchKeyStates[key]) {
            (void)simulation.pressScratchKey(7, key == 0, keyContext);
          } else {
            (void)simulation.releaseScratchKey(7, key == 0, keyContext);
          }
          scratchChangedTimes[key].reset();
        }
        (void)simulation.finishInputUpdate(context);
      } else if (action == 0) (void)simulation.advanceTo(time, time);
      else if (action == 1) (void)simulation.applyPressAt(lane, lane, context);
      else if (action == 2) (void)simulation.applyReleaseAt(lane, context);
      else {
        (void)simulation.applyReleaseAt(lane, context, true);
        (void)simulation.applyPressAt(lane, lane, context);
      }
    }
    const auto snapshot = simulation.snapshot();
    for (auto judgement : {PGreat, Great, Good, Bad, Poor, Kpoor}) std::cout << snapshot.judgeCounts[judgement] << ',';
    int mines = 0, gains = 0, losses = 0;
    for (const auto &event : simulation.replayEvents()) {
      if (event.action == GameplayReplayAction::Mine) ++mines;
      if (event.action == GameplayReplayAction::Gauge && event.judgement == Great) ++gains;
      if (event.action == GameplayReplayAction::Gauge && event.judgement == Bad) ++losses;
    }
    std::cout << snapshot.combo << ',' << mines << ',' << gains << ',' << losses;
    for (auto judgement : {PGreat, Great, Good, Bad, Poor, Kpoor}) {
      const auto &timing = simulation.scoreState().judgementFastSlowCount.at(judgement);
      std::cout << ',' << timing.fast << ',' << timing.slow;
    }
    std::cout << '\n';
  }
}
