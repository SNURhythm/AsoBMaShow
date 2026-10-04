#include "scene/play/CompiledGameplayJudge.h"
#include "scene/play/GameplayJudgeRules.h"
#include "scene/play/Judge.h"

#include <array>
#include <cassert>
#include <cstdint>

namespace {

using gameplay::CandidateSelectionMode;
using gameplay::CompiledGameplayJudge;
using gameplay::GameplayJudgeRules;
using gameplay::JudgeWindowContext;
using gameplay::NoteJudgeRole;
using gameplay::TimingWindow;

constexpr std::array<Judgement, 5> kJudgements = {
    PGreat, Great, Good, Bad, Kpoor};

bool contains(const TimingWindow &window, std::int64_t diff) {
  return window.earlyMicros <= diff && diff <= window.lateMicros;
}

void assertWindow(const CompiledGameplayJudge &judge,
                  JudgeWindowContext context, Judgement judgement,
                  std::int64_t early, std::int64_t late) {
  const auto window = judge.window(context, judgement);
  assert(window.has_value());
  assert(window->judgement == judgement);
  assert(window->earlyMicros == early);
  assert(window->lateMicros == late);
  assert(contains(*window, early));
  assert(contains(*window, late));
  assert(!contains(*window, early - 1));
  assert(!contains(*window, late + 1));
}

void assertRankWindows(int rank, std::int64_t pgreat, std::int64_t great,
                       std::int64_t good) {
  const GameplayJudgeRules rules =
      gameplay::compileGameplayJudgeRules(GameplayRuleset::LR2, rank);
  const CompiledGameplayJudge judge = CompiledGameplayJudge::from(rules);

  for (const JudgeWindowContext context :
       {JudgeWindowContext::Normal, JudgeWindowContext::Scratch}) {
    assertWindow(judge, context, PGreat, -pgreat, pgreat);
    assertWindow(judge, context, Great, -great, great);
    assertWindow(judge, context, Good, -good, good);
    assertWindow(judge, context, Bad, -200000, 200000);
    assertWindow(judge, context, Kpoor, -1000000, 0);
  }

  for (const JudgeWindowContext context :
       {JudgeWindowContext::LongNoteTail,
        JudgeWindowContext::LongScratchTail}) {
    assertWindow(judge, context, PGreat, -good, good);
    assertWindow(judge, context, Great, -good, good);
    assertWindow(judge, context, Good, -good, good);
    assertWindow(judge, context, Bad, -200000, 200000);
    assertWindow(judge, context, Kpoor, 0, 0);
  }

  assert(judge.judgeAt(NoteJudgeRole::Normal, 0, -pgreat).judgement ==
         PGreat);
  assert(judge.judgeAt(NoteJudgeRole::Normal, 0, pgreat).judgement ==
         PGreat);
  assert(judge.judgeAt(NoteJudgeRole::Normal, 0, -great).judgement == Great);
  assert(judge.judgeAt(NoteJudgeRole::Normal, 0, great).judgement == Great);
  assert(judge.judgeAt(NoteJudgeRole::Normal, 0, -good).judgement == Good);
  assert(judge.judgeAt(NoteJudgeRole::Normal, 0, good).judgement == Good);
  assert(judge.judgeAt(NoteJudgeRole::Normal, 0, -200000).judgement == Bad);
  assert(judge.judgeAt(NoteJudgeRole::Normal, 0, 200000).judgement == Bad);
  assert(judge.judgeAt(NoteJudgeRole::Normal, 0, -1000000).judgement ==
         Kpoor);
  assert(judge.judgeAt(NoteJudgeRole::Normal, 0, -1000001).judgement == None);
  assert(judge.judgeAt(NoteJudgeRole::Normal, 0, 200001).judgement == None);
}

void testLr2RankTablesAndSemantics() {
  assertRankWindows(0, 8000, 24000, 40000);
  assertRankWindows(1, 15000, 30000, 60000);
  assertRankWindows(2, 18000, 40000, 100000);
  assertRankWindows(3, 21000, 60000, 120000);
  assertRankWindows(4, 18000, 40000, 100000);

  for (const int invalidRank : {-1, 5, 999}) {
    const auto invalid =
        gameplay::compileGameplayJudgeRules(GameplayRuleset::LR2, invalidRank);
    const auto normal =
        gameplay::compileGameplayJudgeRules(GameplayRuleset::LR2, 2);
    assert(invalid.contexts == normal.contexts);
  }

  const auto rules =
      gameplay::compileGameplayJudgeRules(GameplayRuleset::LR2, 2);
  assert(rules.candidateSelection == CandidateSelectionMode::Combo);
  assert(rules.automaticPoorLateMicros == 200000);
  assert(rules.repeatedKpoor);
  assert(rules.multiBad);
  assert(rules.rejectsLateBadForLongNoteHead);

  const auto judge = CompiledGameplayJudge::from(rules);
  assert(judge.automaticPoorLateMicros() == 200000);
  assert(judge.judgeAt(NoteJudgeRole::LongNoteHead, 0, -150000).judgement ==
         Bad);
  assert(judge.judgeAt(NoteJudgeRole::LongNoteHead, 0, 150000).judgement ==
         None);
  assert(judge.judgeAt(NoteJudgeRole::Normal, 0, 150000).judgement == Bad);
}

void testRoleContextMapping() {
  assert(gameplay::windowContextForRole(NoteJudgeRole::Normal) ==
         JudgeWindowContext::Normal);
  assert(gameplay::windowContextForRole(NoteJudgeRole::LongNoteHead) ==
         JudgeWindowContext::Normal);
  assert(gameplay::windowContextForRole(NoteJudgeRole::Scratch) ==
         JudgeWindowContext::Scratch);
  assert(gameplay::windowContextForRole(NoteJudgeRole::LongScratchHead) ==
         JudgeWindowContext::Scratch);
  assert(gameplay::windowContextForRole(NoteJudgeRole::LongNoteTail) ==
         JudgeWindowContext::LongNoteTail);
  assert(gameplay::windowContextForRole(NoteJudgeRole::LongScratchTail) ==
         JudgeWindowContext::LongScratchTail);
}

void testLr2PracticeScalingAndCourseConstraints() {
  const auto scaled = gameplay::compileGameplayJudgeRules(
      GameplayRuleset::LR2, 2, 75, 80);
  const auto judge = CompiledGameplayJudge::from(scaled);
  assertWindow(judge, JudgeWindowContext::Normal, PGreat, -10800, 10800);
  assertWindow(judge, JudgeWindowContext::Normal, Great, -24000, 24000);
  assertWindow(judge, JudgeWindowContext::Normal, Good, -60000, 60000);
  assertWindow(judge, JudgeWindowContext::Normal, Bad, -200000, 200000);
  assertWindow(judge, JudgeWindowContext::Normal, Kpoor, -1000000, 0);
  assert(judge.automaticPoorLateMicros() == 200000);

  const auto noGood = CompiledGameplayJudge::from(
      gameplay::compileGameplayJudgeRules(
          GameplayRuleset::LR2, 2, 100, 100,
          CourseJudgementConstraint::NoGood));
  assert(noGood.window(JudgeWindowContext::Normal, Good)->earlyMicros ==
         noGood.window(JudgeWindowContext::Normal, Great)->earlyMicros);
  assert(noGood.window(JudgeWindowContext::Normal, Good)->lateMicros ==
         noGood.window(JudgeWindowContext::Normal, Great)->lateMicros);

  const auto noGreat = CompiledGameplayJudge::from(
      gameplay::compileGameplayJudgeRules(
          GameplayRuleset::LR2, 2, 100, 100,
          CourseJudgementConstraint::NoGreat));
  assert(noGreat.window(JudgeWindowContext::Normal, Good)->earlyMicros ==
         noGreat.window(JudgeWindowContext::Normal, PGreat)->earlyMicros);
  assert(noGreat.window(JudgeWindowContext::Normal, Great)->lateMicros ==
         noGreat.window(JudgeWindowContext::Normal, PGreat)->lateMicros);
}

void testBeatorajaReferenceWindowsAndBoundaries() {
  assert(Judge(0).timingWindows.at(Bad).second == 70000);
  assert(Judge(4).timingWindows.at(PGreat).second == 25000);
  // Reference JudgeProperty.NORMAL scales BAD by rank, but fixes KPOOR.
  const auto judge = CompiledGameplayJudge::from(
      gameplay::compileGameplayJudgeRules(GameplayRuleset::Beatoraja, 0));
  assertWindow(judge, JudgeWindowContext::Normal, Bad, -55000, 70000);
  assertWindow(judge, JudgeWindowContext::Normal, Kpoor, -500000, 150000);
  assertWindow(judge, JudgeWindowContext::Scratch, PGreat, -7500, 7500);
  assertWindow(judge, JudgeWindowContext::Scratch, Bad, -57500, 72500);
  assertWindow(judge, JudgeWindowContext::LongNoteTail, PGreat, -30000, 30000);
  assertWindow(judge, JudgeWindowContext::LongScratchTail, Good, -52500, 52500);
  assert(judge.judgeAt(NoteJudgeRole::Normal, 0, 70000).judgement == Bad);
  assert(judge.judgeAt(NoteJudgeRole::Normal, 0, 70001).judgement == Kpoor);
  assert(judge.judgeAt(NoteJudgeRole::Scratch, 0, 7000).judgement == PGreat);
  assert(judge.judgeAt(NoteJudgeRole::Normal, 0, 7000).judgement == Great);
  assert(judge.judgeAt(NoteJudgeRole::LongNoteHead, 0, 50000).judgement == Bad);
  assert(judge.judgeAt(NoteJudgeRole::LongNoteTail, 0, -500001).judgement == None);
  const auto veryEasy = CompiledGameplayJudge::from(
      gameplay::compileGameplayJudgeRules(GameplayRuleset::Beatoraja, 4));
  assertWindow(veryEasy, JudgeWindowContext::Normal, PGreat, -25000, 25000);
  assertWindow(veryEasy, JudgeWindowContext::Normal, Bad, -275000, 350000);
  for (int invalid : {-1, 5, 999}) {
    const auto fallback = CompiledGameplayJudge::from(
        gameplay::compileGameplayJudgeRules(GameplayRuleset::Beatoraja, invalid));
    assertWindow(fallback, JudgeWindowContext::Normal, PGreat, -15000, 15000);
  }
  const auto scaled = CompiledGameplayJudge::from(
      gameplay::compileGameplayJudgeRules(GameplayRuleset::Beatoraja, 0, 75, 45));
  assertWindow(scaled, JudgeWindowContext::Normal, PGreat, -1687, 1687);
  assertWindow(scaled, JudgeWindowContext::Normal, Bad, -55000, 70000);
  assertWindow(scaled, JudgeWindowContext::Normal, Kpoor, -500000, 150000);
}

void testProfileSpecificWindowsAndReleaseDeadlines() {
  const auto build = [](int keys, int rank) {
    return CompiledGameplayJudge::from(gameplay::compileGameplayJudgeRules(
        GameplayRuleset::Beatoraja, rank, 100, 100,
        CourseJudgementConstraint::None, CandidateSelectionMode::Lowest, keys));
  };
  for (int keys : {5, 10}) {
    const auto judge = build(keys, 1);
    assert(judge.judgeAt(NoteJudgeRole::Normal, 0, 26000).judgement == Good);
    assert(judge.judgeAt(NoteJudgeRole::Scratch, 0, 26000).judgement == Great);
    assertWindow(judge, JudgeWindowContext::LongScratchTail, Good, -80000, 80000);
    assert(!judge.rules().comboKpoor);
  }
  const auto pms = build(9, 0);
  assertWindow(pms, JudgeWindowContext::Normal, PGreat, -20000, 20000);
  assertWindow(pms, JudgeWindowContext::Normal, Great, -20000, 20000);
  assertWindow(pms, JudgeWindowContext::Normal, Good, -38610, 38610);
  assertWindow(pms, JudgeWindowContext::Normal, Bad, -183000, 183000);
  assert(pms.rules().singleMiss && !pms.rules().vanishBad);
  assert(pms.rules().normalReleaseMarginMicros == 200000);
  const auto keyboard = build(24, 3);
  assert(keyboard.judgeAt(NoteJudgeRole::LongNoteTail, 0, -25000).judgement == PGreat);
  assert(keyboard.judgeAt(NoteJudgeRole::LongNoteTail, 0, -25001).judgement == Great);
  assert(keyboard.judgeAt(NoteJudgeRole::LongNoteTail, 0, 160000).judgement == PGreat);
  const auto seven = build(7, 1);
  assert(seven.automaticPoorLateMicros(NoteJudgeRole::Normal) == 140000);
  assert(seven.automaticPoorLateMicros(NoteJudgeRole::Scratch) == 145000);
}

void testExtendedRankInterpolation() {
  const auto build = [](GameplayRuleset ruleset, int percent) {
    return CompiledGameplayJudge::from(gameplay::compileGameplayJudgeRules(
        ruleset, 2, 100, 100, CourseJudgementConstraint::None,
        CandidateSelectionMode::Lowest, 7, percent));
  };
  const auto lr2 = build(GameplayRuleset::LR2, 60);
  assertWindow(lr2, JudgeWindowContext::Normal, PGreat, -16200, 16200);
  assertWindow(lr2, JudgeWindowContext::Normal, Great, -34000, 34000);
  assertWindow(lr2, JudgeWindowContext::LongNoteTail, PGreat, -76000, 76000);
  const auto generous = build(GameplayRuleset::LR2, 200);
  assertWindow(generous, JudgeWindowContext::LongNoteTail, PGreat, -200000, 200000);
  const auto beatoraja = build(GameplayRuleset::Beatoraja, 60);
  assertWindow(beatoraja, JudgeWindowContext::Normal, PGreat, -12000, 12000);
  assertWindow(beatoraja, JudgeWindowContext::Normal, Bad, -132000, 168000);
  assert(beatoraja.rules().effectiveJudgeRankPercent == 60);
}

} // namespace

int main() {
  testExtendedRankInterpolation();
  testProfileSpecificWindowsAndReleaseDeadlines();
  testLr2RankTablesAndSemantics();
  testRoleContextMapping();
  testLr2PracticeScalingAndCourseConstraints();
  testBeatorajaReferenceWindowsAndBoundaries();
  return 0;
}
