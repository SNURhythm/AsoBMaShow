#pragma once

#include "scene/play/GameplayRuleset.h"

// One-time best-score/lamp compatibility for the parity update. These explicit
// pairs must not slide forward with later ruleset versions. Playback support and
// recorded provenance remain independent from this selection-only exception.
[[nodiscard]] inline std::optional<RulesetDescriptor>
previousBestScoreCompatibleRuleset(const RulesetDescriptor &required) {
  for (const RulesetDescriptor previous : {
           RulesetDescriptor{.id = "lr2", .version = 4,
                             .scoringModel = "asobmashow-v1",
                             .judgementModel = "lr2-v1",
                             .gaugeModel = "lr2-gauge-v1"},
           RulesetDescriptor{.id = "beatoraja", .version = 3,
                             .scoringModel = "asobmashow-v1",
                             .judgementModel = "bms-rank-v1",
                             .gaugeModel = "beatoraja-profile-gauge-v2"}}) {
    auto next = previous;
    ++next.version;
    if (required == next) return previous;
  }
  return std::nullopt;
}
