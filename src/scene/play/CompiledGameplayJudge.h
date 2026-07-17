#pragma once

#include "Judgement.h"

#include <array>
#include <cstdint>
#include <optional>

class Judge;

namespace gameplay {

struct TimingWindow {
  Judgement judgement = None;
  std::int64_t earlyMicros = 0;
  std::int64_t lateMicros = 0;
};

class CompiledGameplayJudge {
public:
  static CompiledGameplayJudge from(const Judge &judge);

  [[nodiscard]] JudgeResult judgeAt(std::int64_t noteTimeMicros,
                                    std::int64_t inputTimeMicros) const noexcept;
  [[nodiscard]] std::optional<TimingWindow>
  window(Judgement judgement) const noexcept;
  [[nodiscard]] std::int64_t
  latestHittableNoteTiming(std::int64_t inputTimeMicros) const noexcept;
  [[nodiscard]] std::int64_t latePoorTimingMicros() const noexcept;

private:
  static constexpr std::array<Judgement, 5> kHittableJudgements = {
      PGreat, Great, Good, Bad, Kpoor};
  std::array<TimingWindow, kHittableJudgements.size()> windows_{};
};

} // namespace gameplay
