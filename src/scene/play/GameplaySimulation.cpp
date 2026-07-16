#include "GameplaySimulation.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <utility>

namespace gameplay {

GameplaySimulation::GameplaySimulation(const GameplayDefinition &definition,
                                       GameplaySimulationConfig config)
    : definition_(definition), config_(std::move(config)),
      noteStates_(definition.noteCount()) {
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
    if (currentNote.timingMicros == nextNote.timingMicros) {
      return false;
    }
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
  GameplayInputResult result;
  if (config_.allowedNoteRange.has_value() &&
      context.songTimeMicros >= config_.allowedNoteRange->endMicros) {
    return result;
  }
  result.noteId =
      selectPressCandidate(mainLane, compensateLane, inputTime(context));
  return result;
}

GameplayInputResult
GameplaySimulation::releaseLane(int lane, const GameplayInputContext &context,
                                bool) {
  GameplayInputResult result;
  if (config_.allowedNoteRange.has_value() &&
      context.songTimeMicros >= config_.allowedNoteRange->endMicros) {
    return result;
  }
  result.noteId = selectReleaseCandidate(lane, inputTime(context));
  return result;
}

} // namespace gameplay
