#include "CompiledGameplayJudge.h"
#include "Judge.h"

#include <algorithm>
#include <cstddef>
#include <limits>

namespace gameplay {
namespace {
constexpr std::size_t contextIndex(JudgeWindowContext context) noexcept {
  return static_cast<std::size_t>(context);
}
} // namespace

CompiledGameplayJudge CompiledGameplayJudge::from(GameplayJudgeRules rules) {
  CompiledGameplayJudge compiled;
  compiled.rules_ = std::move(rules);
  return compiled;
}

CompiledGameplayJudge CompiledGameplayJudge::from(const Judge &judge) {
  constexpr std::array<Judgement, 5> order = {
      PGreat, Great, Good, Bad, Kpoor};
  GameplayJudgeRules rules;
  rules.ruleset = GameplayRuleset::Beatoraja;
  JudgeWindowSet windows;
  for (std::size_t index = 0; index < order.size(); ++index) {
    const Judgement judgement = order[index];
    const auto found = judge.timingWindows.find(judgement);
    windows.windows[index] =
        found == judge.timingWindows.end()
            ? TimingWindow{judgement, 1, 0}
            : TimingWindow{judgement, found->second.first,
                           found->second.second};
  }
  rules.contexts.fill(windows);
  const auto bad = judge.timingWindows.find(Bad);
  rules.automaticPoorLateMicros =
      bad == judge.timingWindows.end() ? 0 : bad->second.second;
  return from(std::move(rules));
}

JudgeResult CompiledGameplayJudge::judgeAt(
    NoteJudgeRole role, std::int64_t noteTimeMicros,
    std::int64_t inputTimeMicros) const noexcept {
  const std::int64_t diff = inputTimeMicros - noteTimeMicros;
  const JudgeWindowContext context = windowContextForRole(role);
  const auto &windows = rules_.contexts[contextIndex(context)].windows;
  for (const auto &candidate : windows) {
    if (candidate.earlyMicros > diff || diff > candidate.lateMicros) {
      continue;
    }
    const bool longNoteHead = role == NoteJudgeRole::LongNoteHead ||
                              role == NoteJudgeRole::LongScratchHead;
    if (candidate.judgement == Bad && longNoteHead &&
        rules_.rejectsLateBadForLongNoteHead) {
      const auto good = window(context, Good);
      if (good.has_value() && diff > good->lateMicros) {
        return JudgeResult(None, diff);
      }
    }
    return JudgeResult(candidate.judgement, diff);
  }
  return JudgeResult(None, diff);
}

JudgeResult CompiledGameplayJudge::judgeAt(
    std::int64_t noteTimeMicros, std::int64_t inputTimeMicros) const noexcept {
  return judgeAt(NoteJudgeRole::Normal, noteTimeMicros, inputTimeMicros);
}

std::optional<TimingWindow> CompiledGameplayJudge::window(
    JudgeWindowContext context, Judgement judgement) const noexcept {
  const auto &windows = rules_.contexts[contextIndex(context)].windows;
  const auto found = std::ranges::find_if(
      windows, [judgement](const TimingWindow &candidate) {
        return candidate.judgement == judgement;
      });
  return found == windows.end() ? std::nullopt
                                : std::optional<TimingWindow>(*found);
}

std::optional<TimingWindow>
CompiledGameplayJudge::window(Judgement judgement) const noexcept {
  return window(JudgeWindowContext::Normal, judgement);
}

std::int64_t CompiledGameplayJudge::latestHittableNoteTiming(
    NoteJudgeRole role, std::int64_t inputTimeMicros) const noexcept {
  const auto &windows =
      rules_.contexts[contextIndex(windowContextForRole(role))].windows;
  std::int64_t earliest = std::numeric_limits<std::int64_t>::max();
  for (const auto &candidate : windows) {
    if (candidate.earlyMicros <= candidate.lateMicros) {
      earliest = std::min(earliest, candidate.earlyMicros);
    }
  }
  return earliest == std::numeric_limits<std::int64_t>::max()
             ? inputTimeMicros
             : inputTimeMicros - earliest;
}

std::int64_t CompiledGameplayJudge::latestHittableNoteTiming(
    std::int64_t inputTimeMicros) const noexcept {
  return latestHittableNoteTiming(NoteJudgeRole::Normal, inputTimeMicros);
}

std::int64_t CompiledGameplayJudge::automaticPoorLateMicros() const noexcept {
  return rules_.automaticPoorLateMicros;
}

std::int64_t CompiledGameplayJudge::latePoorTimingMicros() const noexcept {
  return automaticPoorLateMicros();
}

const GameplayJudgeRules &CompiledGameplayJudge::rules() const noexcept {
  return rules_;
}

} // namespace gameplay
