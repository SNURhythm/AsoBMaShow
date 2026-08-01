#pragma once

#include "GameplayJudgeRules.h"

#include <array>
#include <cstdint>
#include <optional>

class Judge;

namespace gameplay {

class CompiledGameplayJudge {
public:
  static CompiledGameplayJudge from(GameplayJudgeRules rules);
  static CompiledGameplayJudge from(const Judge &judge);

  [[nodiscard]] JudgeResult judgeAt(NoteJudgeRole role,
                                    std::int64_t noteTimeMicros,
                                    std::int64_t inputTimeMicros) const noexcept;
  [[nodiscard]] JudgeResult judgeAt(std::int64_t noteTimeMicros,
                                    std::int64_t inputTimeMicros) const noexcept;
  [[nodiscard]] std::optional<TimingWindow>
  window(JudgeWindowContext context, Judgement judgement) const noexcept;
  [[nodiscard]] std::optional<TimingWindow>
  window(Judgement judgement) const noexcept;
  [[nodiscard]] std::int64_t latestHittableNoteTiming(
      NoteJudgeRole role, std::int64_t inputTimeMicros) const noexcept;
  [[nodiscard]] std::int64_t
  latestHittableNoteTiming(std::int64_t inputTimeMicros) const noexcept;
  [[nodiscard]] std::int64_t automaticPoorLateMicros() const noexcept;
  [[nodiscard]] std::int64_t latePoorTimingMicros() const noexcept;
  [[nodiscard]] const GameplayJudgeRules &rules() const noexcept;

private:
  GameplayJudgeRules rules_;
};

} // namespace gameplay
