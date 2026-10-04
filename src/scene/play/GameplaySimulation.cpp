#include "../../bms_parser.hpp"
#include "../../ChartTiming.h"
#include "GameplaySimulation.h"
#include "GameplayNoteJudgeRole.h"
#include "ManualKeysoundSelection.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <tuple>
#include <utility>

namespace gameplay {
namespace {
constexpr std::int64_t kHellChargeGaugeTickMicros = 200'000;

std::int64_t atTimingDeadline(const NoteDefinition &note) noexcept {
  // Resolve mines after every input edge stamped at their authored time.
  return chart_timing::add(note.timingMicros, note.kind == NoteKind::Landmine ? 1 : 0);
}

JudgeResult normalizeReleaseJudge(const JudgeResult &judge) {
  if (judge.judgement == None || judge.judgement == Kpoor ||
      judge.judgement == Poor) {
    return JudgeResult(Bad, judge.Diff);
  }
  return judge;
}

std::int64_t absoluteDistance(std::int64_t value) {
  return value < 0 ? chart_timing::subtract(0, value) : value;
}

int longNoteJudgeSeverity(Judgement judgement) noexcept {
  switch (judgement) {
  case PGreat:
    return 0;
  case Great:
    return 1;
  case Good:
    return 2;
  case Bad:
    return 3;
  case Kpoor:
  case Poor:
    return 4;
  case None:
  case JudgementCount:
    return 5;
  }
  return 5;
}

JudgeResult worseLongNoteJudge(const JudgeResult &head,
                               const JudgeResult &tail) noexcept {
  const int headSeverity = longNoteJudgeSeverity(head.judgement);
  const int tailSeverity = longNoteJudgeSeverity(tail.judgement);
  const Judgement judgement = tailSeverity > headSeverity
                                  ? tail.judgement : head.judgement;
  return JudgeResult(judgement,
                     absoluteDistance(tail.Diff) >= absoluteDistance(head.Diff)
                         ? tail.Diff : head.Diff);
}

GameplaySimulationConfig withCompiledGaugeRules(
    const GameplayDefinition &definition, GameplaySimulationConfig config) {
  if (config.gaugeRules.compiled) {
    return config;
  }
  const auto metadata = definition.metadata();
  bms_parser::ChartMeta meta;
  meta.TotalNotes = metadata.totalNotes;
  meta.KeyMode = metadata.keyMode;
  meta.HasTotal = metadata.hasGaugeTotal;
  meta.Total = metadata.gaugeTotal;
  config.gaugeRules = compileGameplayGaugeRules(
      config.judge.rules().ruleset, meta, config.attempt.gaugeProfile);
  return config;
}

std::array<SkinJudgeWindow, 5>
skinJudgeWindows(const CompiledGameplayJudge &judge) {
  std::array<SkinJudgeWindow, 5> result{};
  const auto &windows =
      judge.rules()
          .contexts[static_cast<std::size_t>(JudgeWindowContext::Normal)]
          .windows;
  for (std::size_t index = 0; index < result.size(); ++index) {
    const auto &window = windows[index];
    result[index] = {
        .judgement = window.judgement,
        .minimumTimingMillis =
            -static_cast<int>(window.lateMicros / 1000),
        .maximumTimingMillis =
            -static_cast<int>(window.earlyMicros / 1000),
    };
  }
  return result;
}
} // namespace

std::vector<SkinGameplayGraphNote>
makeSkinGameplayGraphNotes(const GameplayDefinition &definition) {
  std::vector<SkinGameplayGraphNote> result;
  result.reserve(definition.noteCount());
  for (NoteId id = 0; id < definition.noteCount(); ++id) {
    const auto &note = definition.note(id);
    const bool classicTail = note.kind == NoteKind::LongTail &&
                             note.longNoteRule == LongNoteRule::Classic;
    result.push_back({
        .sourceId = id,
        .second = note.timingMicros / 1'000'000,
        .countsTowardJudgement =
            note.inActiveSlot && note.kind != NoteKind::Landmine && !classicTail,
        .redirectSourceId = classicTail ? note.pairId
                                        : kInvalidSkinGameplayGraphSourceId,
    });
  }
  return result;
}

GameplaySimulation::GameplaySimulation(const GameplayDefinition &definition,
                                       GameplaySimulationConfig config)
    : definition_(definition),
      config_(withCompiledGaugeRules(definition, std::move(config))),
      scoreState_({.gaugeRules = config_.gaugeRules,
                   .keyMode = definition.metadata().keyMode}),
      noteStates_(definition.noteCount()),
      hellChargeBalanceMicros_(definition.noteCount()) {
  replayEvents_.reserve(config_.attempt.replayCapacity);
  automaticResults_.reserve(config_.attempt.automaticResultCapacity);
  inputTransactions_.reserve(definition_.noteCount() + 1);
  pressCandidates_.reserve(definition_.noteCount());
  multiBadSourceIndices_.reserve(definition_.noteCount());
  passingUpdateNotes_.reserve(definition_.noteCount());
  scoreState_.configureBoundedGaugeHistory(
      config_.attempt.gaugeHistoryCapacity);
  scoreState_.configureGauge(
      config_.attempt.initialGaugeType, config_.attempt.gaugeAutoShift,
      config_.attempt.gaugeProfile,
      config_.attempt.gaugeAutoShiftLowerBound);
  if (config_.attempt.startingGaugePercent.has_value()) {
    scoreState_.setStartingGaugePercent(
        *config_.attempt.startingGaugePercent);
  }
  if (config_.attempt.carriedGauge.has_value()) {
    auto carriedGauge = *config_.attempt.carriedGauge;
    carriedGauge.gaugeProfile = scoreState_.gaugeProfile;
    scoreState_.restoreGaugeState(carriedGauge);
  }
  scoreState_.combo = config_.attempt.carriedCombo;
  scoreState_.maxCombo = config_.attempt.carriedMaxCombo;
  scoreState_.setAssistClearMark(
      config_.attempt.assistClearMark
          ? AssistClearMark::AssistedEasy
          : config_.attempt.lightAssistClearMark
                ? AssistClearMark::LightAssistedEasy
                : AssistClearMark::None);
  const std::uint64_t graphSecondCount = skinGameplayGraphSecondCount(
      definition_.metadata().finalTimelineTimeMicros);
  skinGameplayGraph_.reset(
      makeSkinGameplayGraphNotes(definition_), graphSecondCount,
      skinJudgeWindows(config_.judge), config_.attempt.gaugeHistoryCapacity);
  (void)skinGameplayGraph_.updateGaugeState(
      scoreState_.gaugeValues, scoreState_.gaugeType,
      scoreState_.gaugeRules());
  const auto chronological = definition_.chronologicalNotes();
  atTimingNoteIds_.assign(chronological.begin(), chronological.end());
  // JudgeManager finishes a held classic LN via its direct processing pointer,
  // independently of whether the tail still occurs in the lane model.
  for (NoteId id = 0; id < definition_.noteCount(); ++id) {
    const auto &note = definition_.note(id);
    if (!note.inActiveSlot && note.kind == NoteKind::LongTail &&
        note.longNoteRule == LongNoteRule::Classic) atTimingNoteIds_.push_back(id);
  }
  std::ranges::stable_sort(atTimingNoteIds_, {}, [&](NoteId id) {
    return atTimingDeadline(definition_.note(id));
  });
  latePoorNoteIds_.assign(chronological.begin(), chronological.end());
  std::ranges::stable_sort(latePoorNoteIds_, {}, [&](NoteId id) {
    const auto &note = definition_.note(id);
    return chart_timing::add(note.timingMicros, config_.judge.automaticPoorLateMicros(
        note.scratchLane ? NoteJudgeRole::Scratch : NoteJudgeRole::Normal));
  });
  laneStates_.reserve(definition.lanes().size());
  updateInputStates_.reserve(definition.lanes().size());
  for (const auto &lane : definition.lanes()) {
    laneStates_.push_back({.lane = lane.lane});
  }
  if (config_.allowedNoteRange.has_value() &&
      config_.allowedNoteRange->startMicros > 0) {
    initializeAt(config_.allowedNoteRange->startMicros);
  }
}

std::int64_t GameplaySimulation::inputTime(
    const GameplayInputContext &context) const noexcept {
  return chart_timing::subtract(context.songTimeMicros, context.inputDelayMicros);
}

GameplaySimulation::LaneRuntimeState *
GameplaySimulation::findLane(int lane) noexcept {
  const auto found =
      std::ranges::lower_bound(laneStates_, lane, {}, &LaneRuntimeState::lane);
  return found != laneStates_.end() && found->lane == lane ? &*found : nullptr;
}

const GameplaySimulation::LaneRuntimeState *
GameplaySimulation::findLane(int lane) const noexcept {
  const auto found =
      std::ranges::lower_bound(laneStates_, lane, {}, &LaneRuntimeState::lane);
  return found != laneStates_.end() && found->lane == lane ? &*found : nullptr;
}

bool GameplaySimulation::lanePressed(int lane) const noexcept {
  const auto *state = findLane(lane);
  return state != nullptr && state->pressed;
}

GameplaySearchStats GameplaySimulation::lastSearchStats() const noexcept {
  return lastSearchStats_;
}

GameplaySearchStats GameplaySimulation::lastAdvanceStats() const noexcept {
  return lastAdvanceStats_;
}

const GameplayScoreState &GameplaySimulation::scoreState() const noexcept {
  return scoreState_;
}

const SkinGameplayDynamicGraphState &
GameplaySimulation::skinGameplayGraphState() const noexcept {
  return skinGameplayGraph_.state();
}

GameplayAttemptSnapshot GameplaySimulation::snapshot() const noexcept {
  GameplayAttemptSnapshot result;
  for (int index = 0; index < JudgementCount; ++index) {
    const auto judgement = static_cast<Judgement>(index);
    const auto count = scoreState_.judgeCount.find(judgement);
    if (count != scoreState_.judgeCount.end()) {
      result.judgeCounts[index] = count->second;
    }
  }
  result.combo = scoreState_.combo;
  result.maxCombo = scoreState_.maxCombo;
  result.comboBreak = scoreState_.comboBreak;
  result.stageCombo = scoreState_.stageCombo;
  result.stagePassedNotes = scoreState_.stagePassedNotes;
  result.score = scoreState_.getScore();
  result.gauge = scoreState_.currentGauge;
  result.gaugeType = scoreState_.gaugeType;
  result.clearTypeRank = scoreState_.getClearTypeRank();
  return result;
}

std::span<const GameplayReplayEvent>
GameplaySimulation::replayEvents() const noexcept {
  return replayEvents_;
}

bool GameplaySimulation::replayOverflowed() const noexcept {
  return replayOverflowed_;
}

std::span<const GameplayInputResult>
GameplaySimulation::automaticResults() const noexcept {
  return automaticResults_;
}

bool GameplaySimulation::automaticResultOverflowed() const noexcept {
  return automaticResultOverflowed_;
}

std::span<const std::int64_t>
GameplaySimulation::hellChargeBalances() const noexcept {
  return hellChargeBalanceMicros_;
}

GameplayTerminalReason GameplaySimulation::terminalReason() const noexcept {
  return terminalReason_;
}

bool GameplaySimulation::terminal() const noexcept {
  return terminalReason_ != GameplayTerminalReason::None;
}

GameplayAttemptSnapshot
GameplaySimulation::terminalSnapshot() const noexcept {
  return terminalSnapshot_;
}

GameplayFinalSummary GameplaySimulation::finalSummary() const noexcept {
  return finalSummary_;
}

void GameplaySimulation::commitJudge(NoteId id, const JudgeResult &judge) {
  const bool wasSurvivalFailed = scoreState_.activeGaugeFailed();
  const bool wasGaugeHistoryOverflowed =
      scoreState_.gaugeHistoryOverflowed();
  scoreState_.commitJudge(judge, judge.isNotePlayed() && noteStates_[id].played);
  skinGameplayGraph_.applyJudge(id, judge);
  (void)skinGameplayGraph_.updateGaugeState(
      scoreState_.gaugeValues, scoreState_.gaugeType,
      scoreState_.gaugeRules());
  observeGaugeMutation(wasSurvivalFailed, wasGaugeHistoryOverflowed);
}

void GameplaySimulation::applyGaugeDelta(float delta) {
  const bool wasSurvivalFailed = scoreState_.activeGaugeFailed();
  const bool wasGaugeHistoryOverflowed =
      scoreState_.gaugeHistoryOverflowed();
  scoreState_.applyGaugeDelta(delta);
  (void)skinGameplayGraph_.updateGaugeState(
      scoreState_.gaugeValues, scoreState_.gaugeType,
      scoreState_.gaugeRules());
  observeGaugeMutation(wasSurvivalFailed, wasGaugeHistoryOverflowed);
}

void GameplaySimulation::applyGaugeJudgementRate(Judgement judgement,
                                                 float rate) {
  const bool wasSurvivalFailed = scoreState_.activeGaugeFailed();
  const bool wasGaugeHistoryOverflowed =
      scoreState_.gaugeHistoryOverflowed();
  scoreState_.applyGaugeJudgementRate(judgement, rate);
  (void)skinGameplayGraph_.updateGaugeState(
      scoreState_.gaugeValues, scoreState_.gaugeType,
      scoreState_.gaugeRules());
  observeGaugeMutation(wasSurvivalFailed, wasGaugeHistoryOverflowed);
}

void GameplaySimulation::observeGaugeMutation(
    bool wasSurvivalFailed, bool wasGaugeHistoryOverflowed) {
  transactionSurvivalFailed_ =
      transactionSurvivalFailed_ ||
      (!wasSurvivalFailed && scoreState_.activeGaugeFailed());
  transactionGaugeHistoryCapacityExceeded_ =
      transactionGaugeHistoryCapacityExceeded_ ||
      (!wasGaugeHistoryOverflowed && scoreState_.gaugeHistoryOverflowed());
}

bool GameplaySimulation::recordReplay(GameplayReplayEvent &event) {
  event.gauge = scoreState_.currentGauge;
  event.gaugeType = scoreState_.gaugeType;
  event.combo = scoreState_.combo;
  event.score = scoreState_.getScore();
  if (replayEvents_.size() >= config_.attempt.replayCapacity) {
    replayOverflowed_ = true;
    transactionReplayCapacityExceeded_ = true;
    return false;
  }
  replayEvents_.push_back(event);
  return true;
}

bool GameplaySimulation::recordAutomaticResult(
    const GameplayInputResult &result) {
  if (automaticResults_.size() >= config_.attempt.automaticResultCapacity) {
    automaticResultOverflowed_ = true;
    transactionAutomaticResultCapacityExceeded_ = true;
    return false;
  }
  automaticResults_.push_back(result);
  return true;
}

void GameplaySimulation::markIdentityResolved(NoteId id) {
  if (id == kInvalidNoteId || id >= noteStates_.size()) {
    return;
  }
  const auto &state = noteStates_[id];
  if (definition_.note(id).inActiveSlot && !state.played && !state.dead) {
    ++resolvedIdentityCount_;
  }
}

void GameplaySimulation::finishTransaction(
    std::int64_t boundaryTimeMicros) {
  // Reference judgement updates finish all lanes before the player observes a
  // failed survival gauge. Capacity failures still stop the transaction now.
  if (lr2UpdateOpen_ && !transactionGaugeHistoryCapacityExceeded_ &&
      !transactionReplayCapacityExceeded_ &&
      !transactionAutomaticResultCapacityExceeded_) {
    return;
  }
  GameplayTerminalReason reason = GameplayTerminalReason::None;
  if (transactionSurvivalFailed_) {
    reason = GameplayTerminalReason::SurvivalGaugeFailed;
  } else if (transactionGaugeHistoryCapacityExceeded_) {
    reason = GameplayTerminalReason::GaugeHistoryCapacityExceeded;
  } else if (transactionReplayCapacityExceeded_) {
    reason = GameplayTerminalReason::ReplayCapacityExceeded;
  } else if (transactionAutomaticResultCapacityExceeded_) {
    reason = GameplayTerminalReason::AutomaticResultCapacityExceeded;
  }
  transactionSurvivalFailed_ = false;
  transactionGaugeHistoryCapacityExceeded_ = false;
  transactionReplayCapacityExceeded_ = false;
  transactionAutomaticResultCapacityExceeded_ = false;
  latchTerminal(reason, boundaryTimeMicros);
}

void GameplaySimulation::latchTerminal(GameplayTerminalReason reason,
                                       std::int64_t boundaryTimeMicros) {
  if (reason == GameplayTerminalReason::None || terminal()) {
    return;
  }
  if (!hasAdvanced_ || boundaryTimeMicros > lastAdvancedMicros_) {
    lastAdvancedMicros_ = boundaryTimeMicros;
  }
  hasAdvanced_ = true;
  terminalReason_ = reason;
  terminalSnapshot_ = snapshot();
  const int totalNotes = definition_.metadata().totalNotes;
  finalSummary_ = {
      .score = terminalSnapshot_.score,
      .maxCombo = terminalSnapshot_.maxCombo,
      .comboBreak = terminalSnapshot_.comboBreak,
      .totalNotes = totalNotes,
      .finalGauge = terminalSnapshot_.gauge,
      .clearTypeRank = terminalSnapshot_.clearTypeRank,
      .fullComboAchieved = totalNotes > 0 &&
                           terminalSnapshot_.comboBreak == 0 &&
                           terminalSnapshot_.maxCombo >= totalNotes,
  };
}

void GameplaySimulation::maybeLatchChartComplete(
    std::int64_t songTimeMicros) {
  const bool hasBoundedNoteRange =
      config_.allowedNoteRange.has_value() &&
      config_.allowedNoteRange->endMicros !=
          std::numeric_limits<std::int64_t>::max();
  if (terminal() || hasBoundedNoteRange ||
      resolvedIdentityCount_ != definition_.chronologicalNotes().size()) {
    return;
  }
  const std::int64_t completionMicros =
      chart_timing::add(chart_timing::add(definition_.metadata().finalTimelineTimeMicros,
          std::max(config_.judge.automaticPoorLateMicros(NoteJudgeRole::Normal),
                   config_.judge.automaticPoorLateMicros(NoteJudgeRole::Scratch))), 1);
  if (songTimeMicros >= completionMicros) {
    latchTerminal(GameplayTerminalReason::ChartComplete, songTimeMicros);
  }
}

GameplayAdvanceResult GameplaySimulation::emptyAdvanceResult() const noexcept {
  return {std::span<const GameplayInputResult>{}, lastAdvancedMicros_};
}

bool GameplaySimulation::noteAllowed(NoteId id) const noexcept {
  return !config_.allowedNoteRange.has_value() ||
         config_.allowedNoteRange->contains(definition_.note(id).timingMicros);
}

void GameplaySimulation::markMissed(NoteId id, std::int64_t judgeTimeMicros,
                                    bool dead) {
  if (id == kInvalidNoteId || id >= noteStates_.size()) {
    return;
  }
  markIdentityResolved(id);
  auto &state = noteStates_[id];
  noteChanges_.record(id);
  state.played = true;
  state.dead = dead;
  state.playedTimeMicros = judgeTimeMicros;
  state.holding = false;
}

void GameplaySimulation::clearPairHolding(NoteId id) {
  if (id == kInvalidNoteId || id >= noteStates_.size()) {
    return;
  }
  if (noteStates_[id].holding) {
    noteChanges_.record(id);
    noteStates_[id].holding = false;
  }
  const NoteId pairId = definition_.note(id).pairId;
  if (pairId != kInvalidNoteId && pairId < noteStates_.size()) {
    if (noteStates_[pairId].holding) {
      noteChanges_.record(pairId);
      noteStates_[pairId].holding = false;
    }
  }
}

GameplayInputResult
GameplaySimulation::commitMiss(NoteId id, std::int64_t songTimeMicros,
                               std::int64_t judgeTimeMicros) {
  return commitMiss(
      id, songTimeMicros, judgeTimeMicros,
      JudgeResult(Poor, chart_timing::subtract(judgeTimeMicros, definition_.note(id).timingMicros)));
}

GameplayInputResult GameplaySimulation::commitMiss(NoteId id,
                                                   std::int64_t songTimeMicros,
                                                   std::int64_t judgeTimeMicros,
                                                   const JudgeResult &judge) {
  const auto &note = definition_.note(id);
  GameplayInputResult result;
  result.noteId = id;
  result.hasJudge = !(config_.judge.rules().singleMiss &&
      noteStates_[id].hasNonvanishingJudge && judge.judgement == Poor);
  result.judge = judge;
  if (result.hasJudge) {
    commitJudge(id, result.judge);
  } else {
    // PMS consumes the missed identity, but an earlier nonvanishing BAD or
    // empty POOR has already applied its one miss penalty.
    ++scoreState_.stagePassedNotes;
  }
  result.hasReplayEvent = true;
  result.replayEvent = {
      .action = GameplayReplayAction::Miss,
      .noteId = id,
      .lane = note.lane,
      .noteTimeMicros = note.timingMicros,
      .songTimeMicros = songTimeMicros,
      .judgeTimeMicros = judgeTimeMicros,
      .judgement = result.hasJudge ? result.judge.judgement : None,
      .diffMicros = result.judge.Diff,
  };
  recordReplay(result.replayEvent);
  return result;
}

GameplayInputResult GameplaySimulation::commitAutomaticRelease(
    NoteId tailId, std::int64_t songTimeMicros, std::int64_t visualTimeMicros,
    std::optional<JudgeResult> releaseJudge) {
  const auto &tail = definition_.note(tailId);
  auto &tailState = noteStates_[tailId];
  const auto &headState = noteStates_[tail.pairId];
  markIdentityResolved(tailId);
  noteChanges_.record(tailId);
  tailState.played = true;
  tailState.playedTimeMicros = songTimeMicros;
  const std::int64_t releaseTime = releaseJudge
      ? chart_timing::add(tail.timingMicros, releaseJudge->Diff) : songTimeMicros;
  tailState.releaseTimeMicros = releaseTime;
  clearPairHolding(tailId);

  const JudgeResult tailJudge = config_.judge.judgeAt(
      judgeRoleFor(tail), tail.timingMicros, songTimeMicros);
  JudgeResult applied = releaseJudge.value_or(normalizeReleaseJudge(tailJudge));
  if (!releaseJudge && tail.longNoteRule == LongNoteRule::Classic) {
    applied = normalizeReleaseJudge(headState.acceptedHeadJudge);
  }

  GameplayInputResult result;
  result.noteId = tailId;
  tailState.acceptedHeadJudge = applied;
  result.hasJudge = true;
  result.judge = applied;
  commitJudge(tailId, result.judge);
  result.hasReplayEvent = true;
  result.replayEvent = {
      .action = GameplayReplayAction::Release,
      .noteId = tailId,
      .lane = tail.lane,
      .noteTimeMicros = tail.timingMicros,
      .songTimeMicros = songTimeMicros,
      .judgeTimeMicros = releaseTime,
      .judgement = applied.judgement,
      .diffMicros = applied.Diff,
  };
  recordReplay(result.replayEvent);
  if (config_.attempt.autoPlay) {
    result.hasLaneVisual = true;
    result.laneVisual = {LaneVisualAction::Release, tail.lane, songTimeMicros,
                         visualTimeMicros, JudgeResult(None, 0)};
  }
  return result;
}

void GameplaySimulation::initializeAt(std::int64_t startMicros) {
  const auto chronological = definition_.chronologicalNotes();
  for (const NoteId id : chronological) {
    const auto &note = definition_.note(id);
    if (note.timingMicros >= startMicros) {
      break;
    }
    markMissed(id, startMicros, true);
    if ((note.kind == NoteKind::LongHead || note.kind == NoteKind::LongTail) &&
        note.pairId != kInvalidNoteId) {
      markMissed(note.pairId, startMicros, true);
    }
  }
  while (atTimingCursor_ < atTimingNoteIds_.size() &&
         definition_.note(atTimingNoteIds_[atTimingCursor_]).timingMicros <
             startMicros) {
    ++atTimingCursor_;
  }
  while (latePoorCursor_ < latePoorNoteIds_.size() &&
         noteStates_[latePoorNoteIds_[latePoorCursor_]].played) {
    ++latePoorCursor_;
  }
  lastAdvancedMicros_ = startMicros;
  hasAdvanced_ = true;
}

void GameplaySimulation::processAtTiming(NoteId id, std::int64_t songTimeMicros,
                                         std::int64_t visualTimeMicros) {
  if (terminal()) {
    return;
  }
  const auto &note = definition_.note(id);
  auto &state = noteStates_[id];
  if (state.played || state.dead || !noteAllowed(id)) {
    return;
  }

  if (note.kind == NoteKind::Landmine) {
    markIdentityResolved(id);
    noteChanges_.record(id);
    state.dead = true;
    state.playedTimeMicros = songTimeMicros;
    if (!lanePressedAtUpdate(note.lane)) {
      return;
    }

    state.played = true;
    applyGaugeDelta(-note.mineDamage);
    GameplayInputResult result;
    result.noteId = id;
    result.hasReplayEvent = true;
    result.replayEvent = {
        .action = GameplayReplayAction::Mine,
        .noteId = id,
        .lane = note.lane,
        .noteTimeMicros = note.timingMicros,
        .songTimeMicros = songTimeMicros,
        .judgeTimeMicros = songTimeMicros,
    };
    recordReplay(result.replayEvent);
    recordAutomaticResult(result);
    finishTransaction(songTimeMicros);
    return;
  }

  if (note.kind == NoteKind::LongTail) {
    const bool autoChargeTail = config_.attempt.autoPlay && note.inActiveSlot &&
                                note.longNoteRule != LongNoteRule::Classic;
    if ((!state.holding && !autoChargeTail) || note.pairId == kInvalidNoteId ||
        (note.longNoteRule != LongNoteRule::Classic &&
         !config_.attempt.autoPlay)) {
      return;
    }
    recordAutomaticResult(
        commitAutomaticRelease(id, songTimeMicros, visualTimeMicros));
    finishTransaction(songTimeMicros);
    return;
  }

  if ((note.kind != NoteKind::Normal && note.kind != NoteKind::LongHead) ||
      !config_.attempt.autoPlay) {
    return;
  }

  markIdentityResolved(id);
  noteChanges_.record(id);
  state.played = true;
  state.playedTimeMicros = songTimeMicros;
  if (note.kind == NoteKind::LongHead) {
    state.acceptedHeadJudge = JudgeResult(PGreat, 0);
    state.holding = true;
    if (note.pairId != kInvalidNoteId) {
      noteChanges_.record(note.pairId);
      noteStates_[note.pairId].holding = true;
      if (auto *lane = findLane(note.lane)) {
        lane->heldTailId = note.pairId;
      }
    }
  }
  GameplayInputResult press;
  press.noteId = id;
  press.soundNoteId = id;
  press.hasJudge = note.kind == NoteKind::Normal ||
                   note.longNoteRule != LongNoteRule::Classic;
  press.judge = JudgeResult(PGreat, 0);
  press.hasLaneVisual = true;
  press.laneVisual = {LaneVisualAction::Press, note.lane, songTimeMicros,
                      visualTimeMicros, press.judge};
  if (press.hasJudge) {
    commitJudge(id, press.judge);
  }
  press.hasReplayEvent = true;
  press.replayEvent = {
      .action = GameplayReplayAction::Press,
      .noteId = id,
      .lane = note.lane,
      .noteTimeMicros = note.timingMicros,
      .songTimeMicros = songTimeMicros,
      .judgeTimeMicros = songTimeMicros,
      .judgement = press.judge.judgement,
      .diffMicros = press.judge.Diff,
  };
  recordReplay(press.replayEvent);
  recordAutomaticResult(press);
  finishTransaction(songTimeMicros);

  if (terminal() || note.kind == NoteKind::LongHead) {
    return;
  }
  GameplayInputResult release;
  release.hasLaneVisual = true;
  release.laneVisual = {LaneVisualAction::Release, note.lane, songTimeMicros,
                        visualTimeMicros, JudgeResult(None, 0)};
  recordAutomaticResult(release);
  finishTransaction(songTimeMicros);
}

void GameplaySimulation::processLatePoor(NoteId id,
                                         std::int64_t songTimeMicros) {
  if (terminal()) {
    return;
  }
  const auto &note = definition_.note(id);
  auto &state = noteStates_[id];
  if (state.played || state.dead || !noteAllowed(id) ||
      note.kind == NoteKind::Landmine) {
    return;
  }

  if (note.kind == NoteKind::Normal) {
    markMissed(id, songTimeMicros, true);
    recordAutomaticResult(commitMiss(id, songTimeMicros, songTimeMicros));
    finishTransaction(songTimeMicros);
    return;
  }

  if (note.kind == NoteKind::LongHead) {
    markMissed(id, songTimeMicros, true);
    clearPairHolding(id);
    const auto headMiss = commitMiss(id, songTimeMicros, songTimeMicros);
    recordAutomaticResult(headMiss);
    if (note.pairId == kInvalidNoteId || !noteAllowed(note.pairId) ||
        noteStates_[note.pairId].played) {
      finishTransaction(songTimeMicros);
      return;
    }
    if (note.longNoteRule == LongNoteRule::Classic) {
      markMissed(note.pairId, songTimeMicros, false);
      finishTransaction(songTimeMicros);
      return;
    }
    // A missed CN/HCN head and its tail are one judgement operation. Commit
    // both before survival failure can end the attempt.
    const bool tailDead =
        songTimeMicros >= definition_.note(note.pairId).timingMicros;
    markMissed(note.pairId, songTimeMicros, tailDead);
    recordAutomaticResult(commitMiss(note.pairId, songTimeMicros,
                                     songTimeMicros, headMiss.judge));
    finishTransaction(songTimeMicros);
    return;
  }

  markMissed(id, songTimeMicros, true);
  clearPairHolding(id);
  // Java's late-POOR lane scan never judges a classic LN end on its own.
  // This matters when its head was displaced from the active lane slots.
  if (note.kind == NoteKind::LongTail && note.longNoteRule == LongNoteRule::Classic) {
    finishTransaction(songTimeMicros);
    return;
  }
  recordAutomaticResult(commitMiss(id, songTimeMicros, songTimeMicros));
  finishTransaction(songTimeMicros);
}

bool GameplaySimulation::hellChargeActiveAt(
    NoteId headId, std::int64_t timeMicros) const {
  if (!noteAllowed(headId) || !noteStates_[headId].played) {
    return false;
  }
  const auto &head = definition_.note(headId);
  if (head.pairId == kInvalidNoteId || head.pairId >= noteStates_.size()) {
    return false;
  }
  const auto &tail = definition_.note(head.pairId);
  if (tail.timingMicros <= head.timingMicros ||
      timeMicros < head.timingMicros || timeMicros >= tail.timingMicros) {
    return false;
  }
  const auto &tailState = noteStates_[head.pairId];
  const bool tailJudgedBeforeTiming =
      tailState.played && tailState.playedTimeMicros < tail.timingMicros;
  return !tailState.dead || tailJudgedBeforeTiming;
}

void GameplaySimulation::commitGaugeTick(Judgement judgement,
                                          std::int64_t songTimeMicros) {
  applyGaugeJudgementRate(judgement, 0.5F);
  GameplayInputResult result;
  result.judge = JudgeResult(judgement, 0);
  result.hasReplayEvent = true;
  result.replayEvent = {
      .action = GameplayReplayAction::Gauge,
      .noteId = kInvalidNoteId,
      .lane = -1,
      .noteTimeMicros = -1,
      .songTimeMicros = songTimeMicros,
      .judgeTimeMicros = songTimeMicros,
      .judgement = judgement,
      .diffMicros = 0,
  };
  recordReplay(result.replayEvent);
  recordAutomaticResult(result);
  finishTransaction(songTimeMicros);
}

void GameplaySimulation::integrateHellChargeInterval(
    std::int64_t fromMicros, std::int64_t toMicros) {
  if (terminal() || toMicros <= fromMicros) {
    return;
  }

  const auto heads = definition_.hellChargeHeads();
  for (const NoteId headId : heads) {
    if (!hellChargeActiveAt(headId, fromMicros)) {
      hellChargeBalanceMicros_[headId] = 0;
    }
  }

  std::int64_t currentMicros = fromMicros;
  while (currentMicros < toMicros) {
    std::int64_t nextCrossingMicros =
        std::numeric_limits<std::int64_t>::max();
    for (const NoteId headId : heads) {
      if (!hellChargeActiveAt(headId, currentMicros)) {
        continue;
      }
      const auto &head = definition_.note(headId);
      const auto &tailState = noteStates_[head.pairId];
      const bool completedSuccessfully = tailState.played &&
          (tailState.acceptedHeadJudge.judgement == PGreat ||
           tailState.acceptedHeadJudge.judgement == Great ||
           tailState.acceptedHeadJudge.judgement == Good);
      const bool gaining = completedSuccessfully || lanePressed(head.lane) ||
                           config_.attempt.autoPlay;
      const std::int64_t balance = hellChargeBalanceMicros_[headId];
      const std::int64_t untilCrossing =
          gaining ? kHellChargeGaugeTickMicros + 1 - balance
                  : balance + kHellChargeGaugeTickMicros + 1;
      if (untilCrossing <= chart_timing::subtract(toMicros, currentMicros)) {
        nextCrossingMicros =
            std::min<std::int64_t>(nextCrossingMicros,
                                   chart_timing::add(currentMicros, untilCrossing));
      }
    }

    const std::int64_t intervalEnd =
        std::min(toMicros, nextCrossingMicros);
    const std::int64_t activeDelta = chart_timing::subtract(intervalEnd, currentMicros);
    for (const NoteId headId : heads) {
      if (!hellChargeActiveAt(headId, currentMicros)) {
        continue;
      }
      const auto &head = definition_.note(headId);
      const auto &tailState = noteStates_[head.pairId];
      const bool completedSuccessfully = tailState.played &&
          (tailState.acceptedHeadJudge.judgement == PGreat ||
           tailState.acceptedHeadJudge.judgement == Great ||
           tailState.acceptedHeadJudge.judgement == Good);
      const bool gaining = completedSuccessfully || lanePressed(head.lane) ||
                           config_.attempt.autoPlay;
      hellChargeBalanceMicros_[headId] +=
          gaining ? activeDelta : -activeDelta;
    }
    currentMicros = intervalEnd;
    (void)skinGameplayGraph_.advanceGaugeHistoryTo(currentMicros);

    while (true) {
      NoteId nextHeadId = kInvalidNoteId;
      Judgement nextJudgement = None;
      for (const NoteId headId : heads) {
        const std::int64_t balance = hellChargeBalanceMicros_[headId];
        const Judgement judgement =
            balance > kHellChargeGaugeTickMicros
                ? Great
                : balance < -kHellChargeGaugeTickMicros ? Bad : None;
        if (judgement != None &&
            (nextHeadId == kInvalidNoteId || headId < nextHeadId)) {
          nextHeadId = headId;
          nextJudgement = judgement;
        }
      }
      if (nextHeadId == kInvalidNoteId) {
        break;
      }
      if (nextJudgement == Great) {
        hellChargeBalanceMicros_[nextHeadId] -=
            kHellChargeGaugeTickMicros;
      } else {
        hellChargeBalanceMicros_[nextHeadId] +=
            kHellChargeGaugeTickMicros;
      }
      commitGaugeTick(nextJudgement, currentMicros);
      if (terminal()) {
        return;
      }
    }
    if (nextCrossingMicros > toMicros) {
      break;
    }
  }
}

bool GameplaySimulation::lanePressedAtUpdate(int lane) const noexcept {
  if (lr2UpdateOpen_) {
    const auto found = std::ranges::find(updateInputStates_, lane,
                                         &GameplayLaneInputState::lane);
    if (found != updateInputStates_.end()) return found->pressed;
  }
  return lanePressed(lane);
}

void GameplaySimulation::beginLr2Update(std::int64_t songTimeMicros,
                                       std::int64_t visualTimeMicros,
                                       std::span<const GameplayLaneInputState> states) {
  lr2UpdateOpen_ = true;
  updateInputStates_.assign(states.begin(), states.end());
  passingUpdateNotes_.clear();
  while (atTimingCursor_ < atTimingNoteIds_.size() &&
         definition_.note(atTimingNoteIds_[atTimingCursor_]).timingMicros <=
             songTimeMicros) {
    const NoteId id = atTimingNoteIds_[atTimingCursor_++];
    // JudgeManager's passing interval excludes its previous sample, initially
    // zero. Input can still judge a head at zero without activating its HCN.
    const auto &note = definition_.note(id);
    if (note.timingMicros > lastAdvancedMicros_) {
      passingUpdateNotes_.push_back(id);
    } else if (note.kind == NoteKind::Landmine && note.inActiveSlot &&
               noteAllowed(id) && !noteStates_[id].played && !noteStates_[id].dead) {
      // Ignored mines still leave the native identity graph at their passage;
      // they never produce a damage judgement or hold completion pending.
      markIdentityResolved(id);
      noteChanges_.record(id);
      noteStates_[id].dead = true;
      noteStates_[id].playedTimeMicros = songTimeMicros;
    }
  }
  // JudgeManager's passing phase visits complete lanes in lane order, not a
  // merged chart-time stream. This order matters for clipping gauge changes.
  std::ranges::sort(passingUpdateNotes_, {}, [&](NoteId id) {
    const auto &note = definition_.note(id);
    return std::tuple{note.lane, note.timingMicros, id};
  });
  for (const NoteId id : passingUpdateNotes_) {
    const auto &note = definition_.note(id);
    ++lastAdvanceStats_.notesExamined;
    if (!note.inActiveSlot || !noteAllowed(id)) continue;
    if (note.longNoteRule == LongNoteRule::HellCharge) {
      if (auto *lane = findLane(note.lane)) {
        if (note.kind == NoteKind::LongHead) {
          lane->passingHellChargeHeadId = id;
        } else if (note.kind == NoteKind::LongTail) {
          if (lane->passingHellChargeHeadId != kInvalidNoteId) {
            hellChargeBalanceMicros_[lane->passingHellChargeHeadId] = 0;
          }
          lane->passingHellChargeHeadId = kInvalidNoteId;
          lane->hellChargeBalanceMicros = 0;
        }
      }
    }
    // Classic LN completion is after input and strictly after its tail time.
    if (note.kind != NoteKind::LongTail ||
        note.longNoteRule != LongNoteRule::Classic) {
      processAtTiming(id, songTimeMicros, visualTimeMicros);
    }
    if (terminal()) return;
  }
  const auto delta = chart_timing::subtract(songTimeMicros, lastAdvancedMicros_);
  for (auto &lane : laneStates_) {
    const NoteId id = lane.passingHellChargeHeadId;
    if (id == kInvalidNoteId || !noteStates_[id].played) continue;
    const auto &head = definition_.note(id);
    const bool completed = head.pairId != kInvalidNoteId &&
        noteStates_[head.pairId].played &&
        (noteStates_[head.pairId].acceptedHeadJudge.judgement == PGreat ||
         noteStates_[head.pairId].acceptedHeadJudge.judgement == Great ||
         noteStates_[head.pairId].acceptedHeadJudge.judgement == Good);
    const bool gaining = lanePressedAtUpdate(lane.lane) || completed ||
                         config_.attempt.autoPlay;
    lane.hellChargeBalanceMicros = gaining
        ? chart_timing::add(lane.hellChargeBalanceMicros, delta)
        : chart_timing::subtract(lane.hellChargeBalanceMicros, delta);
    // Exactly one tick per update, including a zero-time update draining an
    // existing excess. A change of direction tests only that direction's edge.
    if (gaining && lane.hellChargeBalanceMicros > kHellChargeGaugeTickMicros) {
      lane.hellChargeBalanceMicros -= kHellChargeGaugeTickMicros;
      commitGaugeTick(Great, songTimeMicros);
    } else if (!gaining &&
               lane.hellChargeBalanceMicros < -kHellChargeGaugeTickMicros) {
      lane.hellChargeBalanceMicros += kHellChargeGaugeTickMicros;
      commitGaugeTick(Bad, songTimeMicros);
    }
    hellChargeBalanceMicros_[id] = lane.hellChargeBalanceMicros;
    if (terminal()) return;
  }
  lastAdvancedMicros_ = songTimeMicros;
  hasAdvanced_ = true;
}

void GameplaySimulation::finishLr2Update(std::int64_t songTimeMicros,
                                        std::int64_t visualTimeMicros) {
  for (auto &lane : laneStates_) {
    if (terminal()) break;
    const NoteId tailId = lane.heldTailId;
    if (lane.pendingReleaseTailId != kInvalidNoteId &&
        lane.pendingReleaseDeadline <= songTimeMicros &&
        !noteStates_[lane.pendingReleaseTailId].played) {
      const auto pendingTail = lane.pendingReleaseTailId;
      const auto pendingJudge = lane.pendingReleaseJudge;
      lane.pendingReleaseTailId = kInvalidNoteId;
      recordAutomaticResult(commitAutomaticRelease(pendingTail, songTimeMicros,
                                                    visualTimeMicros, pendingJudge));
      finishTransaction(songTimeMicros);
    } else if (tailId != kInvalidNoteId && !noteStates_[tailId].played &&
        definition_.note(tailId).longNoteRule == LongNoteRule::Classic &&
        definition_.note(tailId).timingMicros < songTimeMicros) {
      recordAutomaticResult(commitAutomaticRelease(tailId, songTimeMicros,
                                                    visualTimeMicros));
      finishTransaction(songTimeMicros);
    }
    const auto ids = definition_.laneNotes(lane.lane);
    while (lane.missCursor < ids.size()) {
      const NoteId id = ids[lane.missCursor];
      const auto &note = definition_.note(id);
      const auto edge = config_.judge.automaticPoorLateMicros(
          note.scratchLane ? NoteJudgeRole::Scratch : NoteJudgeRole::Normal);
      if (songTimeMicros <= chart_timing::add(note.timingMicros, edge)) break;
      ++lane.missCursor;
      ++lastAdvanceStats_.notesExamined;
      processLatePoor(id, songTimeMicros);
      if (terminal()) break;
    }
  }
  lr2UpdateOpen_ = false;
  updateInputStates_.clear();
  finishTransaction(songTimeMicros);
  maybeLatchChartComplete(songTimeMicros);
}

GameplayAdvanceResult GameplaySimulation::beginInputUpdate(
    int lane, bool pressed, const GameplayInputContext &context) {
  // Directional adapters emit a backspin release and same-time continuation,
  // while JudgeManager consumes the opposite-key press in a single update.
  const auto *laneState = findLane(lane);
  if (config_.judge.rules().ruleset == GameplayRuleset::LR2 && pressed &&
      laneState != nullptr &&
      laneState->suppressedBackspinPressMicros == inputTime(context)) {
    return emptyAdvanceResult();
  }
  const std::array states{GameplayLaneInputState{lane, pressed}};
  return beginInputUpdate(states, context);
}

GameplayAdvanceResult GameplaySimulation::beginInputUpdate(
    std::span<const GameplayLaneInputState> states, const GameplayInputContext &context) {
  const auto time = inputTime(context);
  if (config_.judge.rules().ruleset != GameplayRuleset::LR2) {
    return advanceTo(time, context.laneBeamTimeMicros);
  }
  if (terminal() || (hasAdvanced_ && time < lastAdvancedMicros_)) {
    return emptyAdvanceResult();
  }
  automaticResults_.clear();
  lastAdvanceStats_ = {};
  beginLr2Update(time, context.laneBeamTimeMicros, states);
  return {automaticResults_, lastAdvancedMicros_};
}

GameplayAdvanceResult GameplaySimulation::finishInputUpdate(
    const GameplayInputContext &context) {
  if (!lr2UpdateOpen_) return emptyAdvanceResult();
  automaticResults_.clear();
  finishLr2Update(inputTime(context), context.laneBeamTimeMicros);
  return {automaticResults_, lastAdvancedMicros_};
}

GameplayAdvanceResult
GameplaySimulation::finalizePracticeRange(std::int64_t finalizationTimeMicros,
                                          std::int64_t visualTimeMicros) {
  (void)visualTimeMicros;
  if (terminal()) {
    return emptyAdvanceResult();
  }
  automaticResults_.clear();
  lastAdvanceStats_ = {};
  if (practiceRangeFinalized_) {
    return {automaticResults_, lastAdvancedMicros_};
  }
  practiceRangeFinalized_ = true;
  if (!config_.allowedNoteRange.has_value()) {
    return {automaticResults_, lastAdvancedMicros_};
  }

  return finalizePendingNotes(*config_.allowedNoteRange,
                               finalizationTimeMicros, false);
}

GameplayAdvanceResult GameplaySimulation::finalizeAbortedAttempt(
    std::int64_t finalizationTimeMicros) {
  if (terminal() && terminalReason_ != GameplayTerminalReason::ChartComplete &&
      terminalReason_ != GameplayTerminalReason::SurvivalGaugeFailed) {
    return emptyAdvanceResult();
  }
  terminalReason_ = GameplayTerminalReason::None;
  automaticResults_.clear();
  return finalizePendingNotes(
      {.startMicros = std::numeric_limits<std::int64_t>::min(),
       .endMicros = std::numeric_limits<std::int64_t>::max()},
      finalizationTimeMicros, true);
}

GameplayAdvanceResult GameplaySimulation::finalizePendingNotes(
    const GameplayTimeRange &range, std::int64_t finalizationTimeMicros,
    bool aborted) {
  const auto finishFinalizationTransaction = [&] {
    if (aborted) {
      transactionSurvivalFailed_ = false;
    }
    finishTransaction(finalizationTimeMicros);
  };
  for (const NoteId id : definition_.chronologicalNotes()) {
    const auto &note = definition_.note(id);
    if (!range.contains(note.timingMicros) || note.kind == NoteKind::Landmine) {
      continue;
    }
    auto &state = noteStates_[id];
    if (note.kind == NoteKind::LongHead && state.played &&
        note.pairId != kInvalidNoteId && !noteStates_[note.pairId].played &&
        !range.contains(definition_.note(note.pairId).timingMicros)) {
      clearPairHolding(id);
      if (note.longNoteRule == LongNoteRule::Classic) {
        continue;
      }
    }
    if (state.played) {
      continue;
    }

    if (note.kind == NoteKind::Normal ||
        note.longNoteRule != LongNoteRule::Classic) {
      markMissed(id, finalizationTimeMicros, true);
      clearPairHolding(id);
      recordAutomaticResult(
          commitMiss(id, finalizationTimeMicros, finalizationTimeMicros));
      finishFinalizationTransaction();
      if (terminal()) {
        return {automaticResults_, lastAdvancedMicros_};
      }
      continue;
    }

    if (note.kind == NoteKind::LongHead) {
      markMissed(id, finalizationTimeMicros, true);
      clearPairHolding(id);
      if (note.pairId != kInvalidNoteId &&
          range.contains(definition_.note(note.pairId).timingMicros) &&
          !noteStates_[note.pairId].played) {
        markMissed(note.pairId, finalizationTimeMicros, false);
      }
      recordAutomaticResult(
          commitMiss(id, finalizationTimeMicros, finalizationTimeMicros));
      finishFinalizationTransaction();
      if (terminal()) {
        return {automaticResults_, lastAdvancedMicros_};
      }
      continue;
    }

    if (note.pairId != kInvalidNoteId &&
        range.contains(definition_.note(note.pairId).timingMicros) &&
        !noteStates_[note.pairId].played) {
      continue;
    }
    markMissed(id, finalizationTimeMicros, true);
    clearPairHolding(id);
    recordAutomaticResult(
        commitMiss(id, finalizationTimeMicros, finalizationTimeMicros));
    finishFinalizationTransaction();
    if (terminal()) {
      return {automaticResults_, lastAdvancedMicros_};
    }
  }
  if (aborted) {
    scoreState_.failUnfinishedAttempt();
  }

  if (!hasAdvanced_ || finalizationTimeMicros > lastAdvancedMicros_) {
    lastAdvancedMicros_ = finalizationTimeMicros;
  }
  hasAdvanced_ = true;
  latchTerminal(aborted ? GameplayTerminalReason::Aborted
                        : GameplayTerminalReason::PracticeComplete,
                finalizationTimeMicros);
  return {automaticResults_, lastAdvancedMicros_};
}

GameplayAdvanceResult
GameplaySimulation::advanceTo(std::int64_t songTimeMicros,
                              std::int64_t visualTimeMicros) {
  if (terminal()) {
    return emptyAdvanceResult();
  }
  automaticResults_.clear();
  lastAdvanceStats_ = {};
  if (hasAdvanced_ && songTimeMicros < lastAdvancedMicros_) {
    return {automaticResults_, lastAdvancedMicros_};
  }

  if (config_.judge.rules().ruleset == GameplayRuleset::LR2) {
    beginLr2Update(songTimeMicros, visualTimeMicros);
    finishLr2Update(songTimeMicros, visualTimeMicros);
    return {automaticResults_, lastAdvancedMicros_};
  }

  std::int64_t segmentStartMicros = lastAdvancedMicros_;
  while (true) {
    const bool hasAtTiming = atTimingCursor_ < atTimingNoteIds_.size();
    const bool hasLatePoor = latePoorCursor_ < latePoorNoteIds_.size();
    LaneRuntimeState *pendingRelease = nullptr;
    for (auto &lane : laneStates_) {
      if (lane.pendingReleaseTailId == kInvalidNoteId) continue;
      if (noteStates_[lane.pendingReleaseTailId].played) {
        lane.pendingReleaseTailId = kInvalidNoteId;
        continue;
      }
      if (pendingRelease == nullptr || lane.pendingReleaseDeadline < pendingRelease->pendingReleaseDeadline) {
        pendingRelease = &lane;
      }
    }
    if (!hasAtTiming && !hasLatePoor && pendingRelease == nullptr) {
      break;
    }

    const std::int64_t atTimingDeadlineMicros =
        hasAtTiming
            ? atTimingDeadline(definition_.note(atTimingNoteIds_[atTimingCursor_]))
            : std::numeric_limits<std::int64_t>::max();
    const std::int64_t latePoorDeadline =
        hasLatePoor
            ? chart_timing::add(chart_timing::add(
                  definition_.note(latePoorNoteIds_[latePoorCursor_]).timingMicros,
                  config_.judge.automaticPoorLateMicros(
                      definition_.note(latePoorNoteIds_[latePoorCursor_]).scratchLane
                          ? NoteJudgeRole::Scratch : NoteJudgeRole::Normal)), 1)
            : std::numeric_limits<std::int64_t>::max();
    const bool processAtTimingPhase = hasAtTiming &&
        (!hasLatePoor || atTimingDeadlineMicros <= latePoorDeadline);
    const std::int64_t noteDeadline =
        processAtTimingPhase ? atTimingDeadlineMicros : latePoorDeadline;
    const bool processRelease = pendingRelease != nullptr &&
        pendingRelease->pendingReleaseDeadline <= noteDeadline;
    const std::int64_t nextDeadline = processRelease
        ? pendingRelease->pendingReleaseDeadline : noteDeadline;
    if (nextDeadline > songTimeMicros) {
      break;
    }

    integrateHellChargeInterval(segmentStartMicros, nextDeadline);
    if (terminal()) {
      return {automaticResults_, lastAdvancedMicros_};
    }
    segmentStartMicros = std::max(segmentStartMicros, nextDeadline);
    ++lastAdvanceStats_.notesExamined;
    if (processRelease) {
      const NoteId tailId = pendingRelease->pendingReleaseTailId;
      const JudgeResult releaseJudge = pendingRelease->pendingReleaseJudge;
      pendingRelease->pendingReleaseTailId = kInvalidNoteId;
      recordAutomaticResult(commitAutomaticRelease(tailId, nextDeadline,
                                                   visualTimeMicros, releaseJudge));
      finishTransaction(nextDeadline);
    } else if (processAtTimingPhase) {
      const NoteId id = atTimingNoteIds_[atTimingCursor_++];
      processAtTiming(id, nextDeadline, visualTimeMicros);
    } else {
      const NoteId id = latePoorNoteIds_[latePoorCursor_++];
      processLatePoor(id, nextDeadline);
    }
    if (terminal()) {
      return {automaticResults_, lastAdvancedMicros_};
    }
  }

  integrateHellChargeInterval(segmentStartMicros, songTimeMicros);
  if (terminal()) {
    return {automaticResults_, lastAdvancedMicros_};
  }

  lastAdvancedMicros_ = songTimeMicros;
  hasAdvanced_ = true;
  maybeLatchChartComplete(songTimeMicros);
  return {automaticResults_, lastAdvancedMicros_};
}

GameplayInputResult
GameplaySimulation::applyPressAt(int mainLane, int compensateLane,
                                 const GameplayInputContext &context) {
  if (terminal()) {
    return {};
  }
  const std::int64_t judgedTime = inputTime(context);
  if (hasAdvanced_ && judgedTime < lastAdvancedMicros_) {
    automaticResults_.clear();
    lastAdvanceStats_ = {};
    return {};
  }
  beginInputUpdate(mainLane, true, context);
  if (terminal()) {
    return {};
  }
  const GameplayInputResult result = pressLane(mainLane, compensateLane, context);
  if (lr2UpdateOpen_) finishLr2Update(judgedTime, context.laneBeamTimeMicros);
  return result;
}

GameplayInputResult GameplaySimulation::applyReleaseAt(
    int lane, const GameplayInputContext &context, bool isBackSpin) {
  if (terminal()) {
    return {};
  }
  const std::int64_t judgedTime = inputTime(context);
  if (hasAdvanced_ && judgedTime < lastAdvancedMicros_) {
    automaticResults_.clear();
    lastAdvanceStats_ = {};
    return {};
  }
  beginInputUpdate(lane, isBackSpin, context);
  if (terminal()) {
    return {};
  }
  const GameplayInputResult result = releaseLane(lane, context, isBackSpin);
  if (lr2UpdateOpen_) finishLr2Update(judgedTime, context.laneBeamTimeMicros);
  return result;
}

const NoteRuntimeState &GameplaySimulation::noteState(NoteId id) const {
  return noteStates_.at(id);
}

GameplayInputBatch GameplaySimulation::inputBatch(
    const GameplayInputResult &selected) const noexcept {
  GameplayInputBatch result;
  static_cast<GameplayInputResult &>(result) = selected;
  result.transactions = inputTransactions_;
  return result;
}

NoteId GameplaySimulation::selectPressCandidate(int mainLane,
                                                int compensateLane,
                                                std::int64_t inputTimeMicros) {
  lastSearchStats_ = {};
  pressCandidates_.clear();
  multiBadSourceIndices_.clear();
  struct LaneScan {
    std::span<const NoteId> ids;
    std::size_t index = 0;
  };
  std::array<LaneScan, 2> scans{};
  std::size_t scanCount = 0;
  std::int64_t latestLateEdge = 0;
  for (const auto context : {JudgeWindowContext::Normal, JudgeWindowContext::Scratch}) {
    for (const auto &window : config_.judge.rules().contexts[static_cast<std::size_t>(context)].windows) {
      latestLateEdge = std::max(latestLateEdge, window.lateMicros);
    }
  }
  const std::int64_t poorCutoff = chart_timing::subtract(inputTimeMicros, latestLateEdge);
  const std::int64_t futureCutoff = std::max(
      config_.judge.latestHittableNoteTiming(NoteJudgeRole::Normal, inputTimeMicros),
      config_.judge.latestHittableNoteTiming(NoteJudgeRole::Scratch, inputTimeMicros));

  const auto addLane = [&](int lane) {
    auto *runtime = findLane(lane);
    if (runtime == nullptr || runtime->pressed) {
      return;
    }
    const auto ids = definition_.laneNotes(lane);
    while (runtime->cursor < ids.size()) {
      const NoteId id = ids[runtime->cursor];
      const auto &note = definition_.note(id);
      const auto &state = noteStates_[id];
      ++lastSearchStats_.notesExamined;
      if (note.timingMicros >= poorCutoff) {
        break;
      }
      ++runtime->cursor;
    }
    scans[scanCount++] = {ids, runtime->cursor};
  };

  addLane(mainLane);
  if (compensateLane != mainLane) {
    addLane(compensateLane);
  }

  const auto noteAllowed = [&](const NoteDefinition &note) {
    return !config_.allowedNoteRange.has_value() ||
           config_.allowedNoteRange->contains(note.timingMicros);
  };
  const auto shouldPrefer = [&](NoteId current, NoteId next) {
    const auto &currentNote = definition_.note(current);
    const auto &nextNote = definition_.note(next);
    if (noteStates_[current].played) {
      return true;
    }
    if (noteStates_[next].played) {
      return false;
    }
    const auto context = windowContextForRole(judgeRoleFor(currentNote));
    switch (config_.notePriorityMode) {
    case AppSettings::NotePriorityMode::Duration:
      return absoluteDistance(chart_timing::subtract(currentNote.timingMicros, inputTimeMicros)) >
             absoluteDistance(chart_timing::subtract(nextNote.timingMicros, inputTimeMicros));
    case AppSettings::NotePriorityMode::Combo: {
      const auto window = config_.judge.window(context, Good);
      return window.has_value() &&
             currentNote.timingMicros < chart_timing::subtract(inputTimeMicros, window->lateMicros) &&
             nextNote.timingMicros <= chart_timing::subtract(inputTimeMicros, window->earlyMicros);
    }
    case AppSettings::NotePriorityMode::Score: {
      const auto window = config_.judge.window(context, Great);
      return window.has_value() &&
             currentNote.timingMicros < chart_timing::subtract(inputTimeMicros, window->lateMicros) &&
             nextNote.timingMicros <= chart_timing::subtract(inputTimeMicros, window->earlyMicros);
    }
    case AppSettings::NotePriorityMode::Lowest:
      return false;
    }
    return false;
  };

  NoteId selected = kInvalidNoteId;
  while (true) {
    std::size_t chosen = scanCount;
    for (std::size_t index = 0; index < scanCount; ++index) {
      const auto &scan = scans[index];
      if (scan.index >= scan.ids.size()) {
        continue;
      }
      if (chosen == scanCount) {
        chosen = index;
        continue;
      }
      const NoteId candidateId = scan.ids[scan.index];
      const NoteId chosenId = scans[chosen].ids[scans[chosen].index];
      const auto &candidate = definition_.note(candidateId);
      const auto &current = definition_.note(chosenId);
      if (candidate.timingMicros < current.timingMicros) {
        chosen = index;
      }
    }
    if (chosen == scanCount) {
      break;
    }

    auto &scan = scans[chosen];
    const NoteId id = scan.ids[scan.index++];
    const auto &note = definition_.note(id);
    ++lastSearchStats_.notesExamined;
    // Both reference JudgeManagers break at dmtime >= mjudgeend before the
    // inclusive window lookup. The outer candidate cutoff is exclusive.
    if (note.timingMicros >= futureCutoff) {
      scan.index = scan.ids.size();
      continue;
    }
    const auto &state = noteStates_[id];
    if (note.kind == NoteKind::Landmine ||
        note.kind == NoteKind::LongTail ||
        !noteAllowed(note)) {
      continue;
    }
    JudgeResult judge = config_.judge.judgeAt(
        judgeRoleFor(note), note.timingMicros, inputTimeMicros);
    if (config_.judge.rules().singleMiss && state.hasNonvanishingJudge &&
        (judge.judgement == Bad || judge.judgement == Kpoor || judge.judgement == None)) {
      continue;
    }
    if (state.played) {
      const auto poor = config_.judge.window(windowContextForRole(judgeRoleFor(note)), Kpoor);
      judge.judgement = !config_.judge.rules().singleMiss && poor &&
          poor->earlyMicros <= judge.Diff && judge.Diff <= poor->lateMicros ? Kpoor : None;
    }
    if (config_.judge.rules().multiBad) {
      pressCandidates_.push_back({
          .sourceIndex = id,
          .timingMicros = note.timingMicros,
          .longNoteHead = note.kind == NoteKind::LongHead,
          .judge = !state.played && note.kind == NoteKind::LongHead
              ? config_.judge.judgeAt(note.scratchLane ? NoteJudgeRole::Scratch
                                                      : NoteJudgeRole::Normal,
                                      note.timingMicros, inputTimeMicros)
              : judge,
          .selectable = judge.judgement != None,
          .played = state.played,
      });
      continue;
    }
    if (judge.judgement == None) {
      continue;
    }
    if (selected == kInvalidNoteId) {
      selected = id;
      if (config_.notePriorityMode == AppSettings::NotePriorityMode::Lowest &&
          !state.played) {
        return selected;
      }
    } else if (shouldPrefer(selected, id) &&
               (judge.judgement != Kpoor ||
                absoluteDistance(definition_.note(selected).timingMicros -
                                 inputTimeMicros) > absoluteDistance(judge.Diff))) {
      selected = id;
    }
  }
  if (config_.judge.rules().multiBad) {
    multiBadSourceIndices_.resize(pressCandidates_.size());
    const auto mainNotes = definition_.laneNotes(mainLane);
    const auto context = !mainNotes.empty() && definition_.note(mainNotes.front()).scratchLane
                             ? JudgeWindowContext::Scratch : JudgeWindowContext::Normal;
    const Lr2CandidateResolution resolution = resolveLr2Candidates(
        pressCandidates_, multiBadSourceIndices_,
        config_.judge.rules().contexts[static_cast<std::size_t>(context)],
        config_.judge.rules().candidateSelection);
    multiBadSourceIndices_.resize(resolution.multiBadCount);
    return resolution.selectedSourceIndex.has_value()
               ? static_cast<NoteId>(*resolution.selectedSourceIndex)
               : kInvalidNoteId;
  }
  return selected;
}

NoteId GameplaySimulation::selectFallbackPressSoundNote(
    int mainLane, int compensateLane,
    std::int64_t inputTimeMicros) const {
  const auto *mainState = findLane(mainLane);
  const auto *compensationState = findLane(compensateLane);
  const auto mainNotes = mainState != nullptr && !mainState->pressed
                             ? definition_.laneKeysoundNotes(mainLane)
                             : std::span<const NoteId>();
  const auto compensationNotes =
      compensateLane != mainLane && compensationState != nullptr &&
              !compensationState->pressed
          ? definition_.laneKeysoundNotes(compensateLane)
          : std::span<const NoteId>();
  const std::int64_t rangeStart =
      config_.allowedNoteRange.has_value()
          ? config_.allowedNoteRange->startMicros
          : std::numeric_limits<std::int64_t>::min();
  const std::int64_t rangeEnd =
      config_.allowedNoteRange.has_value()
          ? config_.allowedNoteRange->endMicros
          : std::numeric_limits<std::int64_t>::max();
  const auto selected = selectManualKeysound(
      mainNotes, compensationNotes, inputTimeMicros, rangeStart, rangeEnd,
      [&](NoteId id) {
        return definition_.keysoundSource(id).timingMicros;
      });
  switch (selected.lane) {
  case ManualKeysoundLane::Main:
    return mainNotes[selected.index];
  case ManualKeysoundLane::Compensation:
    return compensationNotes[selected.index];
  case ManualKeysoundLane::None:
    return kInvalidNoteId;
  }
  return kInvalidNoteId;
}

NoteId
GameplaySimulation::selectReleaseCandidate(int lane,
                                           std::int64_t inputTimeMicros) {
  lastSearchStats_ = {};
  auto *runtime = findLane(lane);
  if (runtime == nullptr) {
    return kInvalidNoteId;
  }
  const NoteId id = runtime->heldTailId;
  if (id == kInvalidNoteId) {
    return kInvalidNoteId;
  }
  ++lastSearchStats_.notesExamined;
  const auto &state = noteStates_[id];
  if (state.holding && !state.played && !state.dead && noteAllowed(id)) {
    return id;
  }
  runtime->heldTailId = kInvalidNoteId;
  return kInvalidNoteId;
}

GameplayInputBatch
GameplaySimulation::pressLane(int lane, const GameplayInputContext &context) {
  return pressLane(lane, lane, context);
}

NoteId GameplaySimulation::previewPreparationPressSoundNote(
    int mainLane, int compensateLane,
    const GameplayInputContext &context) const {
  if (terminal() ||
      (config_.allowedNoteRange.has_value() &&
       context.songTimeMicros >= config_.allowedNoteRange->endMicros)) {
    return kInvalidNoteId;
  }
  return selectFallbackPressSoundNote(mainLane, compensateLane,
                                      inputTime(context));
}

GameplayInputResult GameplaySimulation::pressLaneForPreparation(
    int mainLane, int compensateLane,
    const GameplayInputContext &context) {
  lastSearchStats_ = {};
  GameplayInputResult result;
  if (terminal() ||
      (config_.allowedNoteRange.has_value() &&
       context.songTimeMicros >= config_.allowedNoteRange->endMicros)) {
    return result;
  }
  auto *mainState = findLane(mainLane);
  const auto *compensationState = findLane(compensateLane);
  if ((mainState == nullptr || mainState->pressed) &&
      (compensateLane == mainLane || compensationState == nullptr ||
       compensationState->pressed)) {
    return result;
  }

  const std::int64_t eventTime = inputTime(context);
  result.soundNoteId =
      selectFallbackPressSoundNote(mainLane, compensateLane, eventTime);
  if (mainState != nullptr) {
    mainState->pressed = true;
  }
  result.hasLaneVisual = true;
  result.laneVisual = {LaneVisualAction::Press, mainLane,
                       eventTime, context.laneBeamTimeMicros,
                       JudgeResult(None, 0)};
  result.hasReplayEvent = true;
  result.replayEvent = {
      .action = GameplayReplayAction::Press,
      .lane = mainLane,
      .songTimeMicros = eventTime,
      .judgeTimeMicros = eventTime,
  };
  recordReplay(result.replayEvent);
  finishTransaction(eventTime);
  return result;
}

GameplayInputResult GameplaySimulation::releaseLaneForPreparation(
    int lane, const GameplayInputContext &context) {
  lastSearchStats_ = {};
  GameplayInputResult result;
  if (terminal() ||
      (config_.allowedNoteRange.has_value() &&
       context.songTimeMicros >= config_.allowedNoteRange->endMicros)) {
    return result;
  }
  auto *laneState = findLane(lane);
  if (laneState == nullptr || !laneState->pressed) {
    return result;
  }
  laneState->pressed = false;
  const std::int64_t eventTime = inputTime(context);
  result.hasLaneVisual = true;
  result.laneVisual = {LaneVisualAction::Release, lane,
                       eventTime, context.laneBeamTimeMicros,
                       JudgeResult(None, 0)};
  result.hasReplayEvent = true;
  result.replayEvent = {
      .action = GameplayReplayAction::Release,
      .lane = lane,
      .songTimeMicros = eventTime,
      .judgeTimeMicros = eventTime,
  };
  recordReplay(result.replayEvent);
  finishTransaction(eventTime);
  return result;
}

NoteId GameplaySimulation::previewPressSoundNote(
    int mainLane, int compensateLane, const GameplayInputContext &context) {
  if (terminal() ||
      (config_.allowedNoteRange.has_value() &&
       context.songTimeMicros >= config_.allowedNoteRange->endMicros)) {
    return kInvalidNoteId;
  }
  const auto *mainState = findLane(mainLane);
  const auto *compensateState = findLane(compensateLane);
  if ((mainState == nullptr || mainState->pressed) &&
      (compensateLane == mainLane || compensateState == nullptr ||
       compensateState->pressed)) {
    return kInvalidNoteId;
  }
  const std::int64_t judgedTime = inputTime(context);
  if (mainState != nullptr &&
      mainState->suppressedBackspinPressMicros == judgedTime) {
    return kInvalidNoteId;
  }
  if (mainState != nullptr &&
      selectReleaseCandidate(mainLane, judgedTime) != kInvalidNoteId) {
    // Recovering an existing hold records an input edge without retriggering
    // its head sound. Keep the audio reservation preview aligned with pressLane.
    return kInvalidNoteId;
  }
  const NoteId judgeCandidate =
      selectPressCandidate(mainLane, compensateLane, judgedTime);
  return judgeCandidate != kInvalidNoteId
             ? judgeCandidate
             : selectFallbackPressSoundNote(mainLane, compensateLane,
                                            judgedTime);
}

GameplayInputBatch GameplaySimulation::pressScratchKey(
    int lane, bool clockwise, const GameplayInputContext &context) {
  if (config_.judge.rules().ruleset != GameplayRuleset::LR2) {
    return pressLane(lane, context);
  }
  if (config_.allowedNoteRange.has_value() &&
      context.songTimeMicros >= config_.allowedNoteRange->endMicros) return {};
  auto *state = findLane(lane);
  if (state == nullptr || terminal()) return {};
  const int key = clockwise ? 0 : 1;
  state->scratchKeysPressed[key] = true;
  const NoteId held = selectReleaseCandidate(lane, inputTime(context));
  if (held != kInvalidNoteId && state->scratchProcessingKey >= 0 &&
      state->scratchProcessingKey != key &&
      definition_.note(held).longNoteRule != LongNoteRule::Classic) {
    // JudgeManager consumes an opposite-key press as the charge tail. The
    // physical lane stays held; there is no second head-selection operation.
    const auto result = releaseLane(lane, context, true);
    state->pressed = true;
    state->suppressedBackspinPressMicros.reset();
    if (state->heldTailId == kInvalidNoteId) state->scratchProcessingKey = -1;
    return result;
  }
  state->pressed = false;
  const auto result = pressLane(lane, context);
  if (held == kInvalidNoteId && state->heldTailId != kInvalidNoteId) {
    state->scratchProcessingKey = key;
  }
  state->pressed = state->scratchKeysPressed[0] || state->scratchKeysPressed[1];
  return result;
}

NoteId GameplaySimulation::previewScratchPressSoundNote(
    int lane, const GameplayInputContext &context) {
  auto *state = findLane(lane);
  if (state == nullptr) return kInvalidNoteId;
  const bool pressed = state->pressed;
  state->pressed = false;
  const auto result = previewPressSoundNote(lane, lane, context);
  state->pressed = pressed;
  return result;
}

GameplayInputBatch GameplaySimulation::releaseScratchKey(
    int lane, bool clockwise, const GameplayInputContext &context) {
  if (config_.judge.rules().ruleset != GameplayRuleset::LR2) {
    return releaseLane(lane, context);
  }
  if (config_.allowedNoteRange.has_value() &&
      context.songTimeMicros >= config_.allowedNoteRange->endMicros) return {};
  auto *state = findLane(lane);
  if (state == nullptr || terminal()) return {};
  const int key = clockwise ? 0 : 1;
  state->scratchKeysPressed[key] = false;
  const NoteId held = selectReleaseCandidate(lane, inputTime(context));
  if (held != kInvalidNoteId && state->scratchProcessingKey >= 0 &&
      state->scratchProcessingKey != key) {
    inputTransactions_.clear();
    const auto result = releaseLaneForPreparation(lane, context);
    state->pressed = state->scratchKeysPressed[0] || state->scratchKeysPressed[1];
    inputTransactions_.push_back(result);
    return inputBatch(result);
  }
  state->pressed = true;
  const auto result = releaseLane(lane, context);
  state->pressed = state->scratchKeysPressed[0] || state->scratchKeysPressed[1];
  if (state->heldTailId == kInvalidNoteId) state->scratchProcessingKey = -1;
  return result;
}

GameplayInputBatch
GameplaySimulation::pressLane(int mainLane, int compensateLane,
                              const GameplayInputContext &context) {
  inputTransactions_.clear();
  if (terminal()) {
    return {};
  }
  lastSearchStats_ = {};
  GameplayInputResult result;
  if (config_.allowedNoteRange.has_value() &&
      context.songTimeMicros >= config_.allowedNoteRange->endMicros) {
    return inputBatch(result);
  }
  auto *mainState = findLane(mainLane);
  auto *compensateState = findLane(compensateLane);
  if ((mainState == nullptr || mainState->pressed) &&
      (compensateLane == mainLane || compensateState == nullptr ||
       compensateState->pressed)) {
    return inputBatch(result);
  }

  const std::int64_t judgedTime = inputTime(context);
  if (mainState != nullptr && mainState->suppressedBackspinPressMicros) {
    const bool continuation =
        *mainState->suppressedBackspinPressMicros == judgedTime;
    mainState->suppressedBackspinPressMicros.reset();
    if (continuation) {
      // Directional adapters encode the reference's opposite-key press as a
      // backspin release followed by a held-lane press at the same timestamp.
      // The opposite-key press has already judged the CN/HCN tail.
      mainState->pressed = true;
      result.hasReplayEvent = true;
      result.replayEvent = {.action = GameplayReplayAction::Press, .lane = mainLane,
                           .songTimeMicros = judgedTime, .judgeTimeMicros = judgedTime};
      recordReplay(result.replayEvent);
      result.hasLaneVisual = true;
      result.laneVisual = {LaneVisualAction::Press, mainLane, judgedTime,
                          context.laneBeamTimeMicros, JudgeResult(None, 0)};
      finishTransaction(judgedTime);
      inputTransactions_.push_back(result);
      return inputBatch(result);
    }
  }
  if (mainState != nullptr && selectReleaseCandidate(mainLane, judgedTime) != kInvalidNoteId) {
    mainState->pressed = true;
    mainState->pendingReleaseTailId = kInvalidNoteId;
    result.hasReplayEvent = true;
    result.replayEvent = {.action = GameplayReplayAction::Press, .lane = mainLane,
                         .songTimeMicros = judgedTime, .judgeTimeMicros = judgedTime};
    recordReplay(result.replayEvent);
    result.hasLaneVisual = true;
    result.laneVisual = {LaneVisualAction::Press, mainLane, judgedTime,
                        context.laneBeamTimeMicros, JudgeResult(None, 0)};
    finishTransaction(judgedTime);
    inputTransactions_.push_back(result);
    return inputBatch(result);
  }
  const NoteId selected =
      selectPressCandidate(mainLane, compensateLane, judgedTime);
  if (selected == kInvalidNoteId) {
    result.soundNoteId = selectFallbackPressSoundNote(
        mainLane, compensateLane, judgedTime);
    if (mainState != nullptr) {
      mainState->pressed = true;
    }
    result.hasReplayEvent = true;
    result.replayEvent = {
        .action = GameplayReplayAction::Press,
        .lane = mainLane,
        .songTimeMicros = judgedTime,
        .judgeTimeMicros = judgedTime,
    };
    recordReplay(result.replayEvent);
    result.hasLaneVisual = true;
    result.laneVisual = {LaneVisualAction::Press, mainLane,
                         judgedTime, context.laneBeamTimeMicros,
                         JudgeResult(None, 0)};
    finishTransaction(judgedTime);
    inputTransactions_.push_back(result);
    return inputBatch(result);
  }

  const auto &note = definition_.note(selected);
  if (note.kind == NoteKind::LongTail) {
    return inputBatch(result);
  }
  auto &state = noteStates_[selected];
  for (const std::size_t sourceIndex : multiBadSourceIndices_) {
    if (sourceIndex >= noteStates_.size()) {
      continue;
    }
    const NoteId multiBadId = static_cast<NoteId>(sourceIndex);
    const auto &multiBadNote = definition_.note(multiBadId);
    auto &multiBadState = noteStates_[multiBadId];
    if (multiBadState.played || multiBadState.dead) {
      continue;
    }
    markIdentityResolved(multiBadId);
    noteChanges_.record(multiBadId);
    multiBadState.played = true;
    multiBadState.playedTimeMicros = judgedTime;
    if (multiBadNote.kind == NoteKind::LongHead &&
        multiBadNote.longNoteRule == LongNoteRule::Classic) {
      clearPairHolding(multiBadId);
      if (multiBadNote.pairId != kInvalidNoteId &&
          noteAllowed(multiBadNote.pairId) &&
          !noteStates_[multiBadNote.pairId].played) {
        markMissed(multiBadNote.pairId, judgedTime, false);
      }
    }
    GameplayInputResult multiBad;
    multiBad.noteId = multiBadId;
    multiBad.hasJudge = true;
    multiBad.judge =
        JudgeResult(Bad, chart_timing::subtract(judgedTime, multiBadNote.timingMicros));
    commitJudge(multiBadId, multiBad.judge);
    multiBad.hasReplayEvent = true;
    multiBad.replayEvent = {
        .action = GameplayReplayAction::MultiBad,
        .noteId = multiBadId,
        .lane = multiBadNote.lane,
        .noteTimeMicros = multiBadNote.timingMicros,
        .songTimeMicros = judgedTime,
        .judgeTimeMicros = judgedTime,
        .judgement = Bad,
        .diffMicros = multiBad.judge.Diff,
    };
    recordReplay(multiBad.replayEvent);
    inputTransactions_.push_back(multiBad);
  }

  JudgeResult judge = config_.judge.judgeAt(
      judgeRoleFor(note), note.timingMicros, judgedTime);
  if (state.played && judge.judgement != None) {
    judge.judgement = Kpoor;
  }
  result.noteId = selected;
  result.soundNoteId = selected;
  result.judge = judge;
  if (auto *laneState = findLane(note.lane)) {
    laneState->pressed = true;
  }
  result.hasLaneVisual = true;
  result.laneVisual = {LaneVisualAction::Press, note.lane,
                       judgedTime, context.laneBeamTimeMicros, judge};

  if (judge.judgement != None) {
    noteChanges_.record(selected);
    if (judge.isNotePlayed() &&
        (judge.judgement != Bad || config_.judge.rules().vanishBad)) {
      markIdentityResolved(selected);
      state.played = true;
      state.playedTimeMicros = judgedTime;
      if (note.kind == NoteKind::LongHead) {
        state.acceptedHeadJudge = judge;
        state.holding = true;
        if (note.pairId != kInvalidNoteId) {
          noteChanges_.record(note.pairId);
          noteStates_[note.pairId].holding = true;
          if (auto *lane = findLane(note.lane)) {
            lane->heldTailId = note.pairId;
          }
        }
        result.hasJudge = note.longNoteRule != LongNoteRule::Classic;
      } else {
        result.hasJudge = true;
      }
    } else {
      state.hasNonvanishingJudge = true;
      result.hasJudge = true;
    }
    if (result.hasJudge) {
      commitJudge(selected, result.judge);
    }
    result.hasReplayEvent = true;
    result.replayEvent = {
        .action = GameplayReplayAction::Press,
        .noteId = selected,
        .lane = note.lane,
        .noteTimeMicros = note.timingMicros,
        .songTimeMicros = judgedTime,
        .judgeTimeMicros = judgedTime,
        .judgement = judge.judgement,
        .diffMicros = judge.Diff,
    };
    recordReplay(result.replayEvent);
    finishTransaction(judgedTime);
  }
  inputTransactions_.push_back(result);
  return inputBatch(result);
}

GameplayInputBatch
GameplaySimulation::releaseLane(int lane, const GameplayInputContext &context,
                                bool isBackSpin) {
  inputTransactions_.clear();
  if (terminal()) {
    return {};
  }
  lastSearchStats_ = {};
  GameplayInputResult result;
  if (config_.allowedNoteRange.has_value() &&
      context.songTimeMicros >= config_.allowedNoteRange->endMicros) {
    return inputBatch(result);
  }
  auto *laneState = findLane(lane);
  if (laneState == nullptr || (!laneState->pressed && !isBackSpin)) {
    return inputBatch(result);
  }
  laneState->pressed = false;
  const std::int64_t judgedTime = inputTime(context);
  result.hasLaneVisual = true;
  result.laneVisual = {LaneVisualAction::Release, lane,
                       judgedTime, context.laneBeamTimeMicros,
                       JudgeResult(None, 0)};

  const NoteId selected = selectReleaseCandidate(lane, judgedTime);
  if (selected == kInvalidNoteId) {
    result.hasReplayEvent = true;
    result.replayEvent = {
        .action = GameplayReplayAction::Release,
        .lane = lane,
        .songTimeMicros = judgedTime,
        .judgeTimeMicros = judgedTime,
    };
    recordReplay(result.replayEvent);
    finishTransaction(judgedTime);
    inputTransactions_.push_back(result);
    return inputBatch(result);
  }

  const auto &tail = definition_.note(selected);
  auto &tailState = noteStates_[selected];
  result.noteId = selected;
  if (tail.kind != NoteKind::LongTail || !tailState.holding ||
      tail.pairId == kInvalidNoteId) {
    result.hasReplayEvent = true;
    result.replayEvent = {
        .action = GameplayReplayAction::Release,
        .lane = lane,
        .songTimeMicros = judgedTime,
        .judgeTimeMicros = judgedTime,
    };
    recordReplay(result.replayEvent);
    finishTransaction(judgedTime);
    inputTransactions_.push_back(result);
    return inputBatch(result);
  }

  const JudgeResult tailJudge = config_.judge.judgeAt(
      judgeRoleFor(tail), tail.timingMicros, judgedTime);
  if (tail.longNoteRule != LongNoteRule::Classic && tail.scratchLane &&
      !isBackSpin && tailJudge.judgement != None &&
      tailJudge.judgement != Kpoor) {
    // A scratch lift inside the tail window is not a backspin. The charge
    // remains pending until a reversal or the automatic POOR deadline.
    result.hasReplayEvent = true;
    result.replayEvent = {.action = GameplayReplayAction::Release,
                         .lane = lane,
                         .songTimeMicros = judgedTime,
                         .judgeTimeMicros = judgedTime};
    recordReplay(result.replayEvent);
    finishTransaction(judgedTime);
    inputTransactions_.push_back(result);
    return inputBatch(result);
  }

  JudgeResult applied = tailJudge;
  if (tail.longNoteRule == LongNoteRule::Classic) {
    applied = worseLongNoteJudge(normalizeReleaseJudge(noteStates_[tail.pairId].acceptedHeadJudge),
                                 normalizeReleaseJudge(tailJudge));
  } else if (tail.scratchLane && !isBackSpin) {
    applied = JudgeResult(Poor, chart_timing::subtract(judgedTime, tail.timingMicros));
  } else if (tailJudge.judgement == None || tailJudge.judgement == Kpoor) {
    applied = JudgeResult(Poor, tailJudge.Diff);
  }


  const std::int64_t releaseMargin = tail.scratchLane
      ? config_.judge.rules().scratchReleaseMarginMicros
      : config_.judge.rules().normalReleaseMarginMicros;
  const bool lr2DeferredRelease = config_.judge.rules().ruleset == GameplayRuleset::LR2 &&
      !(isBackSpin && tail.scratchLane && tail.longNoteRule != LongNoteRule::Classic);
  if ((releaseMargin > 0 || lr2DeferredRelease) && applied.Diff < 0 &&
      (applied.judgement == Bad || applied.judgement == Poor)) {
    const auto releaseTime = lr2DeferredRelease ? context.songTimeMicros : judgedTime;
    laneState->pendingReleaseTailId = selected;
    laneState->pendingReleaseDeadline = chart_timing::add(releaseTime, releaseMargin);
    laneState->pendingReleaseJudge = JudgeResult(
        tail.longNoteRule == LongNoteRule::Classic ? Bad : applied.judgement,
        chart_timing::subtract(releaseTime, tail.timingMicros));
    if (lr2DeferredRelease && !lr2UpdateOpen_ && releaseMargin == 0) {
      // Standalone low-level calls are complete updates; batched inputs leave
      // the pending release open so a later key in the update can recover it.
      laneState->pendingReleaseTailId = kInvalidNoteId;
      auto resolved = commitAutomaticRelease(selected, context.songTimeMicros,
                                               context.laneBeamTimeMicros,
                                               laneState->pendingReleaseJudge);
      resolved.hasLaneVisual = result.hasLaneVisual;
      resolved.laneVisual = result.laneVisual;
      finishTransaction(context.songTimeMicros);
      inputTransactions_.push_back(resolved);
      return inputBatch(resolved);
    }
    result.hasReplayEvent = true;
    result.replayEvent = {.action = GameplayReplayAction::Release, .lane = lane,
                         .songTimeMicros = judgedTime, .judgeTimeMicros = judgedTime};
    recordReplay(result.replayEvent);
    finishTransaction(judgedTime);
    inputTransactions_.push_back(result);
    return inputBatch(result);
  }

  auto &headState = noteStates_[tail.pairId];
  markIdentityResolved(selected);
  noteChanges_.record(selected);
  noteChanges_.record(tail.pairId);
  tailState.played = true;
  tailState.playedTimeMicros = judgedTime;
  tailState.releaseTimeMicros = judgedTime;
  tailState.holding = false;
  headState.holding = false;

  tailState.acceptedHeadJudge = applied;
  if (config_.judge.rules().ruleset == GameplayRuleset::LR2 && isBackSpin &&
      tail.scratchLane && tail.longNoteRule != LongNoteRule::Classic) {
    laneState->suppressedBackspinPressMicros = judgedTime;
  }
  result.hasJudge = true;
  result.judge = applied;
  commitJudge(selected, result.judge);
  result.hasReplayEvent = true;
  result.replayEvent = {
      .action = GameplayReplayAction::Release,
      .noteId = selected,
      .lane = lane,
      .noteTimeMicros = tail.timingMicros,
      .songTimeMicros = judgedTime,
      .judgeTimeMicros = judgedTime,
      .judgement = applied.judgement,
      .diffMicros = applied.Diff,
  };
  recordReplay(result.replayEvent);
  finishTransaction(judgedTime);
  inputTransactions_.push_back(result);
  return inputBatch(result);
}

} // namespace gameplay
