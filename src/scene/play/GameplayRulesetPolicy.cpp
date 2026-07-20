#include "GameplayRulesetPolicy.h"

#include "../../CoursePlaySession.h"
#include "../../bms_parser.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

namespace gameplay {
namespace {
constexpr std::int64_t kMaximumReplayWindowMagnitude = 2'000'000;
constexpr std::array<Judgement, 5> kRequiredJudgements{
    PGreat, Great, Good, Bad, Kpoor};
constexpr std::array<JudgeWindowContext, 4> kRequiredContexts{
    JudgeWindowContext::Normal,
    JudgeWindowContext::Scratch,
    JudgeWindowContext::LongNoteTail,
    JudgeWindowContext::LongScratchTail,
};

GameplayPolicyBuildOutcome failure(GameplayPolicyBuildStatus status,
                                   std::string diagnostic) {
  return {.status = status,
          .policy = std::nullopt,
          .diagnostic = std::move(diagnostic)};
}

bool chartIdentityMatches(const ScoreStageProvenance &stage,
                          const bms_parser::ChartMeta &meta) noexcept {
  const bool md5Matches = stage.chartMd5.empty() || meta.MD5.empty() ||
                          stage.chartMd5 == meta.MD5;
  const bool shaMatches = stage.chartSha256.empty() || meta.SHA256.empty() ||
                          stage.chartSha256 == meta.SHA256;
  return md5Matches && shaMatches;
}

std::optional<std::array<JudgeWindowSet, kRequiredContexts.size()>>
validatedReplayWindows(const ScoreStageProvenance &snapshot) {
  if (snapshot.effectiveJudgeWindows.size() !=
      kRequiredContexts.size() * kRequiredJudgements.size()) {
    return std::nullopt;
  }

  std::array<JudgeWindowSet, kRequiredContexts.size()> result{};
  std::array<bool, kRequiredContexts.size() * kRequiredJudgements.size()>
      found{};
  for (const auto &recorded : snapshot.effectiveJudgeWindows) {
    const auto context =
        std::ranges::find(kRequiredContexts, recorded.context);
    const auto expected = std::ranges::find(kRequiredJudgements,
                                            recorded.judgement);
    if (context == kRequiredContexts.end() ||
        expected == kRequiredJudgements.end()) {
      return std::nullopt;
    }
    const std::size_t contextIndex = static_cast<std::size_t>(
        std::distance(kRequiredContexts.begin(), context));
    const std::size_t judgementIndex = static_cast<std::size_t>(
        std::distance(kRequiredJudgements.begin(), expected));
    const std::size_t index =
        contextIndex * kRequiredJudgements.size() + judgementIndex;
    if (found[index] || recorded.earlyMicros > 0 ||
        recorded.lateMicros < 0 ||
        recorded.earlyMicros > recorded.lateMicros ||
        recorded.earlyMicros < -kMaximumReplayWindowMagnitude ||
        recorded.lateMicros > kMaximumReplayWindowMagnitude) {
      return std::nullopt;
    }
    found[index] = true;
    result[contextIndex].windows[judgementIndex] = {
        recorded.judgement, recorded.earlyMicros, recorded.lateMicros};
  }
  if (!std::ranges::all_of(found, [](bool value) { return value; })) {
    return std::nullopt;
  }
  return result;
}

std::optional<GameplayJudgeRules> compileJudge(
    const bms_parser::ChartMeta &meta,
    const GameplayRulesetPolicyBuildInput &input,
    std::string &diagnostic) {
  GameplayJudgeRules rules = compileGameplayJudgeRules(
      input.ruleset, input.sourceRank, input.playbackRatePercent,
      input.judgeScalePercent, input.courseJudgement,
      input.beatorajaCandidateSelection);
  if (!input.replaySnapshot.has_value()) {
    return rules;
  }

  const auto &snapshot = *input.replaySnapshot;
  if (!chartIdentityMatches(snapshot, meta)) {
    diagnostic = "Replay policy snapshot does not match this chart.";
    return std::nullopt;
  }
  const auto windows = validatedReplayWindows(snapshot);
  if (!windows.has_value()) {
    diagnostic = "Replay policy snapshot has incomplete or unsafe windows.";
    return std::nullopt;
  }
  rules.contexts = *windows;
  rules.candidateSelection = snapshot.candidateSelection;
  const auto &normal =
      windows->at(static_cast<std::size_t>(JudgeWindowContext::Normal));
  const auto bad = std::ranges::find_if(
      normal.windows,
      [](const TimingWindow &window) { return window.judgement == Bad; });
  if (bad != normal.windows.end() &&
      input.ruleset == GameplayRuleset::Beatoraja) {
    rules.automaticPoorLateMicros = bad->lateMicros;
  }
  return rules;
}
} // namespace

GameplayPolicyBuildOutcome buildGameplayRulesetPolicy(
    const bms_parser::ChartMeta &meta,
    const GameplayRulesetPolicyBuildInput &input) {
  const RulesetDescriptor expected = RulesetDescriptor::For(input.ruleset);
  if (input.requiredDescriptor.has_value() &&
      (!isSupportedRulesetDescriptor(*input.requiredDescriptor) ||
       *input.requiredDescriptor != expected)) {
    return failure(GameplayPolicyBuildStatus::UnsupportedRuleset,
                   "The recorded gameplay ruleset is not supported.");
  }
  if (meta.TotalNotes <= 0) {
    return failure(GameplayPolicyBuildStatus::InvalidChart,
                   "The chart has no playable notes.");
  }
  if (meta.HasTotal && !std::isfinite(meta.Total)) {
    return failure(GameplayPolicyBuildStatus::InvalidChart,
                   "The chart gauge TOTAL is not finite.");
  }
  if (input.playbackRatePercent <= 0 || input.judgeScalePercent <= 0) {
    return failure(GameplayPolicyBuildStatus::InvalidChart,
                   "The gameplay timing scale is invalid.");
  }

  const GameplayJudgeRules canonicalJudge = compileGameplayJudgeRules(
      input.ruleset, input.sourceRank, input.playbackRatePercent,
      input.judgeScalePercent, input.courseJudgement,
      input.beatorajaCandidateSelection);
  std::string replayDiagnostic;
  const auto judgeRules = compileJudge(meta, input, replayDiagnostic);
  if (!judgeRules.has_value()) {
    return failure(GameplayPolicyBuildStatus::InvalidReplaySnapshot,
                   std::move(replayDiagnostic));
  }
  const GameplayGaugeRules canonicalGauge = compileGameplayGaugeRules(
      input.ruleset, meta, input.gaugeProfile);
  GameplayGaugeRules gauge = canonicalGauge;
  if (input.replaySnapshot.has_value()) {
    const auto &snapshot = *input.replaySnapshot;
    if (snapshot.totalNotes <= 0 ||
        !std::isfinite(snapshot.effectiveGaugeTotal) ||
        snapshot.effectiveGaugeTotal <= 0.0 ||
        (snapshot.authoredGaugeTotal.has_value() &&
         !std::isfinite(*snapshot.authoredGaugeTotal))) {
      return failure(GameplayPolicyBuildStatus::InvalidReplaySnapshot,
                     "Replay policy snapshot has an unsafe gauge proof.");
    }
    gauge.totalNotes = snapshot.totalNotes;
    gauge.effectiveTotal = snapshot.effectiveGaugeTotal;
  }
  if (!gauge.compiled || gauge.ruleset != input.ruleset ||
      !std::isfinite(gauge.effectiveTotal) ||
      gauge.effectiveTotal <= 0.0) {
    return failure(GameplayPolicyBuildStatus::InvalidChart,
                   "The chart gauge policy could not be compiled.");
  }

  bool policyCanonical =
      *judgeRules == canonicalJudge && gauge == canonicalGauge;
  if (input.replaySnapshot.has_value()) {
    const std::optional<double> authoredTotal =
        meta.HasTotal ? std::optional<double>(meta.Total) : std::nullopt;
    policyCanonical =
        policyCanonical &&
        input.replaySnapshot->authoredGaugeTotal == authoredTotal;
  }
  GameplayRulesetPolicy policy{
      .id = input.ruleset,
      .descriptor = expected,
      .judge = CompiledGameplayJudge::from(*judgeRules),
      .gauge = std::move(gauge),
      .canonical = policyCanonical,
  };
  if (policy.judge.rules().ruleset != policy.id ||
      policy.gauge.ruleset != policy.id) {
    return failure(GameplayPolicyBuildStatus::InvalidChart,
                   "The gameplay policy contains mixed rulesets.");
  }
  return {.status = GameplayPolicyBuildStatus::Built,
          .policy = std::move(policy),
          .diagnostic = {}};
}

bool courseSessionAcceptsPolicy(const CoursePlaySession &session,
                                const GameplayRulesetPolicy &policy) noexcept {
  return session.ruleset == policy.id &&
         session.rulesetDescriptor == policy.descriptor;
}

} // namespace gameplay
