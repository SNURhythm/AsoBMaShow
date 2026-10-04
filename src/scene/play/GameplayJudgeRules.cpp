#include "GameplayJudgeRules.h"

#include <algorithm>
#include <array>
#include <cstddef>

namespace gameplay {
namespace {

constexpr std::size_t contextIndex(JudgeWindowContext context) noexcept {
  return static_cast<std::size_t>(context);
}

constexpr TimingWindow symmetric(Judgement judgement,
                                 std::int64_t magnitude) noexcept {
  return {judgement, -magnitude, magnitude};
}

// Reference JudgeProperty stores (note - input); our windows use (input - note).
// Tail tables have no empty-POOR judgement. The zero-width placeholder keeps
// replay snapshots rectangular and is shadowed by PGREAT at zero.
JudgeWindowSet makeWindows(std::int64_t pg, std::int64_t gr,
                           std::int64_t gd, std::int64_t earlyBad,
                           std::int64_t lateBad, std::int64_t earlyPoor = 0,
                           std::int64_t latePoor = 0) {
  return {.windows = {symmetric(PGreat, pg), symmetric(Great, gr),
                      symmetric(Good, gd), {Bad, earlyBad, lateBad},
                      {Kpoor, earlyPoor, latePoor}}};
}

int rankPercent(GameplayRuleset ruleset, int rank, int keyMode) {
  if (rank < 0 || rank > 4) {
    rank = 2;
  }
  constexpr std::array normal{25, 50, 75, 100, 125};
  constexpr std::array pms{33, 50, 70, 100, 133};
  constexpr std::array lr2{25, 50, 75, 100, 75};
  return (ruleset == GameplayRuleset::LR2 ? lr2 : keyMode == 9 ? pms : normal)
      [static_cast<std::size_t>(rank)];
}

void scaleWindows(JudgeWindowSet &set, int rank, int playbackRatePercent,
                  int judgeScalePercent, bool pms, bool lr2) {
  constexpr std::array<std::array<std::int64_t, 5>, 3> lr2Scaling{{
      {0, 8000, 15000, 18000, 21000},
      {0, 24000, 30000, 40000, 60000},
      {0, 40000, 60000, 100000, 120000},
  }};
  for (std::size_t i = 0; i < 4; ++i) {
    auto &window = set.windows[i];
    if (lr2) {
      if (i < 3) {
        // Interpolate the LR2 rank table for DEFEXRANK percentages.
        // A 120 ms tail edge follows GOOD scaling at every judgement tier.
        const std::size_t row = window.lateMicros == 120000 ? 2 : i;
        std::int64_t magnitude = 0;
        if (rank >= 100) {
          magnitude = window.lateMicros * rank / 100;
        } else {
          const int boundedRank = std::max(0, rank);
          const std::size_t index = boundedRank / 25;
          const auto low = lr2Scaling[row][index];
          const auto high = lr2Scaling[row][index + 1];
          magnitude = low + (high - low) * (boundedRank % 25) / 25;
        }
        window.earlyMicros = std::max(-magnitude, set.windows[3].earlyMicros);
        window.lateMicros = std::min(magnitude, set.windows[3].lateMicros);
      }
    } else if (!pms || (i != 0 && i != 3)) {
      window.earlyMicros = window.earlyMicros * rank / 100;
      window.lateMicros = window.lateMicros * rank / 100;
      if (pms) {
        window.earlyMicros = std::clamp(window.earlyMicros,
            set.windows[3].earlyMicros, set.windows[0].earlyMicros);
        window.lateMicros = std::clamp(window.lateMicros,
            set.windows[0].lateMicros, set.windows[3].lateMicros);
      }
    }
  }
  // Custom judge rates affect PG/GREAT/GOOD only, with Java integer truncation.
  const auto bad = set.windows[3];
  for (std::size_t i = 0; i < 3; ++i) {
    auto &window = set.windows[i];
    window.earlyMicros = std::max(bad.earlyMicros,
        window.earlyMicros * playbackRatePercent * judgeScalePercent / 10000);
    window.lateMicros = std::min(bad.lateMicros,
        window.lateMicros * playbackRatePercent * judgeScalePercent / 10000);
    if (i > 0) {
      window.earlyMicros = std::min(window.earlyMicros,
                                   set.windows[i - 1].earlyMicros);
      window.lateMicros = std::max(window.lateMicros,
                                  set.windows[i - 1].lateMicros);
    }
  }
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
    CandidateSelectionMode selection, int keyMode,
    std::optional<int> rankPercentOverride) {

  GameplayJudgeRules result;
  result.ruleset = ruleset;
  result.keyMode = keyMode;
  result.effectiveJudgeRankPercent = rankPercentOverride;
  const bool lr2 = ruleset == GameplayRuleset::LR2;
  const bool pms = !lr2 && keyMode == 9;
  const bool fiveKeys = !lr2 && (keyMode == 5 || keyMode == 10);
  const bool keyboard = !lr2 && (keyMode == 24 || keyMode == 48);
  auto &normal = result.contexts[contextIndex(JudgeWindowContext::Normal)];
  auto &scratch = result.contexts[contextIndex(JudgeWindowContext::Scratch)];
  auto &tail = result.contexts[contextIndex(JudgeWindowContext::LongNoteTail)];
  auto &scratchTail = result.contexts[contextIndex(JudgeWindowContext::LongScratchTail)];
  if (lr2) {
    normal = makeWindows(21000, 60000, 120000, -200000, 200000, -1000000, 0);
    tail = makeWindows(120000, 120000, 120000, -200000, 200000);
    scratch = normal;
    scratchTail = tail;
  } else if (fiveKeys) {
    normal = makeWindows(20000, 50000, 100000, -150000, 150000, -500000, 150000);
    scratch = makeWindows(30000, 60000, 110000, -160000, 160000, -500000, 160000);
    tail = makeWindows(120000, 150000, 200000, -250000, 250000);
    scratchTail = makeWindows(130000, 160000, 110000, -260000, 260000);
  } else if (pms) {
    normal = makeWindows(20000, 50000, 117000, -183000, 183000, -500000, 175000);
    tail = makeWindows(120000, 150000, 217000, -283000, 283000);
    scratch = normal;
    scratchTail = tail;
  } else if (keyboard) {
    normal = makeWindows(30000, 90000, 200000, -240000, 320000, -650000, 200000);
    tail = makeWindows(160000, 200000, 260000, -240000, 320000);
    tail.windows[0].earlyMicros = -25000;
    tail.windows[1].earlyMicros = -75000;
    tail.windows[2].earlyMicros = -140000;
    scratch = normal;
    scratchTail = tail;
  } else {
    normal = makeWindows(20000, 60000, 150000, -220000, 280000, -500000, 150000);
    scratch = makeWindows(30000, 70000, 160000, -230000, 290000, -500000, 160000);
    tail = makeWindows(120000, 160000, 200000, -220000, 280000);
    scratchTail = makeWindows(130000, 170000, 210000, -230000, 290000);
  }
  for (JudgeWindowSet &context : result.contexts) {
    scaleWindows(context, rankPercentOverride.value_or(
                              rankPercent(ruleset, sourceRank, keyMode)),
                  playbackRatePercent, judgeScalePercent, pms, lr2);
    applyConstraint(context, constraint);
  }
  result.candidateSelection = selection == CandidateSelectionMode::LR2
      ? (lr2 ? CandidateSelectionMode::Combo : CandidateSelectionMode::Lowest)
      : selection;
  result.automaticPoorLateMicros = normal.windows[3].lateMicros;
  result.comboKpoor = !fiveKeys && !pms;
  result.singleMiss = pms;
  result.vanishBad = !pms;
  result.normalReleaseMarginMicros = pms ? 200000 : 0;
  result.repeatedKpoor = !pms;
  result.multiBad = lr2;
  result.rejectsLateBadForLongNoteHead = lr2;
  return result;
}

} // namespace gameplay
