#pragma once

#include "GameplayRuleset.h"
#include "Judgement.h"

#include <array>
#include <cstdint>
#include <optional>

namespace gameplay {

// Keyboard BAD has the largest rank-scaled edge (320 ms). Empty-POOR and
// ordinary/older snapshots remain covered by the established two-second cap.
[[nodiscard]] inline constexpr std::int64_t maximumRecordedJudgeWindowMagnitude(
    std::optional<int> effectiveRankPercent) noexcept {
  constexpr std::int64_t ordinaryLimit = 2'000'000;
  const std::int64_t extendedLimit =
      std::int64_t{320'000} * effectiveRankPercent.value_or(0) / 100;
  return extendedLimit > ordinaryLimit ? extendedLimit : ordinaryLimit;
}

struct TimingWindow {
  Judgement judgement = None;
  std::int64_t earlyMicros = 0;
  std::int64_t lateMicros = 0;

  bool operator==(const TimingWindow &) const = default;
};

enum class JudgeWindowContext : std::uint8_t {
  Normal,
  Scratch,
  LongNoteTail,
  LongScratchTail,
};

enum class NoteJudgeRole : std::uint8_t {
  Normal,
  Scratch,
  LongNoteHead,
  LongScratchHead,
  LongNoteTail,
  LongScratchTail,
};

enum class CandidateSelectionMode : std::uint8_t {
  LR2, // Legacy LR2 candidate marker; compiled LR2 uses Combo.
  Lowest,
  Combo,
  Duration,
  Score,
};

struct JudgeWindowSet {
  std::array<TimingWindow, 5> windows{};

  bool operator==(const JudgeWindowSet &) const = default;
};

struct GameplayJudgeRules {
  GameplayRuleset ruleset = GameplayRuleset::Beatoraja;
  std::array<JudgeWindowSet, 4> contexts{};
  CandidateSelectionMode candidateSelection = CandidateSelectionMode::Lowest;
  std::int64_t automaticPoorLateMicros = 0;
  int keyMode = 7;
  std::optional<int> effectiveJudgeRankPercent;
  bool comboKpoor = true;
  bool singleMiss = false;
  bool vanishBad = true;
  std::int64_t normalReleaseMarginMicros = 0;
  std::int64_t scratchReleaseMarginMicros = 0;
  bool repeatedKpoor = false;
  bool multiBad = false;
  bool rejectsLateBadForLongNoteHead = false;

  bool operator==(const GameplayJudgeRules &) const = default;
};

[[nodiscard]] JudgeWindowContext
windowContextForRole(NoteJudgeRole role) noexcept;

[[nodiscard]] GameplayJudgeRules compileGameplayJudgeRules(
    GameplayRuleset ruleset, int sourceRank, int playbackRatePercent = 100,
    int judgeScalePercent = 100,
    CourseJudgementConstraint constraint = CourseJudgementConstraint::None,
    CandidateSelectionMode selection = CandidateSelectionMode::LR2,
    int keyMode = 7,
    std::optional<int> rankPercentOverride = std::nullopt);

} // namespace gameplay
