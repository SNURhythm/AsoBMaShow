#pragma once

#include "GameplayRuleset.h"
#include "Judgement.h"

#include <array>
#include <cstdint>

namespace gameplay {

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
  LR2,
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
  bool repeatedKpoor = false;
  bool multiBad = false;
  bool rejectsLateBadForLongNoteHead = false;
};

[[nodiscard]] JudgeWindowContext
windowContextForRole(NoteJudgeRole role) noexcept;

[[nodiscard]] GameplayJudgeRules compileGameplayJudgeRules(
    GameplayRuleset ruleset, int sourceRank, int playbackRatePercent = 100,
    int judgeScalePercent = 100,
    CourseJudgementConstraint constraint = CourseJudgementConstraint::None,
    CandidateSelectionMode beatorajaSelection =
        CandidateSelectionMode::Lowest);

} // namespace gameplay
