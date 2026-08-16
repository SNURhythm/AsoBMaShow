#include "../../bms_parser.hpp"
#include "GameplayGaugeRules.h"

#include "GameplayScoreState.h"

#include <algorithm>
#include <cmath>

namespace {
double lr2DefaultTotal(int noteCount) noexcept {
  const int notes = std::max(0, noteCount);
  return std::floor(
      160.0 +
      (notes + std::clamp(notes - 400, 0, 200)) * 0.16);
}

double lr2TotalFactor(double total) noexcept {
  const double denominator =
      std::min(10.0, std::max(1.0, std::floor(total / 16.0) - 5.0));
  return 10.0 / denominator;
}

double lr2NoteFactor(int noteCount) noexcept {
  const int notes = std::max(1, noteCount);
  if (notes <= 20) {
    return 10.0;
  }
  if (notes < 30) {
    return 8.0 + 0.2 * (30 - notes);
  }
  if (notes < 60) {
    return 5.0 + 0.2 * (60 - notes) / 3.0;
  }
  if (notes < 125) {
    return 4.0 + static_cast<double>(125 - notes) / 65.0;
  }
  if (notes < 250) {
    return 3.0 + 0.008 * (250 - notes);
  }
  if (notes < 500) {
    return 2.0 + 0.004 * (500 - notes);
  }
  if (notes < 1000) {
    return 1.0 + 0.002 * (1000 - notes);
  }
  return 1.0;
}

double lr2DamageMultiplier(double total, int totalNotes) noexcept {
  return std::max(lr2TotalFactor(total), lr2NoteFactor(totalNotes));
}

CompiledGaugeDefinition lr2GrooveDefinition(
    float clearBorder, std::array<float, 6> deltas) {
  return {
      .initial = 20.0F,
      .minimum = 2.0F,
      .maximum = 100.0F,
      .clearBorder = clearBorder,
      .deathBelow = 0.0F,
      .baseDelta = deltas,
      .scalePositiveByTotal = true,
  };
}

CompiledGaugeDefinition lr2SurvivalDefinition(
    std::array<float, 6> deltas, bool scaleDamage) {
  return {
      .initial = 100.0F,
      .minimum = 0.0F,
      .maximum = 100.0F,
      .clearBorder = 0.0F,
      .deathBelow = 2.0F,
      .baseDelta = deltas,
      .scalePositiveByTotal = false,
      .scaleNegativeByLr2Damage = scaleDamage,
      .survival = true,
  };
}

void compileLr2Standard(GameplayGaugeRules &rules) {
  const std::array<float, 6> easy{
      1.2F, 1.2F, 0.6F, -3.2F, -4.8F, -1.6F};
  rules.gauges[gaugeTypeIndex(GaugeType::AssistedEasy)] =
      lr2GrooveDefinition(60.0F, easy);
  rules.gauges[gaugeTypeIndex(GaugeType::Easy)] =
      lr2GrooveDefinition(80.0F, easy);
  rules.gauges[gaugeTypeIndex(GaugeType::Normal)] = lr2GrooveDefinition(
      80.0F, {1.0F, 1.0F, 0.5F, -4.0F, -6.0F, -2.0F});
  rules.gauges[gaugeTypeIndex(GaugeType::Hard)] = lr2SurvivalDefinition(
      {0.1F, 0.1F, 0.05F, -6.0F, -10.0F, -2.0F}, true);
  rules.gauges[gaugeTypeIndex(GaugeType::ExHard)] = lr2SurvivalDefinition(
      {0.1F, 0.1F, 0.05F, -12.0F, -20.0F, -2.0F}, true);
  rules.gauges[gaugeTypeIndex(GaugeType::Hazard)] = lr2SurvivalDefinition(
      {0.15F, 0.06F, 0.0F, -100.0F, -100.0F, -10.0F}, false);
}

void compileLr2Course(GameplayGaugeRules &rules) {
  const std::array<std::array<float, 6>, 3> tables{
      std::array<float, 6>{0.10F, 0.10F, 0.05F, -2.0F, -3.0F, -2.0F},
      std::array<float, 6>{0.10F, 0.10F, 0.05F, -6.0F, -10.0F, -2.0F},
      std::array<float, 6>{0.10F, 0.10F, 0.05F, -12.0F, -20.0F, -2.0F},
  };
  for (int index = 0; index < static_cast<int>(kGaugeTypeCount); ++index) {
    const int classIndex = courseGaugeClassIndexForType(
        gaugeTypeAtIndex(index));
    rules.gauges[index] = lr2SurvivalDefinition(tables[classIndex], false);
  }
}

void compileBeatoraja(GameplayGaugeRules &rules) {
  for (int index = 0; index < static_cast<int>(kGaugeTypeCount); ++index) {
    const GaugeType type = gaugeTypeAtIndex(index);
    auto &definition = rules.gauges[index];
    definition.initial = gaugeInitialValue(type, rules.resolvedProfile);
    definition.minimum = gaugeMinimumValue(type, rules.resolvedProfile);
    definition.maximum = gaugeMaximumValue(type, rules.resolvedProfile);
    definition.clearBorder = gaugeBorderValue(type, rules.resolvedProfile);
    definition.deathBelow = 0.0F;
    definition.survival = gaugeIsSurvival(type, rules.resolvedProfile);
    for (const Judgement judgement :
         {PGreat, Great, Good, Bad, Poor, Kpoor}) {
      definition.baseDelta[gaugeJudgementIndex(judgement)] =
          gaugeBaseDeltaForJudgement(type, judgement, rules.resolvedProfile);
    }
  }
}
} // namespace

double resolveEffectiveGaugeTotal(
    GameplayRuleset ruleset, const bms_parser::ChartMeta &meta) noexcept {
  if (ruleset == GameplayRuleset::LR2) {
    if (meta.HasTotal && meta.Total > 0.0) {
      return std::floor(meta.Total);
    }
    return lr2DefaultTotal(meta.TotalNotes);
  }
  return meta.HasTotal
             ? meta.Total
             : beatorajaDefaultGaugeTotal(meta.KeyMode, meta.TotalNotes);
}

float gaugeReducedDamageZoneUpperBound(
    GameplayRuleset ruleset, GaugeType gaugeType,
    GaugeProfile profile) noexcept {
  if (ruleset == GameplayRuleset::LR2) {
    if (!gaugeProfileIsCourse(profile)) {
      return gaugeType == GaugeType::Hard ? 32.0F : 0.0F;
    }
    return courseGaugeClassIndexForType(gaugeType) <= 1 ? 32.0F : 0.0F;
  }

  if (!gaugeProfileIsCourse(profile)) {
    return gaugeType == GaugeType::Hard &&
                   profile != GaugeProfile::Standard5Keys
               ? 50.0F
               : 0.0F;
  }

  const int classIndex = courseGaugeClassIndexForType(gaugeType);
  if (profile == GaugeProfile::CourseLR2 && classIndex <= 1) {
    return 30.0F;
  }
  if (profile != GaugeProfile::Course5Keys &&
      profile != GaugeProfile::CourseLR2 && classIndex == 0) {
    return 25.0F;
  }
  return 0.0F;
}

GameplayGaugeRules compileGameplayGaugeRules(
    GameplayRuleset ruleset, const bms_parser::ChartMeta &meta,
    GaugeProfile requestedProfile) {
  GameplayGaugeRules result;
  result.ruleset = ruleset;
  if (ruleset == GameplayRuleset::LR2) {
    result.resolvedProfile = gaugeProfileIsCourse(requestedProfile)
                                 ? GaugeProfile::CourseLR2
                                 : GaugeProfile::Standard;
  } else {
    result.resolvedProfile =
        resolveGaugeProfile(requestedProfile, meta.KeyMode);
  }
  result.totalNotes = std::max(0, meta.TotalNotes);
  result.effectiveTotal = resolveEffectiveGaugeTotal(ruleset, meta);
  result.compiled = true;
  if (ruleset == GameplayRuleset::LR2) {
    if (gaugeProfileIsCourse(result.resolvedProfile)) {
      compileLr2Course(result);
    } else {
      compileLr2Standard(result);
    }
  } else {
    compileBeatoraja(result);
  }
  return result;
}

float GameplayGaugeRules::delta(GaugeType type, Judgement judgement,
                                float currentGauge,
                                float rate) const noexcept {
  if (ruleset == GameplayRuleset::Beatoraja) {
    return gaugeDeltaForJudgement(type, judgement, totalNotes, effectiveTotal,
                                  currentGauge, resolvedProfile) *
           rate;
  }
  const int judgementIndex = gaugeJudgementIndex(judgement);
  if (judgementIndex < 0) {
    return 0.0F;
  }
  const auto &definition = gauges[gaugeTypeIndex(type)];
  float result = definition.baseDelta[judgementIndex];
  if (result > 0.0F && definition.scalePositiveByTotal) {
    result *= static_cast<float>(effectiveTotal) /
              static_cast<float>(std::max(1, totalNotes));
  }
  if (result < 0.0F && definition.scaleNegativeByLr2Damage) {
    result *= static_cast<float>(
        lr2DamageMultiplier(effectiveTotal, totalNotes));
  }
  const float reducedDamageZone = gaugeReducedDamageZoneUpperBound(
      ruleset, type, resolvedProfile);
  if (result < 0.0F && reducedDamageZone > 0.0F &&
      currentGauge < reducedDamageZone) {
    result *= 0.6F;
  }
  return result * rate;
}
