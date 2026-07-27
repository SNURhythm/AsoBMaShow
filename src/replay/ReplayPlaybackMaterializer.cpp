#include "ReplayPlaybackMaterializer.h"
#include "ReplaySetupAdapter.h"

#include "../AssistOptionUtils.h"
#include "../ReplayData.h"
#include "../bms_parser.hpp"
#include "../scene/play/GameplayCandidateSelection.h"
#include "../scene/play/GameplayDefinition.h"
#include "../scene/play/GameplayRulesetPolicy.h"
#include "../scene/play/GameplaySimulation.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace replay {
namespace {

ReplayEventAction replayAction(gameplay::GameplayReplayAction action) {
  switch (action) {
  case gameplay::GameplayReplayAction::Press:
    return ReplayEventAction::Press;
  case gameplay::GameplayReplayAction::Release:
    return ReplayEventAction::Release;
  case gameplay::GameplayReplayAction::Miss:
    return ReplayEventAction::Miss;
  case gameplay::GameplayReplayAction::Mine:
    return ReplayEventAction::Mine;
  case gameplay::GameplayReplayAction::Gauge:
    return ReplayEventAction::Gauge;
  case gameplay::GameplayReplayAction::MultiBad:
    return ReplayEventAction::MultiBad;
  }
  return ReplayEventAction::Miss;
}

::ReplayTouchAction legacyTouchAction(replay::ReplayTouchAction action) {
  switch (action) {
  case replay::ReplayTouchAction::Down:
    return ::ReplayTouchAction::Down;
  case replay::ReplayTouchAction::Move:
    return ::ReplayTouchAction::Move;
  case replay::ReplayTouchAction::Up:
    return ::ReplayTouchAction::Up;
  case replay::ReplayTouchAction::Cancel:
    return ::ReplayTouchAction::Cancel;
  }
  return ::ReplayTouchAction::Cancel;
}

int judgeCount(const GameplayScoreState &state, Judgement judgement) {
  const auto found = state.judgeCount.find(judgement);
  return found == state.judgeCount.end() ? 0 : found->second;
}

result_persistence::ChartJudgementTiming
judgementTiming(const GameplayScoreState &state) {
  result_persistence::ChartJudgementTiming result;
  for (int index = 0; index < JudgementCount; ++index) {
    const auto found =
        state.judgementFastSlowCount.find(static_cast<Judgement>(index));
    if (found != state.judgementFastSlowCount.end()) {
      result.byJudgement[static_cast<std::size_t>(index)] = found->second;
    }
  }
  return result;
}

std::optional<GameplayRuleset> rulesetFor(const ReplaySetup &setup) {
  return gameplayRulesetFromId(setup.ruleset.id);
}

} // namespace

ReplayPlaybackMaterializationOutcome ReplayPlaybackMaterializer::materialize(
    const ReplayChartDocument &document, ReplaySetupSource source,
    const result_persistence::ModernChartResult &savedResult,
    const ReplayJudgingSink &judge, std::size_t eventBudget) {
  if (!judge.advanceTo || (!judge.applyInput && !judge.applyInputBatch) ||
      !judge.finish) {
    return {.state = ReplayPlaybackMaterializationState::JudgingFailed,
            .diagnostic = "Replay judging sink is incomplete."};
  }

  ReplayPlaybackDriver driver(document, source);
  if (!driver.valid()) {
    return {.state = ReplayPlaybackMaterializationState::InvalidReplay,
            .diagnostic = driver.diagnostic()};
  }

  ReplayPlaybackSink sink;
  sink.inputBatch = [&](std::span<const InputTransition> events,
                        std::string &diagnostic) {
    if (events.empty() ||
        !judge.advanceTo(events.front().songTimeMicros, diagnostic)) {
      return false;
    }
    if (judge.applyInputBatch) {
      if (!judge.applyInputBatch(events, diagnostic)) {
        return false;
      }
    } else {
      for (const auto &event : events) {
        if (!judge.applyInput(event, diagnostic)) {
          return false;
        }
      }
    }
    return true;
  };
  const auto advanced = driver.advanceTo(
      document.timeBounds.completionSongTimeMicros, sink, eventBudget);
  if (advanced.state == ReplayPlaybackDriverState::WorkLimitExceeded) {
    return {.state = ReplayPlaybackMaterializationState::WorkLimitExceeded,
            .diagnostic = advanced.diagnostic};
  }
  if (!advanced.advanced() || !driver.complete()) {
    return {.state = ReplayPlaybackMaterializationState::JudgingFailed,
            .diagnostic = advanced.diagnostic.empty()
                              ? "Replay driver did not reach completion."
                              : advanced.diagnostic};
  }

  std::string diagnostic;
  if (!judge.advanceTo(document.timeBounds.completionSongTimeMicros,
                       diagnostic)) {
    return {.state = ReplayPlaybackMaterializationState::JudgingFailed,
            .diagnostic = std::move(diagnostic)};
  }
  const auto judged = judge.finish(diagnostic);
  if (!judged) {
    return {.state = ReplayPlaybackMaterializationState::JudgingFailed,
            .diagnostic = std::move(diagnostic)};
  }
  const auto agreement =
      result_persistence::compareModernChartResultFacts(savedResult, *judged);
  return {.state = agreement.agrees()
                       ? ReplayPlaybackMaterializationState::Matched
                       : ReplayPlaybackMaterializationState::ResultMismatch,
          .agreement = agreement,
          .judgedResult = *judged,
          .diagnostic = agreement.diagnostic};
}

ReplayPlaybackMaterializationOutcome
ReplayPlaybackMaterializer::materializeForConsumers(
    const ReplayChartDocument &document, ReplaySetupSource source,
    const result_persistence::ModernChartResult &savedResult,
    const bms_parser::Chart &chart, std::size_t eventBudget) {
  return materializeForConsumers(document, source, savedResult, chart,
                                 ReplayPlaybackCarryState{}, eventBudget);
}

ReplayPlaybackMaterializationOutcome
ReplayPlaybackMaterializer::materializeForConsumers(
    const ReplayChartDocument &document, ReplaySetupSource source,
    const result_persistence::ModernChartResult &savedResult,
    const bms_parser::Chart &chart, const ReplayPlaybackCarryState &carry,
    std::size_t eventBudget) {
  const auto &setup = document.playback.setup;
  const auto selectedRuleset = rulesetFor(setup);
  const auto *stage = score_provenance::uniqueStageForChart(
      savedResult.score.provenance, chart.Meta);
  if (!selectedRuleset || stage == nullptr) {
    return {.state = ReplayPlaybackMaterializationState::JudgingFailed,
            .diagnostic = "Replay gameplay policy is unavailable."};
  }
  auto policy = gameplay::buildGameplayRulesetPolicy(
      chart.Meta,
      {.ruleset = *selectedRuleset,
       .gaugeProfile = setup.gaugeProfile,
       .sourceRank = stage->sourceJudgeRank.value_or(chart.Meta.Rank),
       .playbackRatePercent = setup.playback.percent,
       .judgeScalePercent = setup.judgeWindowScalePercent,
       .beatorajaCandidateSelection = setup.candidateSelection,
       .requiredDescriptor = setup.ruleset,
       .replaySnapshot = *stage});
  if (!policy.built()) {
    return {.state = ReplayPlaybackMaterializationState::JudgingFailed,
            .diagnostic = policy.diagnostic.empty()
                              ? "Replay gameplay policy is invalid."
                              : std::move(policy.diagnostic)};
  }

  const auto definition =
      gameplay::buildGameplayDefinition(chart, setup.longNoteMode);
  if (definition.metadata().totalNotes <= 0 ||
      definition.metadata().totalNotes > std::numeric_limits<int>::max() / 2) {
    return {.state = ReplayPlaybackMaterializationState::JudgingFailed,
            .diagnostic = "Replay chart note count is invalid."};
  }
  const std::size_t capacity = std::min<std::size_t>(
      eventBudget,
      std::max<std::size_t>(4096, definition.noteCount() * 4 +
                                      document.playback.input.size() * 2 +
                                      1024));
  gameplay::GameplaySimulation simulation(
      definition,
      {.judge = policy.policy->judge,
       .gaugeRules = policy.policy->gauge,
       .notePriorityMode =
           notePriorityForCandidateSelection(setup.candidateSelection),
       .attempt =
           {.initialGaugeType = setup.initialGaugeType,
            .gaugeAutoShift = setup.gaugeAutoShift,
            .gaugeProfile = setup.gaugeProfile,
            .gaugeAutoShiftLowerBound = setup.gaugeAutoShiftLowerBound,
            .startingGaugePercent = static_cast<int>(
                std::lround(setup.startingGaugePercent)),
            .carriedGauge = carry.gauge,
            .carriedCombo = carry.combo,
            .carriedMaxCombo = carry.maximumCombo,
            .assistClearMark = assist_options::isEnabled(setup.assistOption),
            .autoPlay = false,
            .replayCapacity = capacity,
            .automaticResultCapacity = capacity,
            .gaugeHistoryCapacity = capacity}});
  const GaugeStateSnapshot initialGaugeState =
      simulation.scoreState().gaugeSnapshot();

  std::int64_t currentSongTime = kReplayLimits.minimumSongTimeMicros;
  ReplayLogicalGameplayAdapter adapter(
      setup.chart.keyMode,
      {.pressLane = [&](int physicalLane, double inputDelaySeconds) {
         const auto delay = static_cast<std::int64_t>(
             std::llround(std::max(0.0, inputDelaySeconds) * 1'000'000.0));
         simulation.applyPressAt(
             physicalLane, physicalLane,
             {.songTimeMicros = currentSongTime,
              .laneBeamTimeMicros = currentSongTime,
              .inputDelayMicros = delay});
       },
       .releaseLane = [&](int physicalLane, double inputDelaySeconds,
                          bool backSpin) {
         const auto delay = static_cast<std::int64_t>(
             std::llround(std::max(0.0, inputDelaySeconds) * 1'000'000.0));
         simulation.applyReleaseAt(
             physicalLane,
             {.songTimeMicros = currentSongTime,
              .laneBeamTimeMicros = currentSongTime,
              .inputDelayMicros = delay},
             backSpin);
       }});

  ReplayJudgingSink judge;
  judge.advanceTo = [&](std::int64_t songTimeMicros,
                        std::string &diagnostic) {
    currentSongTime = songTimeMicros;
    simulation.advanceTo(songTimeMicros, songTimeMicros);
    if (simulation.replayOverflowed() ||
        simulation.automaticResultOverflowed() ||
        simulation.scoreState().gaugeHistoryOverflowed()) {
      diagnostic = "Replay judging exceeded its bounded capacity.";
      return false;
    }
    return true;
  };
  judge.applyInputBatch = [&](std::span<const InputTransition> transitions,
                              std::string &diagnostic) {
    return adapter.applyBatch(transitions, currentSongTime, diagnostic);
  };
  judge.finish = [&](std::string &diagnostic)
      -> std::optional<result_persistence::ModernChartResult> {
    if (simulation.replayOverflowed() ||
        simulation.automaticResultOverflowed() ||
        simulation.scoreState().gaugeHistoryOverflowed()) {
      diagnostic = "Replay judging exceeded its bounded capacity.";
      return std::nullopt;
    }
    const auto &state = simulation.scoreState();
    result_persistence::ModernChartResult judged = savedResult;
    judged.score.score = state.getScore();
    judged.score.maxScore = definition.metadata().totalNotes * 2;
    judged.score.maxCombo = state.maxCombo;
    judged.score.comboBreak = state.comboBreak;
    judged.score.pGreat = judgeCount(state, PGreat);
    judged.score.great = judgeCount(state, Great);
    judged.score.good = judgeCount(state, Good);
    judged.score.bad = judgeCount(state, Bad);
    judged.score.poor = judgeCount(state, Poor);
    judged.score.kPoor = judgeCount(state, Kpoor);
    judged.score.fast = state.fastCount;
    judged.score.slow = state.slowCount;
    judged.score.finalGauge = state.currentGauge;
    judged.score.clearType = state.getClearTypeRank();
    judged.adoptedGaugeType = state.gaugeType;
    judged.adoptedGaugeHistory = state.gaugeHistoryFor(state.gaugeType);
    judged.judgementTiming = judgementTiming(state);
    judged.resultFingerprint =
        result_persistence::modernResultFingerprint(judged);
    return judged;
  };

  auto outcome = materialize(document, source, savedResult, judge, eventBudget);
  outcome.initialGaugeState = initialGaugeState;
  outcome.finalGaugeState = simulation.scoreState().gaugeSnapshot();
  outcome.endingCombo = simulation.scoreState().combo;
  if (!outcome.matched() || !outcome.judgedResult.has_value()) {
    return outcome;
  }

  std::string setupDiagnostic;
  auto replayValue = makeReplayDataFromSetup(
      setup, savedResult.score.provenance, chart.Meta, setupDiagnostic);
  if (!replayValue.has_value()) {
    outcome.state = ReplayPlaybackMaterializationState::JudgingFailed;
    outcome.diagnostic = std::move(setupDiagnostic);
    outcome.replayData.reset();
    return outcome;
  }
  ReplayData replay = std::move(*replayValue);
  replay.chartMeta.BmsPath = savedResult.score.chartPath;
  replay.chartMeta.MD5 = savedResult.score.chartMd5;
  replay.chartMeta.SHA256 = savedResult.score.chartSha256;
  replay.chartMeta.Title = savedResult.score.chartTitle;
  replay.chartMeta.Artist = savedResult.score.chartArtist;
  replay.chartMeta.LnMode = savedResult.score.longNoteMode;
  replay.finalScore = savedResult.score.score;
  replay.maxCombo = savedResult.score.maxCombo;
  replay.finalGauge = savedResult.score.finalGauge;
  replay.clearType = savedResult.score.clearType;
  const auto events = simulation.replayEvents();
  replay.events.reserve(events.size());
  for (const auto &event : events) {
    replay.events.push_back({.action = replayAction(event.action),
                             .lane = event.lane,
                             .noteTimeMicros = event.noteTimeMicros,
                             .songTimeMicros = event.songTimeMicros,
                             .judgeTimeMicros = event.judgeTimeMicros,
                             .judgement = event.judgement,
                             .diffMicros = event.diffMicros,
                             .gauge = event.gauge,
                             .gaugeType = event.gaugeType,
                             .combo = event.combo,
                             .score = event.score});
  }
  replay.touchSamples.reserve(document.playback.touchSamples.size());
  for (const auto &sample : document.playback.touchSamples) {
    replay.touchSamples.push_back({.action = legacyTouchAction(sample.action),
                                   .fingerId = sample.fingerId,
                                   .songTimeMicros = sample.songTimeMicros,
                                   .x = sample.x,
                                   .y = sample.y});
  }
  replay.laneCoverEvents.reserve(document.playback.laneCoverEvents.size());
  for (const auto &event : document.playback.laneCoverEvents) {
    replay.laneCoverEvents.push_back(
        {.songTimeMicros = event.songTimeMicros,
         .noteStartPositionPercent = event.noteStartPositionPercent,
         .resetVisibleTimeReference = event.resetVisibleTimeReference});
  }
  outcome.replayData = std::make_shared<ReplayData>(std::move(replay));
  return outcome;
}

} // namespace replay
