#include "GameplayCandidateRules.h"
#include "CompiledGameplayJudge.h"
#include "GameplayJudgeRules.h"

#include <algorithm>
#include <iostream>
#include <vector>

int main() {
  int rank, rate, mode, count;
  while (std::cin >> rank >> rate >> mode >> count) {
    const auto selection = static_cast<gameplay::CandidateSelectionMode>(mode + 1);
    const auto rules = gameplay::compileGameplayJudgeRules(
        GameplayRuleset::LR2, 2, 100, rate, CourseJudgementConstraint::None,
        selection, 7, rank);
    const auto compiledJudge = gameplay::CompiledGameplayJudge::from(rules);
    const auto &windows = rules.contexts[0];
    long long past = 0, future = 0;
    for (const auto &window : windows.windows) {
      past = std::max(past, static_cast<long long>(window.lateMicros));
      future = std::max(future, -static_cast<long long>(window.earlyMicros));
    }
    std::vector<gameplay::JudgeCandidateDescriptor> candidates;
    for (int index = 0; index < count; ++index) {
      long long timing;
      int longHead, played;
      std::cin >> timing >> longHead >> played;
      if (timing < -past || timing >= future) continue;
      const long long diff = -timing;
      const auto role = longHead ? gameplay::NoteJudgeRole::LongNoteHead
                                 : gameplay::NoteJudgeRole::Normal;
      const auto accepted = compiledJudge.judgeAt(role, timing, 0);
      Judgement judgement = longHead && !played
          ? compiledJudge.judgeAt(gameplay::NoteJudgeRole::Normal, timing, 0).judgement
          : accepted.judgement;
      bool selectable = accepted.judgement != None;
      if (played) {
        const auto &poor = windows.windows[4];
        judgement = poor.earlyMicros <= diff && diff <= poor.lateMicros ? Kpoor : None;
        selectable = judgement != None;
      }
      candidates.push_back({static_cast<std::size_t>(index), timing, longHead != 0,
                            JudgeResult(judgement, diff), selectable,
                            played != 0});
    }
    std::vector<std::size_t> extra(candidates.size());
    const auto resolution = gameplay::resolveLr2Candidates(candidates, extra, windows, selection);
    int selected = -1, judge = -1;
    if (resolution.selectedSourceIndex) {
      selected = static_cast<int>(*resolution.selectedSourceIndex);
      const auto found = std::ranges::find(candidates, *resolution.selectedSourceIndex,
                                          &gameplay::JudgeCandidateDescriptor::sourceIndex);
      judge = found->judge.judgement == Kpoor ? 5 : static_cast<int>(found->judge.judgement);
    }
    std::cout << selected << ',' << judge;
    for (std::size_t index = 0; index < resolution.multiBadCount; ++index) {
      std::cout << ',' << extra[index];
    }
    std::cout << '\n';
  }
}
