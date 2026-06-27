#include "ReplayResultStateBuilder.h"

#include "CoursePlaySession.h"

#include <algorithm>
#include <string>
#include <unordered_map>

namespace {
std::string replayNoteKey(int lane, long long noteTimeMicros) {
  return std::to_string(lane) + ":" + std::to_string(noteTimeMicros);
}

std::unordered_map<std::string, bms_parser::Note *>
buildReplayNoteLookup(bms_parser::Chart &chart) {
  std::unordered_map<std::string, bms_parser::Note *> lookup;
  for (const auto &measure : chart.Measures) {
    for (const auto &timeline : measure->TimeLines) {
      for (auto *note : timeline->Notes) {
        if (note == nullptr) {
          continue;
        }
        lookup[replayNoteKey(note->Lane, timeline->Timing)] = note;
      }
      for (auto *note : timeline->LandmineNotes) {
        if (note == nullptr) {
          continue;
        }
        lookup[replayNoteKey(note->Lane, timeline->Timing)] = note;
      }
    }
  }
  return lookup;
}

bms_parser::Note *findReplayNote(
    const std::unordered_map<std::string, bms_parser::Note *> &lookup,
    const ReplayEvent &event) {
  if (event.noteTimeMicros < 0) {
    return nullptr;
  }
  const auto it = lookup.find(replayNoteKey(event.lane, event.noteTimeMicros));
  return it == lookup.end() ? nullptr : it->second;
}

bool replayEventCountsInResult(
    bms_parser::Chart &chart,
    const std::unordered_map<std::string, bms_parser::Note *> &lookup,
    const ReplayEvent &event) {
  if (event.judgement == None) {
    return false;
  }
  if (event.action != ReplayEventAction::Press) {
    return true;
  }

  const JudgeResult recordedJudge(event.judgement, event.diffMicros);
  auto *note = findReplayNote(lookup, event);
  if (note == nullptr || !note->IsLongNote()) {
    return true;
  }

  auto *longNote = static_cast<bms_parser::LongNote *>(note);
  return longNote->IsTail() || !recordedJudge.isNotePlayed() ||
         effectiveLongNoteIsCharge(longNote, chart);
}

void syncReplayResultGaugeSnapshot(RhythmState &state,
                                   const ReplayEvent &event) {
  state.gaugeType = event.gaugeType;
  state.currentGauge = event.gauge;
  const int gaugeIndex = gaugeTypeIndex(event.gaugeType);
  if (gaugeIndex >= 0 &&
      gaugeIndex < static_cast<int>(state.gaugeValues.size())) {
    state.gaugeValues[gaugeIndex] = event.gauge;
  }
  if (!state.gaugeHistory.empty()) {
    state.gaugeHistory.back() = event.gauge;
  } else {
    state.gaugeHistory.push_back(event.gauge);
  }
}
} // namespace

namespace replay_result {
RhythmState BuildResultState(bms_parser::Chart &chart,
                             const ReplayData &replay,
                             GaugeProfile gaugeProfile) {
  const auto lookup = buildReplayNoteLookup(chart);
  RhythmState state(&chart, false);
  state.configureGauge(replay.initialGaugeType, replay.gaugeAutoShift,
                       gaugeProfile);
  state.setAssistClearMark(assist_options::isEnabled(replay.assistOption));

  for (const auto &event : replay.events) {
    if (event.action == ReplayEventAction::Mine) {
      if (auto *note = findReplayNote(lookup, event);
          note != nullptr && note->IsLandmineNote()) {
        auto *mine = static_cast<bms_parser::LandmineNote *>(note);
        state.applyGaugeDelta(-mine->Damage);
      }
      syncReplayResultGaugeSnapshot(state, event);
      continue;
    }

    if (!replayEventCountsInResult(chart, lookup, event)) {
      continue;
    }

    const JudgeResult judgeResult(event.judgement, event.diffMicros);
    state.judgeCount[event.judgement]++;
    if (judgeResult.isComboBreak()) {
      state.combo = 0;
      state.comboBreak++;
    } else if (event.judgement != Kpoor) {
      state.combo++;
      state.maxCombo = std::max(state.maxCombo, state.combo);
    }
    state.recordFastSlow(judgeResult);
    state.applyGaugeJudgement(event.judgement);
    state.combo = event.combo;
    state.maxCombo = std::max(state.maxCombo, event.combo);
    syncReplayResultGaugeSnapshot(state, event);
  }

  if (!replay.events.empty() && state.gaugeHistory.empty()) {
    state.currentGauge = replay.finalGauge;
  }
  return state;
}
} // namespace replay_result
