#include "scene/play/GameplayGaugeRules.h"
#include "scene/play/GameplayScoreState.h"

#include "bms_parser.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

bool close(double left, double right, double tolerance = 0.0001) {
  return std::abs(left - right) <= tolerance;
}

bms_parser::ChartMeta meta(int totalNotes, double total,
                           bool hasTotal = true, int keyMode = 7) {
  bms_parser::ChartMeta result;
  result.TotalNotes = totalNotes;
  result.Total = total;
  result.HasTotal = hasTotal;
  result.KeyMode = keyMode;
  return result;
}

double expectedNoteFactor(int noteCount) {
  const int n = std::max(1, noteCount);
  if (n <= 20) {
    return 10.0;
  }
  if (n < 30) {
    return 8.0 + 0.2 * (30 - n);
  }
  if (n < 60) {
    return 5.0 + 0.2 * (60 - n) / 3.0;
  }
  if (n < 125) {
    return 4.0 + static_cast<double>(125 - n) / 65.0;
  }
  if (n < 250) {
    return 3.0 + 0.008 * (250 - n);
  }
  if (n < 500) {
    return 2.0 + 0.004 * (500 - n);
  }
  if (n < 1000) {
    return 1.0 + 0.002 * (1000 - n);
  }
  return 1.0;
}

double expectedTotalFactor(double total) {
  const double denominator =
      std::min(10.0, std::max(1.0, std::floor(total / 16.0) - 5.0));
  return 10.0 / denominator;
}

void testLr2EffectiveTotalRules() {
  require(resolveEffectiveGaugeTotal(GameplayRuleset::LR2,
                                     meta(1000, 200.0)) == 200.0 &&
              resolveEffectiveGaugeTotal(GameplayRuleset::LR2,
                                         meta(1000, 200.5)) == 200.0,
          "LR2 floors positive authored TOTAL");
  require(resolveEffectiveGaugeTotal(GameplayRuleset::LR2,
                                     meta(399, 0.0)) == 223.0 &&
              resolveEffectiveGaugeTotal(GameplayRuleset::LR2,
                                         meta(400, 0.0)) == 224.0 &&
              resolveEffectiveGaugeTotal(GameplayRuleset::LR2,
                                         meta(599, -1.0)) == 287.0 &&
              resolveEffectiveGaugeTotal(GameplayRuleset::LR2,
                                         meta(600, 0.0, false)) == 288.0,
          "LR2 default TOTAL is exact at 400/600-note boundaries");

  const auto beatoraja = meta(432, 200.5);
  require(resolveEffectiveGaugeTotal(GameplayRuleset::Beatoraja,
                                     beatoraja) == 200.5,
          "Beatoraja preserves fractional authored TOTAL");
  const auto absent = meta(432, 0.0, false, 24);
  require(close(resolveEffectiveGaugeTotal(GameplayRuleset::Beatoraja,
                                           absent),
                beatorajaDefaultGaugeTotal(24, 432)),
          "Beatoraja keeps its existing default TOTAL formula");
}

void testLr2StandardGaugeDefinitionsAndDeltas() {
  const auto rules = compileGameplayGaugeRules(
      GameplayRuleset::LR2, meta(200, 200.0), GaugeProfile::Standard);
  struct DefinitionCase {
    GaugeType type;
    float initial;
    float minimum;
    float border;
    bool survival;
  };
  for (const auto &entry : {
           DefinitionCase{GaugeType::AssistedEasy, 20, 2, 60, false},
           DefinitionCase{GaugeType::Easy, 20, 2, 80, false},
           DefinitionCase{GaugeType::Normal, 20, 2, 80, false},
           DefinitionCase{GaugeType::Hard, 100, 0, 0, true},
           DefinitionCase{GaugeType::ExHard, 100, 0, 0, true},
           DefinitionCase{GaugeType::Hazard, 100, 0, 0, true},
       }) {
    const auto &definition = rules.gauges[gaugeTypeIndex(entry.type)];
    require(definition.initial == entry.initial &&
                definition.minimum == entry.minimum &&
                definition.maximum == 100.0F &&
                definition.clearBorder == entry.border &&
                definition.survival == entry.survival &&
                (!entry.survival || definition.deathBelow == 2.0F),
            "LR2 gauge initial/min/max/border/death values are exact");
  }

  const double damage = std::max(expectedTotalFactor(200.0),
                                 expectedNoteFactor(200));
  const std::array judgements{PGreat, Great, Good, Bad, Poor, Kpoor};
  const std::array<std::array<float, 6>, kGaugeTypeCount> bases{
      std::array<float, 6>{1.2F, 1.2F, .6F, -3.2F, -4.8F, -1.6F},
      std::array<float, 6>{1.2F, 1.2F, .6F, -3.2F, -4.8F, -1.6F},
      std::array<float, 6>{1.F, 1.F, .5F, -4.F, -6.F, -2.F},
      std::array<float, 6>{.1F, .1F, .05F, -6.F, -10.F, -2.F},
      std::array<float, 6>{.1F, .1F, .05F, -12.F, -20.F, -2.F},
      std::array<float, 6>{.15F, .06F, 0.F, -100.F, -100.F, -10.F},
  };
  for (int typeIndex = 0; typeIndex < static_cast<int>(kGaugeTypeCount);
       ++typeIndex) {
    const GaugeType type = gaugeTypeAtIndex(typeIndex);
    for (int judgeIndex = 0; judgeIndex < 6; ++judgeIndex) {
      double expected = bases[typeIndex][judgeIndex];
      if ((type == GaugeType::Hard || type == GaugeType::ExHard) &&
          expected < 0.0) {
        expected *= damage;
      }
      require(close(rules.delta(type, judgements[judgeIndex], 100.0F),
                    expected),
              "LR2 standard delta table and scaling are exact");
    }
  }
}

void testLr2ProfileIdentityDoesNotLeakBeatorajaModeProfiles() {
  for (const int keyMode : {4, 5, 6, 7, 8, 9, 10, 14, 24, 48}) {
    const auto standard = compileGameplayGaugeRules(
        GameplayRuleset::LR2, meta(200, 200.0, true, keyMode),
        GaugeProfile::Standard);
    require(standard.resolvedProfile == GaugeProfile::Standard,
            "LR2 ordinary play uses one profile identity across key modes");

    const auto course = compileGameplayGaugeRules(
        GameplayRuleset::LR2, meta(200, 200.0, true, keyMode),
        GaugeProfile::CourseDefault);
    require(course.resolvedProfile == GaugeProfile::CourseLR2,
            "LR2 course play uses the LR2 course profile across key modes");
  }
}

void testLr2DamageFactorBoundariesAndHardGuts() {
  for (const int notes : {20, 21, 29, 30, 59, 60, 124, 125,
                          249, 250, 499, 500, 999, 1000}) {
    const auto rules = compileGameplayGaugeRules(
        GameplayRuleset::LR2, meta(notes, 240.0), GaugeProfile::Standard);
    const double expectedMultiplier =
        std::max(expectedTotalFactor(240.0), expectedNoteFactor(notes));
    require(close(rules.delta(GaugeType::Hard, Bad, 100.0F),
                  -6.0 * expectedMultiplier),
            "LR2 hard damage follows every note-factor boundary");
  }

  for (const double total : {95.0, 96.0, 111.0, 112.0, 239.0, 240.0}) {
    const auto rules = compileGameplayGaugeRules(
        GameplayRuleset::LR2, meta(1000, total), GaugeProfile::Standard);
    require(close(rules.delta(GaugeType::Hard, Bad, 100.0F),
                  -6.0 * std::max(expectedTotalFactor(std::floor(total)),
                                  expectedNoteFactor(1000))),
            "LR2 hard damage follows TOTAL-factor floor boundaries");
  }

  const auto rules = compileGameplayGaugeRules(
      GameplayRuleset::LR2, meta(1000, 240.0), GaugeProfile::Standard);
  require(close(rules.delta(GaugeType::Hard, Bad, 31.999F), -3.6) &&
              close(rules.delta(GaugeType::Hard, Bad, 32.0F), -6.0) &&
              close(rules.delta(GaugeType::Hard, Bad, 32.001F), -6.0),
          "LR2 hard guts is strictly below 32 percent");
}

void testReducedDamagePresentationUsesTheCompiledRuleset() {
  require(gaugeReducedDamageZoneUpperBound(
              GameplayRuleset::LR2, GaugeType::Hard,
              GaugeProfile::Standard) == 32.0F,
          "LR2 hard gauge presents its reduced-damage boundary at 32 percent");
  require(gaugeReducedDamageZoneUpperBound(
              GameplayRuleset::Beatoraja, GaugeType::Hard,
              GaugeProfile::Standard) == 50.0F,
          "Beatoraja hard gauge keeps its staged reduced-damage boundary at "
          "50 percent");
  require(gaugeReducedDamageZoneUpperBound(
              GameplayRuleset::LR2, GaugeType::Normal,
              GaugeProfile::CourseLR2) == 32.0F &&
              gaugeReducedDamageZoneUpperBound(
                  GameplayRuleset::LR2, GaugeType::Hard,
                  GaugeProfile::CourseLR2) == 32.0F &&
              gaugeReducedDamageZoneUpperBound(
                  GameplayRuleset::LR2, GaugeType::ExHard,
                  GaugeProfile::CourseLR2) == 0.0F,
          "LR2 Class and Ex-Class present the same 32-percent boundary as "
          "their damage rule");
}

void testLr2CourseTables() {
  const auto rules = compileGameplayGaugeRules(
      GameplayRuleset::LR2, meta(1000, 240.0),
      GaugeProfile::CourseDefault);
  require(rules.resolvedProfile == GaugeProfile::CourseLR2,
          "LR2 course requests resolve to the LR2 course profile");
  const std::array classes{GaugeType::Normal, GaugeType::Hard,
                           GaugeType::ExHard};
  const std::array<std::array<float, 6>, 3> tables{
      std::array<float, 6>{.10F, .10F, .05F, -2.F, -3.F, -2.F},
      std::array<float, 6>{.10F, .10F, .05F, -6.F, -10.F, -2.F},
      std::array<float, 6>{.10F, .10F, .05F, -12.F, -20.F, -2.F},
  };
  const std::array judgements{PGreat, Great, Good, Bad, Poor, Kpoor};
  for (int classIndex = 0; classIndex < 3; ++classIndex) {
    const auto &definition =
        rules.gauges[gaugeTypeIndex(classes[classIndex])];
    require(definition.initial == 100.0F && definition.minimum == 0.0F &&
                definition.maximum == 100.0F && definition.survival &&
                definition.deathBelow == 2.0F,
            "all LR2 course classes start at 100 and die below 2");
    for (int judgeIndex = 0; judgeIndex < 6; ++judgeIndex) {
      require(close(rules.delta(classes[classIndex], judgements[judgeIndex],
                                100.0F),
                    tables[classIndex][judgeIndex]),
              "LR2 course class delta table is exact");
    }
  }
  require(close(rules.delta(GaugeType::Normal, Bad, 31.999F), -1.2) &&
              close(rules.delta(GaugeType::Hard, Bad, 31.999F), -3.6) &&
              close(rules.delta(GaugeType::ExHard, Bad, 31.999F), -12.0),
          "LR2 Class and Ex-Class have guts while Ex-Hard Class does not");
}

void testLr2StateDeathAndAutoShift() {
  const auto rules = compileGameplayGaugeRules(
      GameplayRuleset::LR2, meta(1000, 240.0), GaugeProfile::Standard);
  GameplayScoreState dead({.gaugeRules = rules, .keyMode = 7});
  dead.configureGauge(GaugeType::Hard, GaugeAutoShiftMode::None,
                      GaugeProfile::Standard);
  dead.setStartingGaugePercent(2);
  dead.applyGaugeDelta(-0.001F);
  require(dead.currentGauge == 0.0F && dead.activeGaugeFailed(),
          "LR2 survival gauge dies when post-delta value is 1.999 or lower");

  GameplayScoreState alive({.gaugeRules = rules, .keyMode = 7});
  alive.configureGauge(GaugeType::Hard, GaugeAutoShiftMode::None,
                       GaugeProfile::Standard);
  alive.setStartingGaugePercent(2);
  require(alive.currentGauge == 2.0F && !alive.activeGaugeFailed(),
          "LR2 survival gauge remains alive at exactly 2 percent");

  GameplayScoreState shifted({.gaugeRules = rules, .keyMode = 7});
  shifted.configureGauge(GaugeType::Hard,
                         GaugeAutoShiftMode::SurvivalToGroove,
                         GaugeProfile::Standard);
  shifted.setStartingGaugePercent(2);
  shifted.applyGaugeDelta(-0.001F);
  require(shifted.gaugeType == GaugeType::Normal &&
              !shifted.activeGaugeFailed(),
          "LR2 survival-to-groove auto shift follows compiled death state");
}

void testBeatorajaCompiledRulesMatchLegacyHelpers() {
  const std::array profiles{
      GaugeProfile::Standard5Keys,  GaugeProfile::Standard,
      GaugeProfile::Standard9Keys, GaugeProfile::Standard24Keys,
      GaugeProfile::Course5Keys,   GaugeProfile::Course7Keys,
      GaugeProfile::Course9Keys,   GaugeProfile::Course24Keys,
      GaugeProfile::CourseLR2,
  };
  const std::array judgements{PGreat, Great, Good, Bad, Poor, Kpoor};
  for (const GaugeProfile profile : profiles) {
    const int keyMode = profile == GaugeProfile::Standard5Keys ||
                                profile == GaugeProfile::Course5Keys
                            ? 5
                        : profile == GaugeProfile::Standard9Keys ||
                                  profile == GaugeProfile::Course9Keys
                            ? 9
                        : profile == GaugeProfile::Standard24Keys ||
                                  profile == GaugeProfile::Course24Keys
                            ? 24
                            : 7;
    const auto rules = compileGameplayGaugeRules(
        GameplayRuleset::Beatoraja, meta(432, 280.5, true, keyMode), profile);
    for (int typeIndex = 0; typeIndex < static_cast<int>(kGaugeTypeCount);
         ++typeIndex) {
      const GaugeType type = gaugeTypeAtIndex(typeIndex);
      const auto &definition = rules.gauges[typeIndex];
      require(definition.initial == gaugeInitialValue(type, profile) &&
                  definition.minimum == gaugeMinimumValue(type, profile) &&
                  definition.maximum == gaugeMaximumValue(type, profile) &&
                  definition.clearBorder == gaugeBorderValue(type, profile) &&
                  definition.survival == gaugeIsSurvival(type, profile),
              "compiled Beatoraja gauge shape matches legacy helpers");
      for (const Judgement judgement : judgements) {
        require(close(rules.delta(type, judgement, 31.0F),
                      gaugeDeltaForJudgement(type, judgement, 432, 280.5,
                                             31.0F, profile)),
                "compiled Beatoraja deltas match every legacy profile");
      }
    }
  }
}
} // namespace

int main() {
  testLr2EffectiveTotalRules();
  testLr2StandardGaugeDefinitionsAndDeltas();
  testLr2ProfileIdentityDoesNotLeakBeatorajaModeProfiles();
  testLr2DamageFactorBoundariesAndHardGuts();
  testReducedDamagePresentationUsesTheCompiledRuleset();
  testLr2CourseTables();
  testLr2StateDeathAndAutoShift();
  testBeatorajaCompiledRulesMatchLegacyHelpers();
  return 0;
}
