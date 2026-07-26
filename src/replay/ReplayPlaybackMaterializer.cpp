#include "ReplayPlaybackMaterializer.h"

#include "ReplayPlaybackDriver.h"
#include "../bms_parser.hpp"
#include "../scene/play/GameplayDefinition.h"
#include "../scene/play/GameplayCandidateSelection.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace replay {
namespace {

class SimulationRhythmControl final : public IRhythmControl {
public:
  explicit SimulationRhythmControl(gameplay::GameplaySimulation &simulation)
      : simulation_(simulation) {}

  void setSongTime(std::int64_t songTimeMicros) noexcept {
    songTimeMicros_ = songTimeMicros;
  }

  bms_parser::Note *pressLane(int mainLane, int compensateLane,
                              double inputDelay) override {
    simulation_.applyPressAt(mainLane, compensateLane, context(inputDelay));
    return nullptr;
  }

  bms_parser::Note *pressLane(int lane, double inputDelay) override {
    simulation_.applyPressAt(lane, lane, context(inputDelay));
    return nullptr;
  }

  bms_parser::Note *releaseLane(int lane, double inputDelay,
                                bool isBackSpin) override {
    simulation_.applyReleaseAt(lane, context(inputDelay), isBackSpin);
    return nullptr;
  }

private:
  [[nodiscard]] gameplay::GameplayInputContext
  context(double inputDelay) const noexcept {
    const auto delayMicros = static_cast<std::int64_t>(
        std::llround(std::max(0.0, inputDelay) * 1'000'000.0));
    return {.songTimeMicros = songTimeMicros_,
            .laneBeamTimeMicros = songTimeMicros_,
            .inputDelayMicros = delayMicros};
  }

  gameplay::GameplaySimulation &simulation_;
  std::int64_t songTimeMicros_ = 0;
};

} // namespace

MaterializeOutcome materializeReplay(
    const ReplayPlaybackData &playback, const bms_parser::Chart &chart,
    const gameplay::GameplayRulesetPolicy &policy,
    ReplayMaterializationSeed seed) {
  if (playback.legacy.has_value()) {
    return {.status = MaterializeOutcome::Status::LegacyTrack,
            .diagnostic =
                "Migrated replay tracks use the isolated legacy adapter."};
  }
  if (!std::ranges::is_sorted(
          playback.input, {}, &InputTransition::songTimeMicros)) {
    return {.status = MaterializeOutcome::Status::Invalid,
            .diagnostic = "Replay input timestamps are not ordered."};
  }

  const auto definition =
      gameplay::buildGameplayDefinition(chart, playback.setup.longNoteMode);
  const std::int64_t finalInputTime =
      playback.input.empty() ? 0 : playback.input.back().songTimeMicros;
  const std::int64_t completionBase =
      std::max(definition.metadata().finalTimelineTimeMicros, finalInputTime);
  const std::int64_t automaticPoorLateMicros =
      policy.judge.automaticPoorLateMicros();
  if (automaticPoorLateMicros < 0 ||
      automaticPoorLateMicros >= std::numeric_limits<std::int64_t>::max()) {
    return {.status = MaterializeOutcome::Status::Invalid,
            .diagnostic =
                "Replay completion timing has an invalid late window."};
  }
  const std::int64_t completionTail = automaticPoorLateMicros + 1;
  if (completionBase >
      std::numeric_limits<std::int64_t>::max() - completionTail) {
    return {.status = MaterializeOutcome::Status::Invalid,
            .diagnostic = "Replay completion timestamp is out of range."};
  }
  const std::int64_t completionTime = completionBase + completionTail;
  const std::size_t replayCapacity =
      std::max<std::size_t>(4096, definition.noteCount() * 4 +
                                      playback.input.size() * 2 + 1024);
  const int carriedCombo = std::max(0, seed.carriedCombo);
  const int carriedMaxCombo =
      std::max(carriedCombo, seed.carriedMaxCombo);
  gameplay::GameplaySimulation simulation(
      definition,
      {.judge = policy.judge,
       .gaugeRules = policy.gauge,
       .notePriorityMode =
           notePriorityForCandidateSelection(playback.setup.candidateSelection),
       .attempt =
           {.initialGaugeType = playback.setup.initialGaugeType,
            .gaugeAutoShift = playback.setup.gaugeAutoShift,
            .gaugeProfile = playback.setup.gaugeProfile,
            .gaugeAutoShiftLowerBound =
                playback.setup.gaugeAutoShiftLowerBound,
            .startingGaugePercent = static_cast<int>(
                std::lround(playback.setup.startingGaugePercent)),
            .carriedGauge = seed.carriedGauge.has_value()
                                ? seed.carriedGauge
                                : playback.setup.startingGaugeState,
            .carriedCombo = carriedCombo,
            .carriedMaxCombo = carriedMaxCombo,
            .assistClearMark =
                assist_options::isEnabled(playback.setup.assistOption),
            .autoPlay = false,
            .replayCapacity = replayCapacity,
            .automaticResultCapacity = replayCapacity,
            .gaugeHistoryCapacity = replayCapacity}});
  SimulationRhythmControl control(simulation);
  ReplayPlaybackDriver driver(playback, control);

  std::size_t cursor = 0;
  while (cursor < playback.input.size()) {
    const std::int64_t timestamp = playback.input[cursor].songTimeMicros;
    control.setSongTime(timestamp);
    driver.advanceTo(timestamp);
    while (cursor < playback.input.size() &&
           playback.input[cursor].songTimeMicros == timestamp) {
      ++cursor;
    }
  }

  simulation.advanceTo(completionTime, completionTime);
  if (simulation.replayOverflowed() ||
      simulation.automaticResultOverflowed() ||
      simulation.scoreState().gaugeHistoryOverflowed()) {
    return {.status = MaterializeOutcome::Status::CapacityExceeded,
            .diagnostic = "Replay materialization exceeded its safety bound."};
  }

  const auto judged = simulation.replayEvents();
  return {.status = MaterializeOutcome::Status::Materialized,
          .value = MaterializedReplay{
              .judgedEvents = {judged.begin(), judged.end()},
              .attempt = simulation.snapshot(),
              .gaugeState = simulation.scoreState().gaugeSnapshot(),
              .gaugeHistory = simulation.scoreState().gaugeHistory}};
}

} // namespace replay
