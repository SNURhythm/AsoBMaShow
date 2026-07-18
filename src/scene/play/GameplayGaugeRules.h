#pragma once

#include "GameplayGaugeTypes.h"
#include "GameplayRuleset.h"
#include "Judgement.h"

#include <array>

namespace bms_parser {
struct ChartMeta;
}

struct CompiledGaugeDefinition {
  float initial = 20.0F;
  float minimum = 2.0F;
  float maximum = 100.0F;
  float clearBorder = 80.0F;
  float deathBelow = 0.0F;
  std::array<float, 6> baseDelta{};
  bool scalePositiveByTotal = false;
  bool scaleNegativeByLr2Damage = false;
  bool hardGutsBelow32 = false;
  bool survival = false;
};

struct GameplayGaugeRules {
  GameplayRuleset ruleset = GameplayRuleset::Beatoraja;
  GaugeProfile resolvedProfile = GaugeProfile::Standard;
  int totalNotes = 0;
  double effectiveTotal = 100.0;
  std::array<CompiledGaugeDefinition, kGaugeTypeCount> gauges{};
  bool compiled = false;

  [[nodiscard]] float delta(GaugeType type, Judgement judgement,
                            float currentGauge,
                            float rate = 1.0F) const noexcept;
};

[[nodiscard]] double resolveEffectiveGaugeTotal(
    GameplayRuleset ruleset, const bms_parser::ChartMeta &meta) noexcept;

[[nodiscard]] GameplayGaugeRules compileGameplayGaugeRules(
    GameplayRuleset ruleset, const bms_parser::ChartMeta &meta,
    GaugeProfile requestedProfile);
