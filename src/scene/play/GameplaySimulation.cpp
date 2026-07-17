#include "GameplaySimulation.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <utility>

namespace gameplay {
namespace {
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
      noteStates_(definition.noteCount()) {
  replayEvents_.reserve(config_.attempt.replayCapacity);
  automaticResults_.reserve(config_.attempt.automaticResultCapacity);
  scoreState_.configureBoundedGaugeHistory(config_.attempt.replayCapacity);
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

void GameplaySimulation::commitJudge(const JudgeResult &judge) {
  scoreState_.commitJudge(judge);
}

bool GameplaySimulation::recordReplay(GameplayReplayEvent &event) {
  event.gauge = scoreState_.currentGauge;
  event.gaugeType = scoreState_.gaugeType;
  event.combo = scoreState_.combo;
  event.score = scoreState_.getScore();
  if (replayEvents_.size() == replayEvents_.capacity()) {
    replayOverflowed_ = true;
    return false;
  }
  replayEvents_.push_back(event);
  return true;
}

bool GameplaySimulation::recordAutomaticResult(
    const GameplayInputResult &result) {
  if (automaticResults_.size() >= config_.attempt.automaticResultCapacity) {
    automaticResultOverflowed_ = true;
    return false;
  }
  automaticResults_.push_back(result);
  return true;
}

void GameplaySimulation::processAtTiming(NoteId id, std::int64_t songTimeMicros,
                                         std::int64_t visualTimeMicros) {
  const auto &note = definition_.note(id);
  auto &state = noteStates_[id];
  if (state.played || state.dead) {
    return;
  }

  if (note.kind == NoteKind::Landmine) {
    state.dead = true;
    state.playedTimeMicros = songTimeMicros;
    if (!lanePressed(note.lane)) {
      return;
    }

    state.played = true;
    scoreState_.applyGaugeDelta(-note.mineDamage);
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
    return;
  }

  if (note.kind != NoteKind::Normal || !config_.attempt.autoPlay) {
    return;
  }

  state.played = true;
  state.playedTimeMicros = songTimeMicros;
  GameplayInputResult press;
  press.noteId = id;
  press.soundNoteId = id;
  press.hasJudge = true;
  press.judge = JudgeResult(PGreat, 0);
  press.hasLaneVisual = true;
  press.laneVisual = {LaneVisualAction::Press, note.lane, visualTimeMicros,
                      press.judge};
  commitJudge(press.judge);
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

  GameplayInputResult release;
  release.hasLaneVisual = true;
  release.laneVisual = {LaneVisualAction::Release, note.lane, visualTimeMicros,
                        JudgeResult(None, 0)};
  recordAutomaticResult(release);
}

void GameplaySimulation::processLatePoor(NoteId id,
                                         std::int64_t songTimeMicros) {
  const auto &note = definition_.note(id);
  auto &state = noteStates_[id];
  if (note.kind != NoteKind::Normal || state.played || state.dead) {
    return;
  }

  state.played = true;
  state.dead = true;
  state.playedTimeMicros = songTimeMicros;
  GameplayInputResult result;
  result.noteId = id;
  result.hasJudge = true;
  result.judge = JudgeResult(Poor, songTimeMicros - note.timingMicros);
  commitJudge(result.judge);
  result.hasReplayEvent = true;
  result.replayEvent = {
      .action = GameplayReplayAction::Miss,
      .noteId = id,
      .lane = note.lane,
      .noteTimeMicros = note.timingMicros,
      .songTimeMicros = songTimeMicros,
      .judgeTimeMicros = songTimeMicros,
      .judgement = result.judge.judgement,
      .diffMicros = result.judge.Diff,
  };
  recordReplay(result.replayEvent);
  recordAutomaticResult(result);
}

GameplayAdvanceResult
GameplaySimulation::advanceTo(std::int64_t songTimeMicros,
                              std::int64_t visualTimeMicros) {
  automaticResults_.clear();
  lastAdvanceStats_ = {};
  if (hasAdvanced_ && songTimeMicros < lastAdvancedMicros_) {
    return {automaticResults_, lastAdvancedMicros_};
  }

  const auto chronological = definition_.chronologicalNotes();
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

    ++lastAdvanceStats_.notesExamined;
    if (processAtTimingPhase) {
      const NoteId id = chronological[atTimingCursor_++];
      processAtTiming(id, nextDeadline, visualTimeMicros);
    } else {
      const NoteId id = chronological[latePoorCursor_++];
      processLatePoor(id, nextDeadline);
    }
  }

  lastAdvancedMicros_ = songTimeMicros;
  hasAdvanced_ = true;
  return {automaticResults_, lastAdvancedMicros_};
}

GameplayInputResult
GameplaySimulation::applyPressAt(int mainLane, int compensateLane,
                                 const GameplayInputContext &context) {
  if (hasAdvanced_ && context.songTimeMicros < lastAdvancedMicros_) {
    automaticResults_.clear();
    lastAdvanceStats_ = {};
    return {};
  }
  advanceTo(context.songTimeMicros, context.laneBeamTimeMicros);
  return pressLane(mainLane, compensateLane, context);
}

GameplayInputResult GameplaySimulation::applyReleaseAt(
    int lane, const GameplayInputContext &context, bool isBackSpin) {
  if (hasAdvanced_ && context.songTimeMicros < lastAdvancedMicros_) {
    automaticResults_.clear();
    lastAdvanceStats_ = {};
    return {};
  }
  advanceTo(context.songTimeMicros, context.laneBeamTimeMicros);
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

GameplayInputResult
GameplaySimulation::pressLane(int mainLane, int compensateLane,
                              const GameplayInputContext &context) {
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
  }
  return result;
}

GameplayInputResult
GameplaySimulation::releaseLane(int lane, const GameplayInputContext &context,
                                bool isBackSpin) {
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
    return result;
  }

  auto &headState = noteStates_[tail.pairId];
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
  return result;
}

} // namespace gameplay
