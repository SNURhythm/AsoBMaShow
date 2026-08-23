#include "ReplayResultStateBuilder.h"

#include "CoursePlaySession.h"
#include "scene/play/PlayfieldChartVisualModel.h"

#include <algorithm>
#include <array>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace {
std::unordered_map<std::string, bms_parser::Note *>
buildReplayNoteLookup(bms_parser::Chart &chart) {
  std::unordered_map<std::string, bms_parser::Note *> lookup;
  for (const auto &measure : chart.Measures) {
    for (const auto &timeline : measure->TimeLines) {
      for (auto *note : timeline->Notes) {
        if (note == nullptr) {
          continue;
        }
        lookup[replay_note::key(note->Lane, timeline->Timing)] = note;
      }
      for (auto *note : timeline->LandmineNotes) {
        if (note == nullptr) {
          continue;
        }
        lookup[replay_note::key(note->Lane, timeline->Timing)] = note;
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
  const auto it =
      lookup.find(replay_note::key(event.lane, event.noteTimeMicros));
  return it == lookup.end() ? nullptr : it->second;
}

std::unordered_set<const bms_parser::LongNote *>
classicLongHeadsWithRecordedTailResult(
    bms_parser::Chart &chart,
    const std::unordered_map<std::string, bms_parser::Note *> &lookup,
    const ReplayData &replay) {
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

std::array<SkinJudgeWindow, 5>
replaySkinJudgeWindows(const bms_parser::Chart &chart,
                       const ReplayData &replay) {
  constexpr std::array<Judgement, 5> order = {
      PGreat, Great, Good, Bad, Kpoor};
  std::array<SkinJudgeWindow, 5> result{};
  for (std::size_t index = 0; index < order.size(); ++index) {
    result[index].judgement = order[index];
  }
  const ScoreStageProvenance *stage =
      score_provenance::uniqueStageForChart(replay.provenance, chart.Meta);
  if (stage == nullptr) {
    return result;
  }
  for (const auto &window : stage->effectiveJudgeWindows) {
    if (window.context != gameplay::JudgeWindowContext::Normal) {
      continue;
    }
    const auto found = std::ranges::find(order, window.judgement);
    if (found == order.end()) {
      continue;
    }
    auto &target = result[static_cast<std::size_t>(
        std::distance(order.begin(), found))];
    target.minimumTimingMillis =
        -static_cast<int>(window.lateMicros / 1'000);
    target.maximumTimingMillis =
        -static_cast<int>(window.earlyMicros / 1'000);
  }
  return result;
}

struct ReplayGraphNoteKey {
  long long timeMicros = 0;
  int lane = -1;
  ChartVisualNoteSource source = ChartVisualNoteSource::Playable;

  bool operator==(const ReplayGraphNoteKey &) const = default;
};

struct ReplayGraphNoteKeyHash {
  [[nodiscard]] std::size_t
  operator()(const ReplayGraphNoteKey &key) const noexcept {
    std::size_t hash = std::hash<long long>{}(key.timeMicros);
    hash ^= std::hash<int>{}(key.lane) + 0x9e3779b9U + (hash << 6U) +
            (hash >> 2U);
    hash ^= std::hash<unsigned int>{}(static_cast<unsigned int>(key.source)) +
            0x9e3779b9U + (hash << 6U) + (hash >> 2U);
    return hash;
  }
};

using ReplayGraphNoteLookup =
    std::unordered_map<ReplayGraphNoteKey, const ChartVisualNote *,
                       ReplayGraphNoteKeyHash>;

ReplayGraphNoteLookup
buildReplayGraphNoteLookup(const PlayfieldChartVisualModel &model) {
  std::unordered_map<ChartVisualId, long long> timelineTimes;
  timelineTimes.reserve(model.timelines.size());
  for (const auto &timeline : model.timelines) {
    timelineTimes.emplace(timeline.id, timeline.timeMicros);
  }

  ReplayGraphNoteLookup lookup;
  lookup.reserve(model.notes.size());
  for (const auto &note : model.notes) {
    const auto timeline = timelineTimes.find(note.timelineId);
    if (timeline == timelineTimes.end()) {
      continue;
    }
    // Match GamePlayScene::buildReplayNoteLookup(): later parser/model notes
    // replace earlier notes with the same replay key.
    lookup.insert_or_assign(
        {.timeMicros = timeline->second, .lane = note.lane, .source = note.source},
        &note);
  }
  return lookup;
}

const ChartVisualNote *replayGraphNote(const ReplayGraphNoteLookup &lookup,
                                       const ReplayEvent &event) {
  if (event.noteTimeMicros < 0) {
    return nullptr;
  }
  const ChartVisualNoteSource source =
      event.action == ReplayEventAction::Mine ? ChartVisualNoteSource::Mine
                                              : ChartVisualNoteSource::Playable;
  const auto found = lookup.find(
      {.timeMicros = event.noteTimeMicros, .lane = event.lane, .source = source});
  return found == lookup.end() ? nullptr : found->second;
}
} // namespace

namespace replay_result {
RhythmState BuildInitialGaugeState(bms_parser::Chart &chart,
                                   const ReplayData &replay,
                                   GaugeProfile gaugeProfile,
                                   const GaugeStateSnapshot *carriedGauge) {
  GameplayRuleset ruleset = GameplayRuleset::Beatoraja;
  if (isSupportedRulesetDescriptor(replay.provenance.ruleset)) {
    ruleset = gameplayRulesetFromId(replay.provenance.ruleset.id)
                  .value_or(GameplayRuleset::Beatoraja);
  }
  RhythmState state(&chart, false, ruleset, gaugeProfile);
  state.configureGauge(replay.initialGaugeType, replay.gaugeAutoShift,
                       gaugeProfile, replay.gaugeAutoShiftLowerBound);
  if (replay.provenance.startingGaugePercent.has_value()) {
    state.setStartingGaugePercent(*replay.provenance.startingGaugePercent);
  }
  if (carriedGauge != nullptr) {
    GaugeStateSnapshot adjustedCarry = *carriedGauge;
    adjustedCarry.gaugeProfile = state.gaugeProfile;
    state.restoreGaugeState(adjustedCarry);
  }
  state.setAssistClearMark(clear_policy::assistClearMarkRequired(
      replay.assistOption, chart.Meta.MinBpm, chart.Meta.MaxBpm,
      replay.provenance.playback));
  return state;
}

RhythmState BuildResultState(bms_parser::Chart &chart,
                             const ReplayData &replay,
                             GaugeProfile gaugeProfile,
                             const GaugeStateSnapshot *carriedGauge,
                             int carriedCombo, int carriedMaxCombo) {
  const auto lookup = buildReplayNoteLookup(chart);
  const auto classicHeadsWithTailResult =
      classicLongHeadsWithRecordedTailResult(chart, lookup, replay);
  RhythmState state =
      BuildInitialGaugeState(chart, replay, gaugeProfile, carriedGauge);
  state.combo = std::max(0, carriedCombo);
  state.maxCombo = std::max(state.combo, carriedMaxCombo);

  for (const auto &event : replay.events) {
    if (event.action == ReplayEventAction::Gauge) {
      if (event.judgement == Great || event.judgement == Bad) {
        state.applyGaugeJudgementRate(event.judgement, 0.5f);
      } else {
        state.gaugeHistory.push_back(event.gauge);
      }
      syncReplayResultGaugeSnapshot(state, event);
      continue;
    }
    if (event.action == ReplayEventAction::Mine) {
      if (auto *note = findReplayNote(lookup, event);
          note != nullptr && note->IsLandmineNote()) {
        auto *mine = static_cast<bms_parser::LandmineNote *>(note);
        state.applyGaugeDelta(-mine->Damage);
      }
      syncReplayResultGaugeSnapshot(state, event);
      continue;
    }

    if (!replayEventCountsInResult(chart, lookup, classicHeadsWithTailResult,
                                   event)) {
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

SkinGameplayGraphState BuildSkinGameplayGraphState(
    bms_parser::Chart &chart, const ReplayData &replay,
    const RhythmState &state) {
  const PlayfieldChartVisualModel model =
      buildPlayfieldChartVisualModel(chart, chart.Meta.LnMode);
  auto chartGraph =
      std::make_shared<SkinGameplayChartGraphState>(model.skinGameplayGraph);
  const auto lookup = buildReplayNoteLookup(chart);
  const auto classicHeadsWithTailResult =
      classicLongHeadsWithRecordedTailResult(chart, lookup, replay);
  SkinGameplayGraphAccumulator accumulator(
      model.skinGameplayGraph.judgementNotes,
      model.skinGameplayGraph.judgementDistributionSeconds,
      replaySkinJudgeWindows(chart, replay), 0);
  (void)accumulator.updateGaugeState(state.gaugeValues, state.gaugeType,
                                     state.gaugeRules());

  const ReplayGraphNoteLookup graphNotes = buildReplayGraphNoteLookup(model);
  for (const ReplayEvent &event : replay.events) {
    if (event.action == ReplayEventAction::Gauge ||
        event.action == ReplayEventAction::Mine ||
        !replayEventCountsInResult(chart, lookup,
                                   classicHeadsWithTailResult, event)) {
      continue;
    }
    const ChartVisualNote *note = replayGraphNote(graphNotes, event);
    if (note != nullptr) {
      accumulator.applyJudge(note->id,
                             JudgeResult(event.judgement, event.diffMicros));
    }
  }

  auto dynamic =
      std::make_shared<SkinGameplayDynamicGraphState>(accumulator.state());
  dynamic->gaugeHistories = state.gaugeHistories;
  const int activeGaugeIndex = gaugeTypeIndex(state.gaugeType);
  if (activeGaugeIndex >= 0 &&
      static_cast<std::size_t>(activeGaugeIndex) <
          dynamic->gaugeHistories.size() &&
      dynamic->gaugeHistories[static_cast<std::size_t>(activeGaugeIndex)]
          .empty()) {
    dynamic->gaugeHistories[static_cast<std::size_t>(activeGaugeIndex)] =
        state.gaugeHistory;
  }
  // The immutable replay-state history replaces the accumulator's sampled
  // collection, so force a distinct renderer cache revision.
  ++dynamic->gaugeRevision;
  return {.chart = std::move(chartGraph), .dynamic = std::move(dynamic)};
}

std::optional<long long>
FindGaugeFailureMicros(bms_parser::Chart &chart, const ReplayData &replay,
                       GaugeProfile gaugeProfile,
                       const GaugeStateSnapshot *carriedGauge) {
  if (replay.gaugeAutoShift == GaugeAutoShiftMode::Continue) {
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
} // namespace replay_result
