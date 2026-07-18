#include "ScoreProvenance.h"
#include "scene/play/GameplayRuleset.h"
#include "scene/play/GameplayScoreState.h"
#include "scene/play/Judge.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <string_view>
#include <utility>

namespace {

using Window = std::pair<long long, long long>;

void testStableIdentity() {
  assert(gameplayRulesetFromId("lr2") == GameplayRuleset::LR2);
  assert(gameplayRulesetFromId("beatoraja") == GameplayRuleset::Beatoraja);
  assert(!gameplayRulesetFromId("LR2").has_value());
  assert(!gameplayRulesetFromId(" lr2").has_value());
  assert(!gameplayRulesetFromId("lr2 ").has_value());
  assert(!gameplayRulesetFromId("unknown").has_value());

  assert(gameplayRulesetId(GameplayRuleset::LR2) == "lr2");
  assert(gameplayRulesetId(GameplayRuleset::Beatoraja) == "beatoraja");
  assert(gameplayRulesetLabel(GameplayRuleset::LR2) == "LR2");
  assert(gameplayRulesetLabel(GameplayRuleset::Beatoraja) == "Beatoraja");
  assert(kDefaultGameplayRuleset == GameplayRuleset::LR2);
  assert(gameplayRulesetSelectionOrDefault("beatoraja") ==
         GameplayRuleset::Beatoraja);
  assert(gameplayRulesetSelectionOrDefault("invalid") == GameplayRuleset::LR2);
}

void testStableDescriptors() {
  const RulesetDescriptor lr2 = RulesetDescriptor::For(GameplayRuleset::LR2);
  assert((lr2 == RulesetDescriptor{
                    .id = "lr2",
                    .version = 3,
                    .scoringModel = "asobmashow-v1",
                    .judgementModel = "lr2-v1",
                    .gaugeModel = "lr2-gauge-v1",
                }));
  assert(lr2 == RulesetDescriptor::Current());
  assert(isSupportedRulesetDescriptor(lr2));

  const RulesetDescriptor beatoraja =
      RulesetDescriptor::For(GameplayRuleset::Beatoraja);
  assert((beatoraja == RulesetDescriptor{
                           .id = "beatoraja",
                           .version = 2,
                           .scoringModel = "asobmashow-v1",
                           .judgementModel = "bms-rank-v1",
                           .gaugeModel = "beatoraja-profile-gauge-v2",
                       }));
  assert(isSupportedRulesetDescriptor(beatoraja));

  const RulesetDescriptor legacy = RulesetDescriptor::Legacy();
  assert((legacy == RulesetDescriptor{
                       .id = "legacy-unknown",
                       .version = 0,
                       .scoringModel = "legacy-unknown",
                       .judgementModel = "legacy-unknown",
                       .gaugeModel = "legacy-unknown",
                   }));
  assert(!isSupportedRulesetDescriptor(legacy));
}

void testBeatorajaJudgeCharacterization() {
  const std::array<std::map<Judgement, Window>, 4> expected = {{
      {{PGreat, {-5000, 5000}},
       {Great, {-15000, 15000}},
       {Good, {-37500, 37500}},
       {Bad, {-385000, 490000}},
       {Kpoor, {-500000, 150000}}},
      {{PGreat, {-10000, 10000}},
       {Great, {-30000, 30000}},
       {Good, {-75000, 75000}},
       {Bad, {-330000, 420000}},
       {Kpoor, {-500000, 150000}}},
      {{PGreat, {-15000, 15000}},
       {Great, {-45000, 45000}},
       {Good, {-112500, 112500}},
       {Bad, {-275000, 350000}},
       {Kpoor, {-500000, 150000}}},
      {{PGreat, {-20000, 20000}},
       {Great, {-60000, 60000}},
       {Good, {-150000, 150000}},
       {Bad, {-220000, 280000}},
       {Kpoor, {-500000, 150000}}},
  }};

  for (int rank = 0; rank < static_cast<int>(expected.size()); ++rank) {
    assert(Judge(rank).timingWindows == expected[rank]);
  }
}

void testBeatorajaDefaultTotalCharacterization() {
  constexpr double epsilon = 1e-12;
  assert(std::abs(beatorajaDefaultGaugeTotal(5, 500) -
                  330.6521739130435) < epsilon);
  assert(std::abs(beatorajaDefaultGaugeTotal(7, 1000) -
                  460.90909090909093) < epsilon);
  assert(std::abs(beatorajaDefaultGaugeTotal(9, 1500) -
                  530.58139534883719) < epsilon);
  assert(std::abs(beatorajaDefaultGaugeTotal(24, 1000) - 507.0) < epsilon);
}

} // namespace

int main() {
  testStableIdentity();
  testStableDescriptors();
  testBeatorajaJudgeCharacterization();
  testBeatorajaDefaultTotalCharacterization();
  return 0;
}
