#include "GameplayCandidateRules.h"

#include <algorithm>
#include <cstdlib>

namespace gameplay {
namespace {

const JudgeCandidateDescriptor *findCandidate(
    std::span<const JudgeCandidateDescriptor> candidates,
    std::size_t sourceIndex) noexcept {
  const auto found = std::ranges::find(
      candidates, sourceIndex, &JudgeCandidateDescriptor::sourceIndex);
  return found == candidates.end() ? nullptr : &*found;
}

bool prefersCandidate(const JudgeCandidateDescriptor &current,
                      const JudgeCandidateDescriptor &next,
                      const JudgeWindowSet &windows,
                      CandidateSelectionMode selection) noexcept {
  if (next.played) {
    return false;
  }
  switch (selection) {
  case CandidateSelectionMode::Duration:
    return std::llabs(current.judge.Diff) > std::llabs(next.judge.Diff);
  case CandidateSelectionMode::LR2:
  case CandidateSelectionMode::Combo:
    return current.judge.Diff > windows.windows[2].lateMicros &&
           next.judge.Diff >= windows.windows[2].earlyMicros;
  case CandidateSelectionMode::Score:
    return current.judge.Diff > windows.windows[1].lateMicros &&
           next.judge.Diff >= windows.windows[1].earlyMicros;
  case CandidateSelectionMode::Lowest:
    return false;
  }
  return false;
}

} // namespace

Lr2CandidateResolution resolveLr2Candidates(
    std::span<const JudgeCandidateDescriptor> candidates,
    std::span<std::size_t> multiBadSourceIndices,
    const JudgeWindowSet &windows, CandidateSelectionMode selection) noexcept {
  Lr2CandidateResolution result;
  const JudgeCandidateDescriptor *selected = nullptr;
  for (const auto &candidate : candidates) {
    if (selected != nullptr && !selected->played &&
        !prefersCandidate(*selected, candidate, windows, selection)) {
      continue;
    }
    // JudgeManager clears the previous candidate when a replacement is
    // rejected, including a long-note head in the late BAD region.
    if (!candidate.selectable || candidate.judge.judgement == None) {
      selected = nullptr;
      continue;
    }
    if (candidate.judge.judgement != Kpoor || selected == nullptr ||
        std::llabs(selected->judge.Diff) > std::llabs(candidate.judge.Diff)) {
      selected = &candidate;
    }
  }
  if (selected == nullptr) {
    return result;
  }
  result.selectedSourceIndex = selected->sourceIndex;

  std::size_t count = 0;
  for (const auto &candidate : candidates) {
    if (count >= multiBadSourceIndices.size() ||
        candidate.sourceIndex == selected->sourceIndex || candidate.played ||
        candidate.judge.judgement != Bad) {
      continue;
    }
    multiBadSourceIndices[count++] = candidate.sourceIndex;
  }

  for (std::size_t index = 1; index < count; ++index) {
    std::size_t cursor = index;
    while (cursor > 0) {
      const auto *left =
          findCandidate(candidates, multiBadSourceIndices[cursor - 1]);
      const auto *right =
          findCandidate(candidates, multiBadSourceIndices[cursor]);
      if (left == nullptr || right == nullptr ||
          std::pair{left->timingMicros, left->sourceIndex} <=
              std::pair{right->timingMicros, right->sourceIndex}) {
        break;
      }
      std::swap(multiBadSourceIndices[cursor - 1],
                multiBadSourceIndices[cursor]);
      --cursor;
    }
  }

  if (selected->longNoteHead || selected->judge.judgement != Bad) {
    std::size_t kept = 0;
    for (std::size_t index = 0; index < count; ++index) {
      const auto *candidate =
          findCandidate(candidates, multiBadSourceIndices[index]);
      if (candidate != nullptr &&
          candidate->timingMicros < selected->timingMicros) {
        multiBadSourceIndices[kept++] = candidate->sourceIndex;
      }
    }
    count = kept;
  }

  std::size_t first = count;
  for (std::size_t index = 0; index < count; ++index) {
    const auto *candidate =
        findCandidate(candidates, multiBadSourceIndices[index]);
    if (candidate != nullptr &&
        (candidate->timingMicros >= selected->timingMicros ||
         !candidate->longNoteHead)) {
      first = index;
      break;
    }
  }
  if (first == count) {
    count = 0;
  } else if (first != 0) {
    for (std::size_t index = first; index < count; ++index) {
      multiBadSourceIndices[index - first] = multiBadSourceIndices[index];
    }
    count -= first;
  }

  result.multiBadCount = count;
  return result;
}

} // namespace gameplay
