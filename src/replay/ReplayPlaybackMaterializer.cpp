#include "ReplayPlaybackMaterializer.h"
#include "ReplaySetupAdapter.h"

#include "../AssistOptionUtils.h"
#include "../ChartTiming.h"
#include "../ReplayData.h"
#include "../ScoreHistoryTime.h"
#include "../ResultContracts.h"
#include "../bms_parser.hpp"
#include "../scene/play/GameplayCandidateSelection.h"
#include "../scene/play/GameplayDefinition.h"
#include "../scene/play/GameplayGaugeTypes.h"
#include "../scene/play/GameplayRulesetPolicy.h"
#include "../scene/play/GameplaySimulation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <tuple>
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
    const ReplayChartDocument &document,
    const result_persistence::ModernChartResult &savedResult,
    const ReplayJudgingSink &judge, std::size_t eventBudget) {
  if (!judge.advanceTo || (!judge.applyInput && !judge.applyInputBatch) ||
      !judge.finish) {
    return {.state = ReplayPlaybackMaterializationState::JudgingFailed,
            .diagnostic = "Replay judging sink is incomplete."};
  }

  ReplayPlaybackDriver driver(document);
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
    const ReplayChartDocument &document,
    const result_persistence::ModernChartResult &savedResult,
    const bms_parser::Chart &chart, std::size_t eventBudget) {
  return materializeForConsumers(document, savedResult, chart,
                                 ReplayPlaybackCarryState{}, eventBudget);
}

ReplayPlaybackMaterializationOutcome
ReplayPlaybackMaterializer::materializeForConsumers(
    const ReplayChartDocument &document,
    const result_persistence::ModernChartResult &savedResult,
    const bms_parser::Chart &chart, const ReplayPlaybackCarryState &carry,
    std::size_t eventBudget) {
  const auto &setup = document.playback.setup;
  const auto selectedRuleset = rulesetFor(setup);
  const auto *stage = score_provenance::uniqueStageForChart(
      savedResult.score.provenance, chart.Meta);
  if (!selectedRuleset || stage == nullptr ||
      !replayRulesetCanBeRejudged(setup.ruleset)) {
    return {.state = ReplayPlaybackMaterializationState::JudgingFailed,
            .diagnostic = "Replay gameplay policy is unavailable."};
  }
  const bool historicalRules = isObsoleteRulesetDescriptor(setup.ruleset);
  const GaugeProfile gaugeProfile =
      carry.courseGaugeProfile.value_or(setup.gaugeProfile);
  auto policy = gameplay::buildGameplayRulesetPolicy(
      chart.Meta,
      {.ruleset = *selectedRuleset,
       .gaugeProfile = gaugeProfile,
       .sourceRank = stage->sourceJudgeRank.value_or(chart.Meta.Rank),
       .playbackRatePercent = setup.playback.percent,
       .judgeScalePercent = setup.judgeWindowScalePercent,
       .courseJudgement = carry.courseJudgement,
       .beatorajaCandidateSelection = setup.candidateSelection,
       .requiredDescriptor = historicalRules
                                 ? RulesetDescriptor::For(*selectedRuleset)
                                 : setup.ruleset,
       .replaySnapshot = historicalRules ? std::nullopt
                                         : std::optional(*stage)});
  if (!policy.built()) {
    return {.state = ReplayPlaybackMaterializationState::JudgingFailed,
            .diagnostic = policy.diagnostic.empty()
                              ? "Replay gameplay policy is invalid."
                              : std::move(policy.diagnostic)};
  }

  const auto definition =
      gameplay::buildGameplayDefinition(chart, setup.longNoteMode);
  const auto maximumScore = result_contract::maximumScoreForNotes(
      definition.metadata().totalNotes);
  if (definition.metadata().totalNotes <= 0 || !maximumScore) {
    return {.state = ReplayPlaybackMaterializationState::JudgingFailed,
            .diagnostic = "Replay chart note count is invalid."};
  }
  const std::size_t capacity = std::min<std::size_t>(
      eventBudget,
      std::max<std::size_t>(4096, definition.noteCount() * 4 +
                                      document.playback.input.size() * 2 +
                                      1024));
  const AssistClearMark assistClearMark =
      clear_policy::assistClearMarkRequired(
          setup.assistOption, chart.Meta.MinBpm, chart.Meta.MaxBpm,
          setup.playback);
  gameplay::GameplaySimulation simulation(
      definition,
      {.judge = policy.policy->judge,
       .gaugeRules = policy.policy->gauge,
       .notePriorityMode =
           notePriorityForCandidateSelection(setup.candidateSelection),
       .attempt =
           {.initialGaugeType = setup.initialGaugeType,
            .gaugeAutoShift = setup.gaugeAutoShift,
            .gaugeProfile = gaugeProfile,
            .gaugeAutoShiftLowerBound = setup.gaugeAutoShiftLowerBound,
            .startingGaugePercent =
                savedResult.score.provenance.startingGaugePercent.has_value()
                    ? std::optional(static_cast<int>(
                          std::lround(setup.startingGaugePercent)))
                    : std::nullopt,
            .carriedGauge = carry.gauge,
            .carriedCombo = carry.combo,
            .carriedMaxCombo = carry.maximumCombo,
            .assistClearMark =
                assistClearMark == AssistClearMark::AssistedEasy,
            .lightAssistClearMark =
                assistClearMark == AssistClearMark::LightAssistedEasy,
            .autoPlay = false,
            .replayCapacity = capacity,
            .automaticResultCapacity = capacity,
            .gaugeHistoryCapacity = capacity}});
  const GaugeStateSnapshot initialGaugeState =
      simulation.scoreState().gaugeSnapshot();

  std::int64_t currentSongTime = kReplayLimits.minimumSongTimeMicros;
  struct GameplayEdge {
    int lane;
    std::int64_t delayMicros;
    bool pressed;
    bool backSpin;
    LogicalControl control;
    std::size_t sequence;
  };
  std::vector<GameplayEdge> edges;
  std::optional<std::int64_t> lastSimulatedTime;
  std::vector<std::pair<std::int64_t, gameplay::NoteId>> classicReleaseDeadlines;
  if (*selectedRuleset == GameplayRuleset::LR2) {
    for (const auto id : definition.chronologicalNotes()) {
      const auto &note = definition.note(id);
      if (note.kind == gameplay::NoteKind::LongTail &&
          note.longNoteRule == gameplay::LongNoteRule::Classic) {
        classicReleaseDeadlines.emplace_back(
            chart_timing::add(note.timingMicros, 1), id);
      }
    }
  }
  std::size_t nextClassicRelease = 0;
  LogicalControl currentControl;
  std::size_t currentSequence = 0;
  std::map<int, std::array<bool, 2>> scratchHeldKeys;
  ReplayLogicalGameplayAdapter adapter(
      setup.chart.keyMode,
      {.pressLane = [&](int physicalLane, double inputDelaySeconds) {
         const auto delay = static_cast<std::int64_t>(
             std::llround(std::max(0.0, inputDelaySeconds) * 1'000'000.0));
         edges.push_back({physicalLane, delay, true, false, currentControl, currentSequence});
       },
       .releaseLane = [&](int physicalLane, double inputDelaySeconds,
                          bool backSpin) {
         const auto delay = static_cast<std::int64_t>(
             std::llround(std::max(0.0, inputDelaySeconds) * 1'000'000.0));
         edges.push_back({physicalLane, delay, false, backSpin, currentControl, currentSequence});
       },
       .beforeTransition = [&](const InputTransition &transition, std::size_t sequence) {
         currentControl = transition.control;
         currentSequence = sequence;
       }});

  ReplayJudgingSink judge;
  judge.advanceTo = [&](std::int64_t songTimeMicros,
                        std::string &diagnostic) {
    // Raw replay input omits the live worker's intervening updates. A held LN
    // must finish after its tail, before a later physical lift can judge it.
    // Preserve input-first ordering when an edge lands exactly on the deadline.
    while (nextClassicRelease < classicReleaseDeadlines.size() &&
           classicReleaseDeadlines[nextClassicRelease].first < songTimeMicros) {
      const auto [deadline, id] = classicReleaseDeadlines[nextClassicRelease++];
      const auto &state = simulation.noteState(id);
      if (state.holding && !state.played) {
        simulation.advanceTo(deadline, deadline);
        lastSimulatedTime = deadline;
      }
    }
    currentSongTime = songTimeMicros;
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
    edges.clear();
    if (!adapter.applyBatch(transitions, currentSongTime, diagnostic)) return false;
    if (*selectedRuleset == GameplayRuleset::LR2) {
      using PhysicalKey = std::tuple<int, int, int>;
      const auto physicalKey = [](const LogicalControl &control) {
        return PhysicalKey{control.player, static_cast<int>(control.kind), control.lane};
      };
      std::map<PhysicalKey, std::size_t> latestSequence;
      for (std::size_t index = 0; index < transitions.size(); ++index) {
        latestSequence[physicalKey(transitions[index].control)] = index;
      }
      std::erase_if(edges, [&](const auto &edge) {
        return isDirectionalScratchControl(edge.control.kind) ||
               latestSequence[physicalKey(edge.control)] != edge.sequence;
      });
      // Scratch ownership validation consumes every raw edge above. Judging
      // samples the latest state of each directional key, including handoffs.
      for (const auto &[key, index] : latestSequence) {
        const auto &transition = transitions[index];
        if (!isDirectionalScratchControl(transition.control.kind)) continue;
        const auto lane = physicalChartLaneForLogicalControl(setup.chart.keyMode,
                                                             transition.control);
        if (!lane) return false;
        const auto direction = transition.control.kind == LogicalControlKind::ScratchClockwise
                                   ? 0U : 1U;
        scratchHeldKeys[*lane][direction] = transition.pressed;
        edges.push_back({*lane, 0, transition.pressed, false, transition.control, index});
      }
    }
    if (*selectedRuleset == GameplayRuleset::Beatoraja) {
      simulation.advanceTo(currentSongTime, currentSongTime);
      for (const auto &edge : edges) {
        const gameplay::GameplayInputContext context{
            .songTimeMicros = currentSongTime, .laneBeamTimeMicros = currentSongTime,
            .inputDelayMicros = edge.delayMicros};
        if (edge.pressed) simulation.applyPressAt(edge.lane, edge.lane, context);
        else simulation.applyReleaseAt(edge.lane, context, edge.backSpin);
      }
      lastSimulatedTime = currentSongTime;
      return !simulation.replayOverflowed() && !simulation.automaticResultOverflowed() &&
             !simulation.scoreState().gaugeHistoryOverflowed();
    }
    std::stable_sort(edges.begin(), edges.end(), [](const auto &left, const auto &right) {
      return std::tuple{left.lane, static_cast<int>(left.control.kind), left.sequence} <
             std::tuple{right.lane, static_cast<int>(right.control.kind), right.sequence};
    });
    std::vector<gameplay::GameplayLaneInputState> physicalStates;
    for (const auto &edge : edges) {
      const bool pressed = isDirectionalScratchControl(edge.control.kind)
          ? scratchHeldKeys[edge.lane][0] || scratchHeldKeys[edge.lane][1]
          : edge.pressed || edge.backSpin;
      if (!physicalStates.empty() && physicalStates.back().lane == edge.lane) {
        physicalStates.back().pressed = pressed;
      } else {
        physicalStates.push_back({edge.lane, pressed});
      }
    }
    const gameplay::GameplayInputContext context{
        .songTimeMicros = currentSongTime, .laneBeamTimeMicros = currentSongTime};
    simulation.beginInputUpdate(physicalStates, context);
    for (const auto &edge : edges) {
      auto edgeContext = context;
      edgeContext.inputDelayMicros = edge.delayMicros;
      if (isDirectionalScratchControl(edge.control.kind)) {
        const bool clockwise = edge.control.kind == LogicalControlKind::ScratchClockwise;
        if (edge.pressed) simulation.pressScratchKey(edge.lane, clockwise, edgeContext);
        else simulation.releaseScratchKey(edge.lane, clockwise, edgeContext);
      } else if (edge.pressed) {
        simulation.pressLane(edge.lane, edge.lane, edgeContext);
      } else {
        simulation.releaseLane(edge.lane, edgeContext, edge.backSpin);
      }
    }
    simulation.finishInputUpdate(context);
    lastSimulatedTime = currentSongTime;
    return !simulation.replayOverflowed() && !simulation.automaticResultOverflowed() &&
           !simulation.scoreState().gaugeHistoryOverflowed();
  };
  std::size_t acceptedReplayEventCount = 0;
  judge.finish = [&](std::string &diagnostic)
      -> std::optional<result_persistence::ModernChartResult> {
    if (!lastSimulatedTime || *lastSimulatedTime != currentSongTime) {
      simulation.advanceTo(currentSongTime, currentSongTime);
    }
    acceptedReplayEventCount = simulation.replayEvents().size();
    if (document.timeBounds.aborted.value_or(false)) {
      simulation.finalizeAbortedAttempt(document.timeBounds.completionSongTimeMicros);
    }
    if (simulation.replayOverflowed() ||
        simulation.automaticResultOverflowed() ||
        simulation.scoreState().gaugeHistoryOverflowed()) {
      diagnostic = "Replay judging exceeded its bounded capacity.";
      return std::nullopt;
    }
    const auto &state = simulation.scoreState();
    result_persistence::ModernChartResult judged = savedResult;
    judged.score.score = state.getScore();
    judged.score.maxScore = *maximumScore;
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
    if (savedResult.score.badPoints) {
      judged.score.badPoints = judged.score.bad + judged.score.poor +
          judged.score.kPoor + *maximumScore / 2 - state.stagePassedNotes;
    }
    judged.score.finalGauge = state.currentGauge;
    judged.score.clearType = state.getClearTypeRank();
    judged.adoptedGaugeType = state.gaugeType;
    judged.adoptedGaugeHistory = state.gaugeHistoryFor(state.gaugeType);
    judged.judgementTiming = judgementTiming(state);
    judged.resultFingerprint =
        result_persistence::modernResultFingerprint(judged);
    return judged;
  };

  auto outcome = materialize(document, savedResult, judge, eventBudget);
  outcome.initialGaugeState = initialGaugeState;
  outcome.finalGaugeState = simulation.scoreState().gaugeSnapshot();
  outcome.endingCombo = simulation.scoreState().combo;
  if (!outcome.judgedResult.has_value()) {
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
  replay.resultAttemptId = savedResult.attemptId;
  replay.resultPassedNotes = simulation.scoreState().stagePassedNotes;
  replay.createdAt = scoreHistoryTime(savedResult.playedAtUnixMillis);
  replay.chartMeta.BmsPath = savedResult.score.chartPath;
  replay.chartMeta.MD5 = savedResult.score.chartMd5;
  replay.chartMeta.SHA256 = savedResult.score.chartSha256;
  replay.chartMeta.Title = savedResult.score.chartTitle;
  replay.chartMeta.Artist = savedResult.score.chartArtist;
  replay.chartMeta.LnMode = setup.longNoteMode;
  replay.staleResult = !outcome.matched();
  replay.finalScore = outcome.judgedResult->score.score;
  replay.maxCombo = outcome.judgedResult->score.maxCombo;
  replay.finalGauge = outcome.judgedResult->score.finalGauge;
  replay.clearType = outcome.judgedResult->score.clearType;
  if (historicalRules) {
    replay.playbackRuleset = policy.policy->descriptor;
    replay.playbackGaugeProfile = policy.policy->gauge.resolvedProfile;
    ScoreStageProvenance runtimePolicy = *stage;
    runtimePolicy.totalNotes = policy.policy->gauge.totalNotes;
    runtimePolicy.authoredGaugeTotal = chart.Meta.HasTotal
                                         ? std::optional(chart.Meta.Total)
                                         : std::nullopt;
    runtimePolicy.effectiveGaugeTotal = policy.policy->gauge.effectiveTotal;
    runtimePolicy.candidateSelection = policy.policy->judge.rules().candidateSelection;
    runtimePolicy.effectiveJudgeRankPercent =
        policy.policy->judge.rules().effectiveJudgeRankPercent;
    runtimePolicy.effectiveJudgeWindows.clear();
    const auto &contexts = policy.policy->judge.rules().contexts;
    for (std::size_t index = 0; index < contexts.size(); ++index) {
      for (const auto &window : contexts[index].windows) {
        runtimePolicy.effectiveJudgeWindows.push_back({
            .context = static_cast<gameplay::JudgeWindowContext>(index),
            .judgement = window.judgement, .earlyMicros = window.earlyMicros,
            .lateMicros = window.lateMicros});
      }
    }
    replay.playbackPolicy = std::move(runtimePolicy);
  }
  if (document.timeBounds.aborted.value_or(false)) {
    replay.abortedAtSongTimeMicros = document.timeBounds.completionSongTimeMicros;
  }
  const auto events = simulation.replayEvents().first(acceptedReplayEventCount);
  replay.events.reserve(events.size());
  for (const auto &event : events) {
    if (event.noteId != gameplay::kInvalidNoteId &&
        !definition.note(event.noteId).inActiveSlot) {
      outcome.consumerIdentityCompatible = false;
      outcome.diagnostic = replay_note::kUnsupportedIdentityDiagnostic;
    }
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
         .laneCoverEnabled = event.laneCoverEnabled,
         .changeKind = event.changeKind,
         .resetVisibleTimeReference = event.resetVisibleTimeReference});
  }
  replay.consumerIdentityCompatible = outcome.consumerIdentityCompatible;
  outcome.replayData = std::make_shared<ReplayData>(std::move(replay));
  return outcome;
}

} // namespace replay
