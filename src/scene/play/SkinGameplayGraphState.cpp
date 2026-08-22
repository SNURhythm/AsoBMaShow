#include "SkinGameplayGraphState.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace {
void advanceRevision(std::uint64_t &value) noexcept {
  value = value == std::numeric_limits<std::uint64_t>::max() ? 1U : value + 1U;
}
} // namespace

SkinGameplayGraphStateView
skinGameplayGraphStateView(const SkinGameplayGraphState &state) noexcept {
  SkinGameplayGraphStateView view;
  if (state.chart != nullptr) {
    view.normalDistribution = state.chart->normalDistribution;
    view.bpmSeries = state.chart->bpmSeries;
    view.mainBpm = state.chart->mainBpm;
    view.minimumBpm = state.chart->minimumBpm;
    view.maximumBpm = state.chart->maximumBpm;
  }
  if (state.dynamic != nullptr) {
    view.judgementDistribution = state.dynamic->judgementDistribution;
    view.earlyLateDistribution = state.dynamic->earlyLateDistribution;
    view.judgeWindows = state.dynamic->judgeWindows;
    view.recentJudgeTimingsMillis = state.dynamic->recentJudgeTimingsMillis;
    view.recentJudgeTimingIndex = state.dynamic->recentJudgeTimingIndex;
    view.judgementRevision = state.dynamic->judgementRevision;
    const int gaugeIndex = gaugeTypeIndex(state.dynamic->gaugeType);
    if (gaugeIndex >= 0 &&
        static_cast<std::size_t>(gaugeIndex) <
            state.dynamic->gaugeHistories.size()) {
      view.gaugeHistory =
          state.dynamic->gaugeHistories[static_cast<std::size_t>(gaugeIndex)];
    }
    view.gaugeType = gaugeIndex;
    view.gaugeMinimum = state.dynamic->gaugeMinimum;
    view.gaugeMaximum = state.dynamic->gaugeMaximum;
    view.gaugeBorder = state.dynamic->gaugeBorder;
    view.gaugeSupported = state.dynamic->gaugeSupported;
    view.gaugeRevision = state.dynamic->gaugeRevision;
  }
  return view;
}

SkinGameplayGraphAccumulator::SkinGameplayGraphAccumulator(
    std::vector<SkinGameplayGraphNote> notes, std::size_t secondCount,
    std::array<SkinJudgeWindow, 5> judgeWindows,
    std::size_t gaugeHistoryCapacity) {
  reset(std::move(notes), secondCount, judgeWindows, gaugeHistoryCapacity);
}

void SkinGameplayGraphAccumulator::reset(
    std::vector<SkinGameplayGraphNote> notes, std::size_t secondCount,
    std::array<SkinJudgeWindow, 5> judgeWindows,
    std::size_t gaugeHistoryCapacity) {
  state_ = {};
  state_.judgeWindows = judgeWindows;
  state_.judgementDistribution.assign(secondCount, {});
  state_.earlyLateDistribution.assign(secondCount, {});
  for (auto &history : state_.gaugeHistories) {
    history.reserve(gaugeHistoryCapacity);
  }
  gaugeHistoryCapacity_ = gaugeHistoryCapacity;
  gaugeValues_ = {};
  nextGaugeSampleMicros_ = 0;
  gaugeValuesInitialized_ = false;

  notes_.clear();
  notes_.reserve(notes.size());
  noteIndices_.clear();
  noteIndices_.reserve(notes.size());
  for (auto &note : notes) {
    const std::size_t index = notes_.size();
    noteIndices_.insert_or_assign(note.sourceId, index);
    notes_.push_back({.definition = std::move(note)});
    const auto &stored = notes_.back().definition;
    if (!stored.countsTowardJudgement || stored.second < 0 ||
        static_cast<std::size_t>(stored.second) >= secondCount) {
      continue;
    }
    ++state_.judgementDistribution[stored.second][0];
    ++state_.earlyLateDistribution[stored.second][0];
  }
}

SkinGameplayGraphAccumulator::NoteState *
SkinGameplayGraphAccumulator::resolvedNote(std::uint32_t sourceId) noexcept {
  const auto found = noteIndices_.find(sourceId);
  if (found == noteIndices_.end()) {
    return nullptr;
  }
  NoteState *note = &notes_[found->second];
  if (note->definition.countsTowardJudgement) {
    return note;
  }
  if (note->definition.redirectSourceId ==
      kInvalidSkinGameplayGraphSourceId) {
    return nullptr;
  }
  const auto redirect = noteIndices_.find(note->definition.redirectSourceId);
  return redirect == noteIndices_.end() ? nullptr : &notes_[redirect->second];
}

int SkinGameplayGraphAccumulator::judgeState(
    Judgement judgement) noexcept {
  switch (judgement) {
  case PGreat:
    return 1;
  case Great:
    return 2;
  case Good:
    return 3;
  case Bad:
    return 4;
  case Poor:
    return 5;
  case Kpoor:
  case None:
  case JudgementCount:
    return -1;
  }
  return -1;
}

int SkinGameplayGraphAccumulator::earlyLateBucket(
    int state, std::int64_t playTimeMillis) noexcept {
  if (state <= 1) {
    return state;
  }
  return playTimeMillis >= 0 ? state : state + 4;
}

void SkinGameplayGraphAccumulator::applyJudge(
    std::uint32_t sourceId, const JudgeResult &judge) {
  const int nextState = judgeState(judge.judgement);
  if (nextState < 0) {
    return;
  }

  NoteState *note = resolvedNote(sourceId);
  if (note == nullptr || note->definition.second < 0 ||
      static_cast<std::size_t>(note->definition.second) >=
          state_.judgementDistribution.size()) {
    return;
  }
  const std::int64_t nextPlayTimeMillis = -(judge.Diff / 1000);
  const int previousEarlyLate =
      earlyLateBucket(note->state, note->playTimeMillis);
  const int nextEarlyLate = earlyLateBucket(nextState, nextPlayTimeMillis);
  auto &judgements =
      state_.judgementDistribution[note->definition.second];
  auto &earlyLate =
      state_.earlyLateDistribution[note->definition.second];
  --judgements[static_cast<std::size_t>(note->state)];
  ++judgements[static_cast<std::size_t>(nextState)];
  --earlyLate[static_cast<std::size_t>(previousEarlyLate)];
  ++earlyLate[static_cast<std::size_t>(nextEarlyLate)];
  note->state = nextState;
  note->playTimeMillis = nextPlayTimeMillis;

  if (judge.judgement >= PGreat && judge.judgement <= Bad) {
    state_.recentJudgeTimingIndex =
        (state_.recentJudgeTimingIndex + 1) %
        state_.recentJudgeTimingsMillis.size();
    state_.recentJudgeTimingsMillis[state_.recentJudgeTimingIndex] =
        nextPlayTimeMillis;
  }
  advanceRevision(state_.judgementRevision);
}

bool SkinGameplayGraphAccumulator::setGauge(
    GaugeType type, const GameplayGaugeRules &rules) noexcept {
  const auto before = std::array{
      state_.gaugeMinimum, state_.gaugeMaximum, state_.gaugeBorder};
  const GaugeType beforeType = state_.gaugeType;
  const bool beforeSupported = state_.gaugeSupported;
  state_.gaugeType = type;
  const int index = gaugeTypeIndex(type);
  if (!rules.compiled || index < 0 ||
      static_cast<std::size_t>(index) >= rules.gauges.size()) {
    state_.gaugeSupported = false;
    const bool changed = beforeType != state_.gaugeType || beforeSupported;
    if (changed) advanceRevision(state_.gaugeRevision);
    return changed;
  }
  const auto &gauge = rules.gauges[static_cast<std::size_t>(index)];
  state_.gaugeMinimum = gauge.minimum;
  state_.gaugeMaximum = gauge.maximum;
  state_.gaugeBorder = gauge.clearBorder;
  state_.gaugeSupported = true;
  const bool changed = beforeType != state_.gaugeType || !beforeSupported ||
                       before != std::array{state_.gaugeMinimum,
                                            state_.gaugeMaximum,
                                            state_.gaugeBorder};
  if (changed) advanceRevision(state_.gaugeRevision);
  return changed;
}

bool SkinGameplayGraphAccumulator::updateGaugeState(
    const std::array<float, kGaugeTypeCount> &values, GaugeType type,
    const GameplayGaugeRules &rules) {
  bool changed = setGauge(type, rules);
  changed = changed || !gaugeValuesInitialized_ || gaugeValues_ != values;
  gaugeValues_ = values;
  if (!gaugeValuesInitialized_) {
    gaugeValuesInitialized_ = true;
    changed = advanceGaugeHistoryTo(0) || changed;
  }
  return changed;
}

bool SkinGameplayGraphAccumulator::advanceGaugeHistoryTo(
    std::int64_t playTimeMicros) {
  constexpr std::int64_t sampleIntervalMicros = 500'000;
  if (!gaugeValuesInitialized_ || playTimeMicros < nextGaugeSampleMicros_) {
    return false;
  }

  const std::uint64_t due = static_cast<std::uint64_t>(
      (playTimeMicros - nextGaugeSampleMicros_) / sampleIntervalMicros + 1);
  const std::size_t existing = state_.gaugeHistories.front().size();
  const std::size_t available =
      existing < gaugeHistoryCapacity_ ? gaugeHistoryCapacity_ - existing : 0;
  const std::size_t appended = static_cast<std::size_t>(
      std::min<std::uint64_t>(due, available));
  for (std::size_t type = 0; type < state_.gaugeHistories.size(); ++type) {
    auto &history = state_.gaugeHistories[type];
    history.insert(history.end(), appended, gaugeValues_[type]);
  }
  if (appended != 0) advanceRevision(state_.gaugeRevision);

  const auto remaining =
      std::numeric_limits<std::int64_t>::max() - nextGaugeSampleMicros_;
  if (due > static_cast<std::uint64_t>(remaining / sampleIntervalMicros)) {
    nextGaugeSampleMicros_ = std::numeric_limits<std::int64_t>::max();
  } else {
    nextGaugeSampleMicros_ +=
        static_cast<std::int64_t>(due) * sampleIntervalMicros;
  }
  return appended != 0;
}
