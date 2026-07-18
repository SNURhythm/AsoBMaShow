#include "GameplayJudgeRules.h"

#include "Judge.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <map>
#include <utility>

namespace gameplay {
namespace {

constexpr std::array<Judgement, 5> kWindowOrder = {
    PGreat, Great, Good, Bad, Kpoor};

constexpr std::size_t contextIndex(JudgeWindowContext context) noexcept {
  return static_cast<std::size_t>(context);
}

constexpr TimingWindow symmetric(Judgement judgement,
                                 std::int64_t magnitude) noexcept {
  return {judgement, -magnitude, magnitude};
}

JudgeWindowSet lr2NormalWindows(int rank) {
  struct RankWindows {
    std::int64_t pgreat;
    std::int64_t great;
    std::int64_t good;
  };
  constexpr std::array<RankWindows, 5> ranks = {{
      {8000, 24000, 40000},
      {15000, 30000, 60000},
      {18000, 40000, 100000},
      {21000, 60000, 120000},
      {18000, 40000, 100000},
  }};
  if (rank < 0 || rank >= static_cast<int>(ranks.size())) {
    rank = 2;
  }
  const RankWindows selected = ranks[static_cast<std::size_t>(rank)];
  return {.windows = {
              symmetric(PGreat, selected.pgreat),
              symmetric(Great, selected.great),
              symmetric(Good, selected.good),
              symmetric(Bad, 200000),
              {Kpoor, -1000000, 0},
          }};
}

JudgeWindowSet lr2TailWindows() {
  return {.windows = {
              symmetric(PGreat, 120000),
              symmetric(Great, 120000),
              symmetric(Good, 120000),
              symmetric(Bad, 200000),
              {Kpoor, -1000000, 0},
          }};
}

JudgeWindowSet windowsFromMap(
    const std::map<Judgement, std::pair<long long, long long>> &source) {
  JudgeWindowSet result;
  for (std::size_t index = 0; index < kWindowOrder.size(); ++index) {
    const Judgement judgement = kWindowOrder[index];
    const auto found = source.find(judgement);
    result.windows[index] =
        found == source.end()
            ? TimingWindow{judgement, 1, 0}
            : TimingWindow{judgement, found->second.first,
                           found->second.second};
  }
  return result;
}

std::int64_t scaleWindowEdge(std::int64_t value, int playbackRatePercent,
                             int judgeScalePercent) noexcept {
  constexpr std::int64_t denominator = 10000;
  const std::int64_t numerator =
      value * static_cast<std::int64_t>(playbackRatePercent) *
      static_cast<std::int64_t>(judgeScalePercent);
  constexpr std::int64_t roundingOffset = denominator / 2;
  return numerator >= 0 ? (numerator + roundingOffset) / denominator
                        : (numerator - roundingOffset) / denominator;
}

void scaleLr2Windows(JudgeWindowSet &set, int playbackRatePercent,
                     int judgeScalePercent) {
  const TimingWindow &bad = set.windows[3];
  for (std::size_t index = 0; index < 3; ++index) {
    TimingWindow &window = set.windows[index];
    window.earlyMicros =
        std::max(bad.earlyMicros,
                 scaleWindowEdge(window.earlyMicros, playbackRatePercent,
                                 judgeScalePercent));
    window.lateMicros =
        std::min(bad.lateMicros,
                 scaleWindowEdge(window.lateMicros, playbackRatePercent,
                                 judgeScalePercent));
  }

  set.windows[1].earlyMicros =
      std::min(set.windows[1].earlyMicros, set.windows[0].earlyMicros);
  set.windows[2].earlyMicros =
      std::min(set.windows[2].earlyMicros, set.windows[1].earlyMicros);
  set.windows[1].lateMicros =
      std::max(set.windows[1].lateMicros, set.windows[0].lateMicros);
  set.windows[2].lateMicros =
      std::max(set.windows[2].lateMicros, set.windows[1].lateMicros);
}

void applyConstraint(JudgeWindowSet &set,
                     CourseJudgementConstraint constraint) {
  const auto copyEdges = [&set](std::size_t target, std::size_t source) {
    set.windows[target].earlyMicros = set.windows[source].earlyMicros;
    set.windows[target].lateMicros = set.windows[source].lateMicros;
  };
  switch (constraint) {
  case CourseJudgementConstraint::NoGood:
    copyEdges(2, 1);
    return;
  case CourseJudgementConstraint::NoGreat:
    copyEdges(1, 0);
    copyEdges(2, 0);
    return;
  case CourseJudgementConstraint::None:
    return;
  }
}

} // namespace

JudgeWindowContext windowContextForRole(NoteJudgeRole role) noexcept {
  switch (role) {
  case NoteJudgeRole::Normal:
  case NoteJudgeRole::LongNoteHead:
    return JudgeWindowContext::Normal;
  case NoteJudgeRole::Scratch:
  case NoteJudgeRole::LongScratchHead:
    return JudgeWindowContext::Scratch;
  case NoteJudgeRole::LongNoteTail:
    return JudgeWindowContext::LongNoteTail;
  case NoteJudgeRole::LongScratchTail:
    return JudgeWindowContext::LongScratchTail;
  }
  return JudgeWindowContext::Normal;
}

GameplayJudgeRules compileGameplayJudgeRules(
    GameplayRuleset ruleset, int sourceRank, int playbackRatePercent,
    int judgeScalePercent, CourseJudgementConstraint constraint,
    CandidateSelectionMode beatorajaSelection) {
  GameplayJudgeRules result;
  result.ruleset = ruleset;

  if (ruleset == GameplayRuleset::Beatoraja) {
    Judge judge(sourceRank);
    judge.applyCourseJudgementConstraint(constraint);
    judge.applyWindowScale(playbackRatePercent, judgeScalePercent);
    const JudgeWindowSet windows = windowsFromMap(judge.timingWindows);
    result.contexts.fill(windows);
    result.candidateSelection = beatorajaSelection;
    result.automaticPoorLateMicros = judge.timingWindows.at(Bad).second;
    return result;
  }

  const JudgeWindowSet normal = lr2NormalWindows(sourceRank);
  const JudgeWindowSet tail = lr2TailWindows();
  result.contexts[contextIndex(JudgeWindowContext::Normal)] = normal;
  result.contexts[contextIndex(JudgeWindowContext::Scratch)] = normal;
  result.contexts[contextIndex(JudgeWindowContext::LongNoteTail)] = tail;
  result.contexts[contextIndex(JudgeWindowContext::LongScratchTail)] = tail;
  for (JudgeWindowSet &context : result.contexts) {
    scaleLr2Windows(context, playbackRatePercent, judgeScalePercent);
    applyConstraint(context, constraint);
  }
  result.candidateSelection = CandidateSelectionMode::LR2;
  result.automaticPoorLateMicros = 200000;
  result.repeatedKpoor = true;
  result.multiBad = true;
  result.rejectsLateBadForLongNoteHead = true;
  return result;
}

} // namespace gameplay
