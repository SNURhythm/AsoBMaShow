#include "GameplayCandidateRules.h"

#include <algorithm>

namespace gameplay {
namespace {

const JudgeCandidateDescriptor *findCandidate(
    std::span<const JudgeCandidateDescriptor> candidates,
    std::size_t sourceIndex) noexcept {
  const auto found = std::ranges::find(
      candidates, sourceIndex, &JudgeCandidateDescriptor::sourceIndex);
  return found == candidates.end() ? nullptr : &*found;
}

bool lr2ComboPrefers(const JudgeCandidateDescriptor &current,
                     const JudgeCandidateDescriptor &next) noexcept {
  if (current.judge.judgement != Bad || current.judge.Diff <= 0) {
    return false;
  }
  switch (next.judge.judgement) {
  case PGreat:
  case Great:
  case Good:
    return true;
  case Bad:
    return next.judge.Diff >= 0;
  case Kpoor:
  case Poor:
  case None:
  case JudgementCount:
    return false;
  }
  return false;
}

} // namespace

Lr2CandidateResolution resolveLr2Candidates(
    std::span<const JudgeCandidateDescriptor> candidates,
    std::span<std::size_t> multiBadSourceIndices) noexcept {
  Lr2CandidateResolution result;
  const JudgeCandidateDescriptor *selected = nullptr;
  for (const auto &candidate : candidates) {
    if (candidate.judge.judgement == None) {
      continue;
    }
    if (selected == nullptr || lr2ComboPrefers(*selected, candidate)) {
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
        candidate.sourceIndex == selected->sourceIndex ||
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
