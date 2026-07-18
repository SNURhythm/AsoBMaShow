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
    assertWindow(judge, context, PGreat, -120000, 120000);
    assertWindow(judge, context, Great, -120000, 120000);
    assertWindow(judge, context, Good, -120000, 120000);
    assertWindow(judge, context, Bad, -200000, 200000);
    assertWindow(judge, context, Kpoor, -1000000, 0);
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
  assert(rules.candidateSelection == CandidateSelectionMode::LR2);
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

void testBeatorajaCharacterizationAcrossContexts() {
  for (int rank = 0; rank < 4; ++rank) {
    const Judge legacy(rank);
    const auto rules = gameplay::compileGameplayJudgeRules(
        GameplayRuleset::Beatoraja, rank, 100, 100,
        CourseJudgementConstraint::None, CandidateSelectionMode::Score);
    assert(rules.ruleset == GameplayRuleset::Beatoraja);
    assert(rules.candidateSelection == CandidateSelectionMode::Score);
    assert(!rules.repeatedKpoor);
    assert(!rules.multiBad);
    assert(!rules.rejectsLateBadForLongNoteHead);
    assert(rules.automaticPoorLateMicros ==
           legacy.timingWindows.at(Bad).second);

    const auto compiled = CompiledGameplayJudge::from(rules);
    for (const auto context : {JudgeWindowContext::Normal,
                               JudgeWindowContext::Scratch,
                               JudgeWindowContext::LongNoteTail,
                               JudgeWindowContext::LongScratchTail}) {
      for (const Judgement judgement : kJudgements) {
        const auto expected = legacy.timingWindows.at(judgement);
        assertWindow(compiled, context, judgement, expected.first,
                     expected.second);
      }
    }
  }

  Judge scaledLegacy(0);
  scaledLegacy.applyWindowScale(75, 45);
  const auto scaled = CompiledGameplayJudge::from(
      gameplay::compileGameplayJudgeRules(GameplayRuleset::Beatoraja, 0, 75,
                                          45));
  for (const Judgement judgement : kJudgements) {
    const auto expected = scaledLegacy.timingWindows.at(judgement);
    assertWindow(scaled, JudgeWindowContext::Normal, judgement,
                 expected.first, expected.second);
  }
}

} // namespace

int main() {
  testLr2RankTablesAndSemantics();
  testRoleContextMapping();
  testLr2PracticeScalingAndCourseConstraints();
  testBeatorajaCharacterizationAcrossContexts();
  return 0;
}
