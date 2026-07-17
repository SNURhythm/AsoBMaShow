#include "GameplaySimulation.h"
#include "ManualKeysoundSelection.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <utility>

namespace gameplay {
namespace {
constexpr std::int64_t kHellChargeGaugeTickMicros = 200'000;

JudgeResult normalizeReleaseJudge(const JudgeResult &judge) {
  if (judge.judgement == None || judge.judgement == Kpoor ||
      judge.judgement == Poor) {
    return JudgeResult(Bad, judge.Diff);
  }
  return judge;
}

std::int64_t absoluteDistance(std::int64_t value) {
  return value < 0 ? -value : value;
}
} // namespace

GameplaySimulation::GameplaySimulation(const GameplayDefinition &definition,
                                       GameplaySimulationConfig config)
    : definition_(definition), config_(std::move(config)),
      scoreState_({.totalNotes = definition.metadata().totalNotes,
                   .keyMode = definition.metadata().keyMode,
                   .gaugeTotal = definition.metadata().gaugeTotal}),
      noteStates_(definition.noteCount()),
      hellChargeBalanceMicros_(definition.noteCount()) {
  replayEvents_.reserve(config_.attempt.replayCapacity);
  automaticResults_.reserve(config_.attempt.automaticResultCapacity);
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
  scoreState_.setAssistClearMark(config_.attempt.assistClearMark);
  laneStates_.reserve(definition.lanes().size());
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
  return context.songTimeMicros - context.inputDelayMicros;
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

void GameplaySimulation::commitJudge(const JudgeResult &judge) {
  const bool wasSurvivalFailed = scoreState_.activeGaugeFailed();
  const bool wasGaugeHistoryOverflowed =
      scoreState_.gaugeHistoryOverflowed();
  scoreState_.commitJudge(judge);
  observeGaugeMutation(wasSurvivalFailed, wasGaugeHistoryOverflowed);
}

void GameplaySimulation::applyGaugeDelta(float delta) {
  const bool wasSurvivalFailed = scoreState_.activeGaugeFailed();
  const bool wasGaugeHistoryOverflowed =
      scoreState_.gaugeHistoryOverflowed();
  scoreState_.applyGaugeDelta(delta);
  observeGaugeMutation(wasSurvivalFailed, wasGaugeHistoryOverflowed);
}

void GameplaySimulation::applyGaugeJudgementRate(Judgement judgement,
                                                 float rate) {
  const bool wasSurvivalFailed = scoreState_.activeGaugeFailed();
  const bool wasGaugeHistoryOverflowed =
      scoreState_.gaugeHistoryOverflowed();
  scoreState_.applyGaugeJudgementRate(judgement, rate);
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
  if (!state.played && !state.dead) {
    ++resolvedIdentityCount_;
  }
}

void GameplaySimulation::finishTransaction(
    std::int64_t boundaryTimeMicros) {
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
      resolvedIdentityCount_ != definition_.noteCount()) {
    return;
  }
  const std::int64_t completionMicros =
      definition_.metadata().finalTimelineTimeMicros +
      config_.judge.latePoorTimingMicros() + 1;
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
  state.played = true;
  state.dead = dead;
  state.playedTimeMicros = judgeTimeMicros;
  state.holding = false;
}

void GameplaySimulation::clearPairHolding(NoteId id) {
  if (id == kInvalidNoteId || id >= noteStates_.size()) {
    return;
  }
  noteStates_[id].holding = false;
  const NoteId pairId = definition_.note(id).pairId;
  if (pairId != kInvalidNoteId && pairId < noteStates_.size()) {
    noteStates_[pairId].holding = false;
  }
}

GameplayInputResult
GameplaySimulation::commitMiss(NoteId id, std::int64_t songTimeMicros,
                               std::int64_t judgeTimeMicros) {
  return commitMiss(
      id, songTimeMicros, judgeTimeMicros,
      JudgeResult(Poor, judgeTimeMicros - definition_.note(id).timingMicros));
}

GameplayInputResult GameplaySimulation::commitMiss(NoteId id,
                                                   std::int64_t songTimeMicros,
                                                   std::int64_t judgeTimeMicros,
                                                   const JudgeResult &judge) {
  const auto &note = definition_.note(id);
  GameplayInputResult result;
  result.noteId = id;
  result.hasJudge = true;
  result.judge = judge;
  commitJudge(result.judge);
  result.hasReplayEvent = true;
  result.replayEvent = {
      .action = GameplayReplayAction::Miss,
      .noteId = id,
      .lane = note.lane,
      .noteTimeMicros = note.timingMicros,
      .songTimeMicros = songTimeMicros,
      .judgeTimeMicros = judgeTimeMicros,
      .judgement = result.judge.judgement,
      .diffMicros = result.judge.Diff,
  };
  recordReplay(result.replayEvent);
  return result;
}

GameplayInputResult GameplaySimulation::commitAutomaticRelease(
    NoteId tailId, std::int64_t songTimeMicros, std::int64_t visualTimeMicros) {
  const auto &tail = definition_.note(tailId);
  auto &tailState = noteStates_[tailId];
  const auto &headState = noteStates_[tail.pairId];
  markIdentityResolved(tailId);
  tailState.played = true;
  tailState.playedTimeMicros = songTimeMicros;
  tailState.releaseTimeMicros = songTimeMicros;
  clearPairHolding(tailId);

  const JudgeResult tailJudge =
      config_.judge.judgeAt(tail.timingMicros, songTimeMicros);
  JudgeResult applied = normalizeReleaseJudge(tailJudge);
  if (tail.longNoteRule == LongNoteRule::Classic) {
    const auto &head = definition_.note(tail.pairId);
    const JudgeResult headJudge =
        config_.judge.judgeAt(head.timingMicros, headState.playedTimeMicros);
    applied = normalizeReleaseJudge(absoluteDistance(tailJudge.Diff) >
                                            absoluteDistance(headJudge.Diff)
                                        ? tailJudge
                                        : headJudge);
  }

  GameplayInputResult result;
  result.noteId = tailId;
  result.hasJudge = true;
  result.judge = applied;
  commitJudge(result.judge);
  result.hasReplayEvent = true;
  result.replayEvent = {
      .action = GameplayReplayAction::Release,
      .noteId = tailId,
      .lane = tail.lane,
      .noteTimeMicros = tail.timingMicros,
      .songTimeMicros = songTimeMicros,
      .judgeTimeMicros = songTimeMicros,
      .judgement = applied.judgement,
      .diffMicros = applied.Diff,
  };
  recordReplay(result.replayEvent);
  if (config_.attempt.autoPlay) {
    result.hasLaneVisual = true;
    result.laneVisual = {LaneVisualAction::Release, tail.lane, visualTimeMicros,
                         JudgeResult(None, 0)};
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
  while (atTimingCursor_ < chronological.size() &&
         definition_.note(chronological[atTimingCursor_]).timingMicros <
             startMicros) {
    ++atTimingCursor_;
  }
  while (latePoorCursor_ < chronological.size() &&
         definition_.note(chronological[latePoorCursor_]).timingMicros <
             startMicros) {
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
    state.dead = true;
    state.playedTimeMicros = songTimeMicros;
    if (!lanePressed(note.lane)) {
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
    if (!state.holding || note.pairId == kInvalidNoteId ||
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
  state.played = true;
  state.playedTimeMicros = songTimeMicros;
  if (note.kind == NoteKind::LongHead) {
    state.holding = true;
    if (note.pairId != kInvalidNoteId) {
      noteStates_[note.pairId].holding = true;
    }
  }
  GameplayInputResult press;
  press.noteId = id;
  press.soundNoteId = id;
  press.hasJudge = note.kind == NoteKind::Normal ||
                   note.longNoteRule != LongNoteRule::Classic;
  press.judge = JudgeResult(PGreat, 0);
  press.hasLaneVisual = true;
  press.laneVisual = {LaneVisualAction::Press, note.lane, visualTimeMicros,
                      press.judge};
  if (press.hasJudge) {
    commitJudge(press.judge);
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
  release.laneVisual = {LaneVisualAction::Release, note.lane, visualTimeMicros,
                        JudgeResult(None, 0)};
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
    finishTransaction(songTimeMicros);
    if (terminal()) {
      return;
    }
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
  recordAutomaticResult(commitMiss(id, songTimeMicros, songTimeMicros));
  finishTransaction(songTimeMicros);
}

bool GameplaySimulation::hellChargeActiveAt(
    NoteId headId, std::int64_t timeMicros) const {
  if (!noteAllowed(headId)) {
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
      const auto &headState = noteStates_[headId];
      const bool gaining = headState.holding || lanePressed(head.lane) ||
                           config_.attempt.autoPlay;
      const std::int64_t balance = hellChargeBalanceMicros_[headId];
      const std::int64_t untilCrossing =
          gaining ? kHellChargeGaugeTickMicros + 1 - balance
                  : balance + kHellChargeGaugeTickMicros + 1;
      if (untilCrossing <= toMicros - currentMicros) {
        nextCrossingMicros =
            std::min(nextCrossingMicros, currentMicros + untilCrossing);
      }
    }

    const std::int64_t intervalEnd =
        std::min(toMicros, nextCrossingMicros);
    const std::int64_t activeDelta = intervalEnd - currentMicros;
    for (const NoteId headId : heads) {
      if (!hellChargeActiveAt(headId, currentMicros)) {
        continue;
      }
      const auto &head = definition_.note(headId);
      const auto &headState = noteStates_[headId];
      const bool gaining = headState.holding || lanePressed(head.lane) ||
                           config_.attempt.autoPlay;
      hellChargeBalanceMicros_[headId] +=
          gaining ? activeDelta : -activeDelta;
    }
    currentMicros = intervalEnd;

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

  const auto &range = *config_.allowedNoteRange;
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
      finishTransaction(finalizationTimeMicros);
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
      finishTransaction(finalizationTimeMicros);
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
    finishTransaction(finalizationTimeMicros);
    if (terminal()) {
      return {automaticResults_, lastAdvancedMicros_};
    }
  }

  if (!hasAdvanced_ || finalizationTimeMicros > lastAdvancedMicros_) {
    lastAdvancedMicros_ = finalizationTimeMicros;
  }
  hasAdvanced_ = true;
  latchTerminal(GameplayTerminalReason::PracticeComplete,
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

  const auto chronological = definition_.chronologicalNotes();
  std::int64_t segmentStartMicros = lastAdvancedMicros_;
  while (true) {
    const bool hasAtTiming = atTimingCursor_ < chronological.size();
    const bool hasLatePoor = latePoorCursor_ < chronological.size();
    if (!hasAtTiming && !hasLatePoor) {
      break;
    }

    const std::int64_t atTimingDeadline =
        hasAtTiming
            ? definition_.note(chronological[atTimingCursor_]).timingMicros
            : std::numeric_limits<std::int64_t>::max();
    const std::int64_t latePoorDeadline =
        hasLatePoor
            ? definition_.note(chronological[latePoorCursor_]).timingMicros +
                  config_.judge.latePoorTimingMicros() + 1
            : std::numeric_limits<std::int64_t>::max();
    const bool processAtTimingPhase = atTimingDeadline <= latePoorDeadline;
    const std::int64_t nextDeadline =
        processAtTimingPhase ? atTimingDeadline : latePoorDeadline;
    if (nextDeadline > songTimeMicros) {
      break;
    }

    integrateHellChargeInterval(segmentStartMicros, nextDeadline);
    if (terminal()) {
      return {automaticResults_, lastAdvancedMicros_};
    }
    segmentStartMicros = std::max(segmentStartMicros, nextDeadline);
    ++lastAdvanceStats_.notesExamined;
    if (processAtTimingPhase) {
      const NoteId id = chronological[atTimingCursor_++];
      processAtTiming(id, nextDeadline, visualTimeMicros);
    } else {
      const NoteId id = chronological[latePoorCursor_++];
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
  advanceTo(judgedTime, context.laneBeamTimeMicros);
  if (terminal()) {
    return {};
  }
  return pressLane(mainLane, compensateLane, context);
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
  advanceTo(judgedTime, context.laneBeamTimeMicros);
  if (terminal()) {
    return {};
  }
  return releaseLane(lane, context, isBackSpin);
}

const NoteRuntimeState &GameplaySimulation::noteState(NoteId id) const {
  return noteStates_.at(id);
}

NoteId GameplaySimulation::selectPressCandidate(int mainLane,
                                                int compensateLane,
                                                std::int64_t inputTimeMicros) {
  lastSearchStats_ = {};
  struct LaneScan {
    std::span<const NoteId> ids;
    std::size_t index = 0;
  };
  std::array<LaneScan, 2> scans{};
  std::size_t scanCount = 0;
  const std::int64_t poorCutoff =
      inputTimeMicros - config_.judge.latePoorTimingMicros();
  const std::int64_t futureCutoff =
      config_.judge.latestHittableNoteTiming(inputTimeMicros);

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
      if (!state.played && !state.dead && note.timingMicros >= poorCutoff) {
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
    switch (config_.notePriorityMode) {
    case AppSettings::NotePriorityMode::Duration:
      return std::llabs(currentNote.timingMicros - inputTimeMicros) >
             std::llabs(nextNote.timingMicros - inputTimeMicros);
    case AppSettings::NotePriorityMode::Combo: {
      const auto window = config_.judge.window(Good);
      return window.has_value() &&
             currentNote.timingMicros < inputTimeMicros - window->lateMicros &&
             nextNote.timingMicros <= inputTimeMicros - window->earlyMicros;
    }
    case AppSettings::NotePriorityMode::Score: {
      const auto window = config_.judge.window(Great);
      return window.has_value() &&
             currentNote.timingMicros < inputTimeMicros - window->lateMicros &&
             nextNote.timingMicros <= inputTimeMicros - window->earlyMicros;
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
    if (note.timingMicros > futureCutoff) {
      scan.index = scan.ids.size();
      continue;
    }
    const auto &state = noteStates_[id];
    if (state.played || state.dead || note.kind == NoteKind::Landmine ||
        note.kind == NoteKind::LongTail ||
        !noteAllowed(note)) {
      continue;
    }
    const JudgeResult judge =
        config_.judge.judgeAt(note.timingMicros, inputTimeMicros);
    if (judge.judgement == None) {
      continue;
    }
    if (selected == kInvalidNoteId) {
      selected = id;
      if (config_.notePriorityMode == AppSettings::NotePriorityMode::Lowest) {
        return selected;
      }
    } else if (shouldPrefer(selected, id)) {
      selected = id;
    }
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
  const auto ids = definition_.laneNotes(lane);
  const std::int64_t poorCutoff =
      inputTimeMicros - config_.judge.latePoorTimingMicros();
  for (std::size_t index = runtime->cursor; index < ids.size(); ++index) {
    const NoteId id = ids[index];
    const auto &note = definition_.note(id);
    const auto &state = noteStates_[id];
    ++lastSearchStats_.notesExamined;
    if (config_.allowedNoteRange.has_value() &&
        note.timingMicros >= config_.allowedNoteRange->endMicros) {
      runtime->cursor = ids.size();
      return kInvalidNoteId;
    }
    if (state.played || state.dead || note.timingMicros < poorCutoff) {
      runtime->cursor = index + 1;
      continue;
    }
    if (config_.allowedNoteRange.has_value() &&
        !config_.allowedNoteRange->contains(note.timingMicros)) {
      continue;
    }
    return id;
  }
  return kInvalidNoteId;
}

GameplayInputResult
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
                       context.laneBeamTimeMicros, JudgeResult(None, 0)};
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
                       context.laneBeamTimeMicros, JudgeResult(None, 0)};
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
  const NoteId judgeCandidate =
      selectPressCandidate(mainLane, compensateLane, judgedTime);
  return judgeCandidate != kInvalidNoteId
             ? judgeCandidate
             : selectFallbackPressSoundNote(mainLane, compensateLane,
                                            judgedTime);
}

GameplayInputResult
GameplaySimulation::pressLane(int mainLane, int compensateLane,
                              const GameplayInputContext &context) {
  if (terminal()) {
    return {};
  }
  lastSearchStats_ = {};
  GameplayInputResult result;
  if (config_.allowedNoteRange.has_value() &&
      context.songTimeMicros >= config_.allowedNoteRange->endMicros) {
    return result;
  }
  auto *mainState = findLane(mainLane);
  auto *compensateState = findLane(compensateLane);
  if ((mainState == nullptr || mainState->pressed) &&
      (compensateLane == mainLane || compensateState == nullptr ||
       compensateState->pressed)) {
    return result;
  }

  const std::int64_t judgedTime = inputTime(context);
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
                         context.laneBeamTimeMicros, JudgeResult(None, 0)};
    finishTransaction(judgedTime);
    return result;
  }

  const auto &note = definition_.note(selected);
  if (note.kind == NoteKind::LongTail) {
    return result;
  }
  auto &state = noteStates_[selected];
  const JudgeResult judge = config_.judge.judgeAt(note.timingMicros, judgedTime);
  result.noteId = selected;
  result.soundNoteId = selected;
  result.judge = judge;
  if (auto *laneState = findLane(note.lane)) {
    laneState->pressed = true;
  }
  result.hasLaneVisual = true;
  result.laneVisual = {LaneVisualAction::Press, note.lane,
                       context.laneBeamTimeMicros, judge};

  if (judge.judgement != None) {
    if (judge.isNotePlayed()) {
      markIdentityResolved(selected);
      state.played = true;
      state.playedTimeMicros = judgedTime;
      if (note.kind == NoteKind::LongHead) {
        state.holding = true;
        if (note.pairId != kInvalidNoteId) {
          noteStates_[note.pairId].holding = true;
        }
        result.hasJudge = note.longNoteRule != LongNoteRule::Classic;
      } else {
        result.hasJudge = true;
      }
    } else {
      result.hasJudge = true;
    }
    if (result.hasJudge) {
      commitJudge(result.judge);
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
  return result;
}

GameplayInputResult
GameplaySimulation::releaseLane(int lane, const GameplayInputContext &context,
                                bool isBackSpin) {
  if (terminal()) {
    return {};
  }
  lastSearchStats_ = {};
  GameplayInputResult result;
  if (config_.allowedNoteRange.has_value() &&
      context.songTimeMicros >= config_.allowedNoteRange->endMicros) {
    return result;
  }
  auto *laneState = findLane(lane);
  if (laneState == nullptr || !laneState->pressed) {
    return result;
  }
  laneState->pressed = false;
  const std::int64_t judgedTime = inputTime(context);
  result.hasLaneVisual = true;
  result.laneVisual = {LaneVisualAction::Release, lane,
                       context.laneBeamTimeMicros, JudgeResult(None, 0)};

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
    return result;
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
    return result;
  }

  auto &headState = noteStates_[tail.pairId];
  markIdentityResolved(selected);
  tailState.played = true;
  tailState.playedTimeMicros = judgedTime;
  tailState.releaseTimeMicros = judgedTime;
  tailState.holding = false;
  headState.holding = false;

  const JudgeResult tailJudge =
      config_.judge.judgeAt(tail.timingMicros, judgedTime);
  JudgeResult applied = tailJudge;
  if (tail.longNoteRule == LongNoteRule::Classic) {
    const auto &head = definition_.note(tail.pairId);
    const JudgeResult headJudge =
        config_.judge.judgeAt(head.timingMicros, headState.playedTimeMicros);
    applied = normalizeReleaseJudge(
        absoluteDistance(tailJudge.Diff) > absoluteDistance(headJudge.Diff)
            ? tailJudge
            : headJudge);
  } else if (tail.scratchLane && !isBackSpin) {
    applied = JudgeResult(Poor, judgedTime - tail.timingMicros);
  } else {
    applied = normalizeReleaseJudge(tailJudge);
  }

  result.hasJudge = true;
  result.judge = applied;
  commitJudge(result.judge);
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
  return result;
}

} // namespace gameplay
