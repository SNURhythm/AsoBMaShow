#include "JudgedPlaybackResultState.h"

#include "../CoursePlaySession.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>

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

bool recordedDifferenceMatches(std::int64_t judgeTimeMicros,
                               std::int64_t noteTimeMicros,
                               std::int64_t diffMicros) noexcept {
  if ((diffMicros > 0 &&
       noteTimeMicros >
           std::numeric_limits<std::int64_t>::max() - diffMicros) ||
      (diffMicros < 0 &&
       noteTimeMicros <
           std::numeric_limits<std::int64_t>::min() - diffMicros)) {
    return false;
  }
  return noteTimeMicros + diffMicros == judgeTimeMicros;
}

bool recordedDifferenceMatches(const ReplayEvent &event) noexcept {
  return recordedDifferenceMatches(event.judgeTimeMicros, event.noteTimeMicros,
                                   event.diffMicros);
}

bool ordinaryReplayNote(bms_parser::Note *note) noexcept {
  return note != nullptr && !note->IsLandmineNote();
}

struct ReplayTimingSources {
  std::unordered_map<std::string, const ReplayEvent *> playedPressByNote;
  std::unordered_map<std::string, const ReplayEvent *> poorMissByNote;
};

bool classicReleaseTimingMatches(bms_parser::LongNote *tail,
                                 const ReplayTimingSources &sources,
                                 const ReplayEvent &release) noexcept {
  if (recordedDifferenceMatches(release) || tail == nullptr ||
      tail->Head == nullptr || tail->Head->Timeline == nullptr) {
    return recordedDifferenceMatches(release);
  }
  const auto *head = tail->Head;
  const auto found = sources.playedPressByNote.find(
      replayNoteKey(head->Lane, head->Timeline->Timing));
  if (found == sources.playedPressByNote.end()) {
    return false;
  }
  const ReplayEvent &candidate = *found->second;
  return candidate.songTimeMicros <= release.songTimeMicros &&
         candidate.diffMicros == release.diffMicros;
}

bool chargeTailMissTimingMatches(bms_parser::LongNote *tail,
                                 const ReplayTimingSources &sources,
                                 const ReplayEvent &miss) noexcept {
  if (recordedDifferenceMatches(miss) || tail == nullptr ||
      tail->Head == nullptr || tail->Head->Timeline == nullptr) {
    return recordedDifferenceMatches(miss);
  }
  const auto *head = tail->Head;
  const auto found = sources.poorMissByNote.find(
      replayNoteKey(head->Lane, head->Timeline->Timing));
  if (found == sources.poorMissByNote.end()) {
    return false;
  }
  const ReplayEvent &candidate = *found->second;
  return candidate.songTimeMicros == miss.songTimeMicros &&
         candidate.judgeTimeMicros == miss.judgeTimeMicros &&
         candidate.diffMicros == miss.diffMicros;
}

void rememberReplayTimingSource(ReplayTimingSources &sources,
                                const ReplayEvent &event) {
  const std::string key = replayNoteKey(event.lane, event.noteTimeMicros);
  if (event.action == ReplayEventAction::Press &&
      JudgeResult(event.judgement, event.diffMicros).isNotePlayed()) {
    sources.playedPressByNote.try_emplace(key, &event);
  }
  if (event.action == ReplayEventAction::Miss && event.judgement == Poor) {
    sources.poorMissByNote.try_emplace(key, &event);
  }
}

std::optional<std::int64_t>
earliestHellChargeHeadMicros(bms_parser::Chart &chart) {
  std::optional<std::int64_t> earliest;
  for (const auto *measure : chart.Measures) {
    if (measure == nullptr) {
      continue;
    }
    for (const auto *timeline : measure->TimeLines) {
      if (timeline == nullptr) {
        continue;
      }
      for (auto *note : timeline->Notes) {
        if (note == nullptr || !note->IsLongNote()) {
          continue;
        }
        auto *head = static_cast<bms_parser::LongNote *>(note);
        if (head->IsTail() || head->Tail == nullptr ||
            head->Tail->Timeline == nullptr ||
            !effectiveLongNoteIsHellCharge(head, chart)) {
          continue;
        }
        earliest = !earliest.has_value()
                       ? std::optional<std::int64_t>(timeline->Timing)
                       : std::min(*earliest, timeline->Timing);
      }
    }
  }
  return earliest;
}

bool replayEventContractMatches(
    bms_parser::Chart &chart,
    const std::unordered_map<std::string, bms_parser::Note *> &lookup,
    const ReplayTimingSources &timingSources,
    std::optional<std::int64_t> earliestHellChargeHead,
    const ReplayEvent &event) {
  auto *note = findReplayNote(lookup, event);
  switch (event.action) {
  case ReplayEventAction::Press:
    if (event.judgement == None) {
      return event.noteTimeMicros == -1 && event.diffMicros == 0 &&
             event.songTimeMicros == event.judgeTimeMicros;
    }
    if (event.judgement == Poor || !ordinaryReplayNote(note) ||
        (note->IsLongNote() &&
         static_cast<bms_parser::LongNote *>(note)->IsTail())) {
      return false;
    }
    return event.songTimeMicros == event.judgeTimeMicros &&
           recordedDifferenceMatches(event);
  case ReplayEventAction::Release:
    if (event.judgement == None) {
      return event.noteTimeMicros == -1 && event.diffMicros == 0 &&
             event.songTimeMicros == event.judgeTimeMicros;
    }
    if (event.judgement == Kpoor || !ordinaryReplayNote(note) ||
        !note->IsLongNote()) {
      return false;
    }
    if (auto *tail = static_cast<bms_parser::LongNote *>(note);
        tail->IsTail()) {
      if (effectiveLongNoteIsCharge(tail, chart)) {
        if (event.judgement == Poor &&
            !chartLaneIsScratch(chart.Meta, event.lane)) {
          return false;
        }
        return event.songTimeMicros == event.judgeTimeMicros &&
               recordedDifferenceMatches(event);
      }
      return event.judgement != Poor &&
             classicReleaseTimingMatches(tail, timingSources, event);
    }
    return false;
  case ReplayEventAction::Miss:
    if (event.judgement != Poor || !ordinaryReplayNote(note)) {
      return false;
    }
    if (note->IsLongNote()) {
      auto *longNote = static_cast<bms_parser::LongNote *>(note);
      if (longNote->IsTail() && effectiveLongNoteIsCharge(longNote, chart)) {
        return chargeTailMissTimingMatches(longNote, timingSources, event);
      }
    }
    return recordedDifferenceMatches(event);
  case ReplayEventAction::Mine:
    return event.judgement == None && event.diffMicros == 0 &&
           note != nullptr && note->IsLandmineNote() &&
           event.judgeTimeMicros >= event.noteTimeMicros;
  case ReplayEventAction::Gauge:
    return (event.judgement == Great || event.judgement == Bad) &&
           event.lane == -1 && event.noteTimeMicros == -1 &&
           event.diffMicros == 0 &&
           event.songTimeMicros == event.judgeTimeMicros &&
           earliestHellChargeHead.has_value() &&
           event.songTimeMicros > *earliestHellChargeHead;
  case ReplayEventAction::MultiBad:
    return event.judgement == Bad && ordinaryReplayNote(note) &&
           (!note->IsLongNote() ||
            !static_cast<bms_parser::LongNote *>(note)->IsTail()) &&
           event.songTimeMicros == event.judgeTimeMicros &&
           recordedDifferenceMatches(event);
  }
  return false;
}

std::unordered_set<const bms_parser::LongNote *>
classicLongHeadsWithRecordedTailResult(
    bms_parser::Chart &chart,
    const std::unordered_map<std::string, bms_parser::Note *> &lookup,
    const JudgedPlaybackData &replay) {
  std::unordered_set<const bms_parser::LongNote *> heads;
  for (const ReplayEvent &event : replay.events) {
    if (event.judgement == None ||
        (event.action != ReplayEventAction::Release &&
         event.action != ReplayEventAction::Miss)) {
      continue;
    }
    auto *note = findReplayNote(lookup, event);
    if (note == nullptr || !note->IsLongNote()) {
      continue;
    }
    auto *tail = static_cast<bms_parser::LongNote *>(note);
    if (tail->IsTail() && tail->Head != nullptr &&
        !effectiveLongNoteIsCharge(tail, chart)) {
      heads.insert(tail->Head);
    }
  }
  return heads;
}

bool replayEventCountsInResult(
    bms_parser::Chart &chart,
    const std::unordered_map<std::string, bms_parser::Note *> &lookup,
    const std::unordered_set<const bms_parser::LongNote *>
        &classicHeadsWithTailResult,
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
         effectiveLongNoteIsCharge(longNote, chart) ||
         (event.judgement == Bad &&
          !classicHeadsWithTailResult.contains(longNote));
}

bool classicBadHeadSnapshotPrecedesResult(
    bms_parser::Chart &chart,
    const std::unordered_map<std::string, bms_parser::Note *> &lookup,
    const std::unordered_set<const bms_parser::LongNote *>
        &classicHeadsWithTailResult,
    const ReplayEvent &event) {
  if (event.action != ReplayEventAction::Press || event.judgement != Bad) {
    return false;
  }
  auto *note = findReplayNote(lookup, event);
  if (note == nullptr || !note->IsLongNote()) {
    return false;
  }
  auto *longNote = static_cast<bms_parser::LongNote *>(note);
  return !longNote->IsTail() && !effectiveLongNoteIsCharge(longNote, chart) &&
         !classicHeadsWithTailResult.contains(longNote);
}

bool replayCountersMatch(const RhythmState &state,
                         const ReplayEvent &event) noexcept {
  return event.combo == state.combo && event.score == state.getScore();
}

bool replaySnapshotMatches(const RhythmState &state,
                           const ReplayEvent &event) noexcept {
  return replayCountersMatch(state, event) &&
         event.gaugeType == state.gaugeType &&
         std::bit_cast<std::uint32_t>(event.gauge) ==
             std::bit_cast<std::uint32_t>(state.currentGauge);
}

void syncReplayResultGaugeSnapshot(RhythmState &state,
                                   const ReplayEvent &event) {
  state.gaugeType = event.gaugeType;
  state.currentGauge = event.gauge;
  const int gaugeIndex = gaugeTypeIndex(event.gaugeType);
  if (gaugeIndex >= 0 &&
      gaugeIndex < static_cast<int>(state.gaugeValues.size())) {
    state.gaugeValues[gaugeIndex] = event.gauge;
    if (gaugeIsSurvival(event.gaugeType, state.gaugeProfile) &&
        event.gauge <= 0.0f) {
      state.gaugeSurvivalFailed[gaugeIndex] = true;
    }
  }
  if (!state.gaugeHistory.empty()) {
    state.gaugeHistory.back() = event.gauge;
  } else {
    state.gaugeHistory.push_back(event.gauge);
  }
  auto &typedHistory = state.gaugeHistoryFor(event.gaugeType);
  if (!typedHistory.empty()) {
    typedHistory.back() = event.gauge;
  } else {
    typedHistory.push_back(event.gauge);
  }
}
} // namespace

namespace analysis {
RhythmState BuildInitialGaugeState(bms_parser::Chart &chart,
                                   const JudgedPlaybackData &replay,
                                   std::optional<GaugeProfile> gaugeProfile,
                                   const GaugeStateSnapshot *carriedGauge) {
  const auto recordedRuleset = gameplayRulesetFromReplayIdentity(
      replay.setup.playbackRulesetId,
      replay.setup.playbackRulesetRevision);
  if (!recordedRuleset.has_value()) {
    throw std::invalid_argument(
        "Replay result has an unsupported gameplay ruleset identity");
  }
  const GaugeProfile effectiveGaugeProfile =
      gaugeProfile.value_or(replay.setup.gaugeProfile);
  RhythmState state(&chart, false, *recordedRuleset,
                    effectiveGaugeProfile);
  state.configureGauge(replay.setup.initialGaugeType,
                       replay.setup.gaugeAutoShift, effectiveGaugeProfile,
                       replay.setup.gaugeAutoShiftLowerBound);
  if (replay.setup.startingGaugeState.has_value()) {
    state.restoreGaugeState(*replay.setup.startingGaugeState);
  } else if (replay.context.startingGaugePercent.has_value()) {
    state.setStartingGaugePercent(*replay.context.startingGaugePercent);
  }
  if (carriedGauge != nullptr) {
    GaugeStateSnapshot adjustedCarry = *carriedGauge;
    adjustedCarry.gaugeProfile = state.gaugeProfile;
    state.restoreGaugeState(adjustedCarry);
  }
  const audio::PlaybackRate playback{
      .percent = replay.setup.playbackRatePercent,
      .mode = replay.setup.playbackMode,
  };
  state.setAssistClearMark(
      assist_options::isEnabled(replay.setup.assistOption) ||
      clear_policy::assistClearRequired(playback));
  return state;
}

namespace {

std::optional<ValidatedJudgedPlaybackResultState>
buildResultState(bms_parser::Chart &chart, const JudgedPlaybackData &replay,
                 std::optional<GaugeProfile> gaugeProfile,
                 const GaugeStateSnapshot *carriedGauge, int carriedCombo,
                 int carriedMaxCombo, bool validateRecordedSnapshots) {
  const auto lookup = buildReplayNoteLookup(chart);
  const auto classicHeadsWithTailResult =
      classicLongHeadsWithRecordedTailResult(chart, lookup, replay);
  const auto earliestHellChargeHead = earliestHellChargeHeadMicros(chart);
  RhythmState state =
      BuildInitialGaugeState(chart, replay, gaugeProfile, carriedGauge);
  state.combo = std::max(0, carriedCombo);
  state.maxCombo = std::max(state.combo, carriedMaxCombo);
  std::vector<std::pair<GaugeType, float>> recordedGaugeHistory;
  ReplayTimingSources timingSources;

  for (const auto &event : replay.events) {
    if (validateRecordedSnapshots &&
        !replayEventContractMatches(chart, lookup, timingSources,
                                    earliestHellChargeHead, event)) {
      return std::nullopt;
    }
    if (validateRecordedSnapshots) {
      rememberReplayTimingSource(timingSources, event);
    }
    if (event.action == ReplayEventAction::Gauge) {
      if (validateRecordedSnapshots && !replayCountersMatch(state, event)) {
        return std::nullopt;
      }
      if (event.judgement == Great || event.judgement == Bad) {
        state.applyGaugeJudgementRate(event.judgement, 0.5f);
      } else {
        state.gaugeHistory.push_back(event.gauge);
      }
      if (validateRecordedSnapshots && !replaySnapshotMatches(state, event)) {
        return std::nullopt;
      }
      syncReplayResultGaugeSnapshot(state, event);
      if (validateRecordedSnapshots) {
        recordedGaugeHistory.emplace_back(event.gaugeType, event.gauge);
      }
      continue;
    }
    if (event.action == ReplayEventAction::Mine) {
      if (validateRecordedSnapshots && !replayCountersMatch(state, event)) {
        return std::nullopt;
      }
      if (auto *note = findReplayNote(lookup, event);
          note != nullptr && note->IsLandmineNote()) {
        auto *mine = static_cast<bms_parser::LandmineNote *>(note);
        state.applyGaugeDelta(-mine->Damage);
      }
      if (validateRecordedSnapshots && !replaySnapshotMatches(state, event)) {
        return std::nullopt;
      }
      syncReplayResultGaugeSnapshot(state, event);
      if (validateRecordedSnapshots) {
        recordedGaugeHistory.emplace_back(event.gaugeType, event.gauge);
      }
      continue;
    }

    const bool countsInResult = replayEventCountsInResult(
        chart, lookup, classicHeadsWithTailResult, event);
    if (!countsInResult) {
      if (validateRecordedSnapshots && !replaySnapshotMatches(state, event)) {
        return std::nullopt;
      }
      continue;
    }

    const bool snapshotPrecedesResult = classicBadHeadSnapshotPrecedesResult(
        chart, lookup, classicHeadsWithTailResult, event);
    if (validateRecordedSnapshots && snapshotPrecedesResult &&
        !replaySnapshotMatches(state, event)) {
      return std::nullopt;
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
    // A classic head is recorded before any judgement is committed. Migration
    // counts an orphaned Bad head only as a result fallback, so it must not
    // manufacture gauge damage or a hidden survival failure.
    if (!snapshotPrecedesResult) {
      state.applyGaugeJudgement(event.judgement);
    }
    if (validateRecordedSnapshots && !snapshotPrecedesResult &&
        !replaySnapshotMatches(state, event)) {
      return std::nullopt;
    }
    state.combo = event.combo;
    state.maxCombo = std::max(state.maxCombo, event.combo);
    syncReplayResultGaugeSnapshot(state, event);
    if (validateRecordedSnapshots) {
      recordedGaugeHistory.emplace_back(event.gaugeType, event.gauge);
    }
  }

  if (!validateRecordedSnapshots && !replay.events.empty() &&
      state.gaugeHistory.empty()) {
    state.currentGauge = replay.finalGauge;
  }
  std::vector<float> adoptedGaugeHistory;
  if (validateRecordedSnapshots) {
    adoptedGaugeHistory.reserve(recordedGaugeHistory.size());
    for (const auto &[gaugeType, gauge] : recordedGaugeHistory) {
      if (gaugeType == state.gaugeType) {
        adoptedGaugeHistory.push_back(gauge);
      }
    }
  }
  return ValidatedJudgedPlaybackResultState{
      .state = std::move(state),
      .adoptedGaugeHistory = std::move(adoptedGaugeHistory),
  };
}

} // namespace

RhythmState BuildResultState(bms_parser::Chart &chart,
                             const JudgedPlaybackData &replay,
                             std::optional<GaugeProfile> gaugeProfile,
                             const GaugeStateSnapshot *carriedGauge,
                             int carriedCombo, int carriedMaxCombo) {
  return std::move(buildResultState(chart, replay, gaugeProfile, carriedGauge,
                                    carriedCombo, carriedMaxCombo, false)
                       ->state);
}

std::optional<ValidatedJudgedPlaybackResultState>
BuildValidatedResultState(bms_parser::Chart &chart,
                          const JudgedPlaybackData &replay,
                          std::optional<GaugeProfile> gaugeProfile,
                          const GaugeStateSnapshot *carriedGauge,
                          int carriedCombo, int carriedMaxCombo) {
  return buildResultState(chart, replay, gaugeProfile, carriedGauge,
                          carriedCombo, carriedMaxCombo, true);
}

std::optional<long long>
FindGaugeFailureMicros(bms_parser::Chart &chart,
                       const JudgedPlaybackData &replay,
                       std::optional<GaugeProfile> gaugeProfile,
                       const GaugeStateSnapshot *carriedGauge) {
  if (replay.setup.gaugeAutoShift == GaugeAutoShiftMode::Continue) {
    return std::nullopt;
  }
  const RhythmState initialState =
      BuildInitialGaugeState(chart, replay, gaugeProfile, carriedGauge);
  if (initialState.activeGaugeFailed()) {
    return 0LL;
  }
  for (const ReplayEvent &event : replay.events) {
    if (event.gauge <= 0.0f &&
        gaugeIsSurvival(event.gaugeType, initialState.gaugeProfile)) {
      return std::max(0LL, event.songTimeMicros);
    }
  }
  return std::nullopt;
}
} // namespace analysis
