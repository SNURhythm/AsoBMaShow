#include "CompiledGameplayJudge.h"
#include "Judge.h"

#include <algorithm>
#include <limits>

namespace gameplay {

CompiledGameplayJudge CompiledGameplayJudge::from(const Judge &judge) {
  CompiledGameplayJudge compiled;
  for (std::size_t index = 0; index < kHittableJudgements.size(); ++index) {
    const Judgement judgement = kHittableJudgements[index];
    const auto source = judge.timingWindows.find(judgement);
    if (source == judge.timingWindows.end()) {
      compiled.windows_[index] = {judgement, 1, 0};
      continue;
    }
    compiled.windows_[index] = {
        judgement, source->second.first, source->second.second};
  }
  return compiled;
}

JudgeResult CompiledGameplayJudge::judgeAt(
    std::int64_t noteTimeMicros, std::int64_t inputTimeMicros) const noexcept {
  const std::int64_t diff = inputTimeMicros - noteTimeMicros;
  for (const auto &window : windows_) {
    if (window.earlyMicros <= diff && diff <= window.lateMicros) {
      return JudgeResult(window.judgement, diff);
    }
  }
  return JudgeResult(None, diff);
}

std::optional<TimingWindow>
CompiledGameplayJudge::window(Judgement judgement) const noexcept {
  const auto found = std::ranges::find_if(
      windows_, [judgement](const TimingWindow &candidate) {
        return candidate.judgement == judgement;
      });
  return found == windows_.end() ? std::nullopt
                                 : std::optional<TimingWindow>(*found);
}

std::int64_t CompiledGameplayJudge::latestHittableNoteTiming(
    std::int64_t inputTimeMicros) const noexcept {
  std::int64_t earliest = std::numeric_limits<std::int64_t>::max();
  for (const auto &window : windows_) {
    if (window.earlyMicros <= window.lateMicros) {
      earliest = std::min(earliest, window.earlyMicros);
    }
  }
  return earliest == std::numeric_limits<std::int64_t>::max()
             ? inputTimeMicros
             : inputTimeMicros - earliest;
}

std::int64_t CompiledGameplayJudge::latePoorTimingMicros() const noexcept {
  const auto bad = window(Bad);
  return bad.has_value() ? bad->lateMicros : 0;
}

} // namespace gameplay
