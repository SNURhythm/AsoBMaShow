#pragma once

#include "CompiledGameplayJudge.h"
#include "GameplayGaugeRules.h"
#include "GameplayRuleset.h"
#include "../../ScoreProvenance.h"

#include <optional>
#include <string>

struct CoursePlaySession;

namespace gameplay {

struct GameplayRulesetPolicy {
  GameplayRuleset id = kDefaultGameplayRuleset;
  RulesetDescriptor descriptor =
      RulesetDescriptor::For(kDefaultGameplayRuleset);
  CompiledGameplayJudge judge;
  GameplayGaugeRules gauge;
};

enum class GameplayPolicyBuildStatus {
  Built,
  UnsupportedRuleset,
  InvalidChart,
  InvalidReplaySnapshot,
};

struct GameplayRulesetPolicyBuildInput {
  GameplayRuleset ruleset = kDefaultGameplayRuleset;
  GaugeProfile gaugeProfile = GaugeProfile::Standard;
  int sourceRank = 2;
  int playbackRatePercent = 100;
  int judgeScalePercent = 100;
  CourseJudgementConstraint courseJudgement =
      CourseJudgementConstraint::None;
  CandidateSelectionMode beatorajaCandidateSelection =
      CandidateSelectionMode::Lowest;
  std::optional<RulesetDescriptor> requiredDescriptor;
  std::optional<ScoreStageProvenance> replaySnapshot;
};

struct GameplayPolicyBuildOutcome {
  GameplayPolicyBuildStatus status = GameplayPolicyBuildStatus::InvalidChart;
  std::optional<GameplayRulesetPolicy> policy;
  std::string diagnostic;

  [[nodiscard]] bool built() const noexcept {
    return status == GameplayPolicyBuildStatus::Built && policy.has_value();
  }
};

[[nodiscard]] GameplayPolicyBuildOutcome buildGameplayRulesetPolicy(
    const bms_parser::ChartMeta &meta,
    const GameplayRulesetPolicyBuildInput &input);

[[nodiscard]] bool courseSessionAcceptsPolicy(
    const CoursePlaySession &session,
    const GameplayRulesetPolicy &policy) noexcept;

} // namespace gameplay
