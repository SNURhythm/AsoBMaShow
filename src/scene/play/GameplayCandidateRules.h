#pragma once

#include "Judgement.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace gameplay {

struct JudgeCandidateDescriptor {
  std::size_t sourceIndex = 0;
  std::int64_t timingMicros = 0;
  bool longNoteHead = false;
  JudgeResult judge = JudgeResult(None, 0);
};

struct Lr2CandidateResolution {
  std::optional<std::size_t> selectedSourceIndex;
  std::size_t multiBadCount = 0;
};

[[nodiscard]] Lr2CandidateResolution resolveLr2Candidates(
    std::span<const JudgeCandidateDescriptor> candidates,
    std::span<std::size_t> multiBadSourceIndices) noexcept;

} // namespace gameplay
