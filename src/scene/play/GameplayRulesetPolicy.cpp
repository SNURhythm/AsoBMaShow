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

std::optional<JudgeWindowSet>
validatedReplayWindows(const ScoreStageProvenance &snapshot) {
  if (snapshot.effectiveJudgeWindows.size() != kRequiredJudgements.size()) {
    return std::nullopt;
  }

  JudgeWindowSet result;
  std::array<bool, kRequiredJudgements.size()> found{};
  for (const auto &recorded : snapshot.effectiveJudgeWindows) {
    const auto expected = std::ranges::find(kRequiredJudgements,
                                            recorded.judgement);
    if (expected == kRequiredJudgements.end()) {
      return std::nullopt;
    }
    const std::size_t index = static_cast<std::size_t>(
        std::distance(kRequiredJudgements.begin(), expected));
    if (found[index] || recorded.earlyMicros > 0 ||
        recorded.lateMicros < 0 ||
        recorded.earlyMicros > recorded.lateMicros ||
        recorded.earlyMicros < -kMaximumReplayWindowMagnitude ||
        recorded.lateMicros > kMaximumReplayWindowMagnitude) {
      return std::nullopt;
    }
    found[index] = true;
    result.windows[index] = {recorded.judgement, recorded.earlyMicros,
                             recorded.lateMicros};
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
  rules.contexts.fill(*windows);
  const auto bad = std::ranges::find_if(
      windows->windows,
      [](const TimingWindow &window) { return window.judgement == Bad; });
  if (bad != windows->windows.end() && input.ruleset == GameplayRuleset::Beatoraja) {
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

  std::string replayDiagnostic;
  const auto judgeRules = compileJudge(meta, input, replayDiagnostic);
  if (!judgeRules.has_value()) {
    return failure(GameplayPolicyBuildStatus::InvalidReplaySnapshot,
                   std::move(replayDiagnostic));
  }
  GameplayGaugeRules gauge = compileGameplayGaugeRules(
      input.ruleset, meta, input.gaugeProfile);
  if (!gauge.compiled || gauge.ruleset != input.ruleset ||
      !std::isfinite(gauge.effectiveTotal) ||
      gauge.effectiveTotal <= 0.0) {
    return failure(GameplayPolicyBuildStatus::InvalidChart,
                   "The chart gauge policy could not be compiled.");
  }

  GameplayRulesetPolicy policy{
      .id = input.ruleset,
      .descriptor = expected,
      .judge = CompiledGameplayJudge::from(*judgeRules),
      .gauge = std::move(gauge),
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
