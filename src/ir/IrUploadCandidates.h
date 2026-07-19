#pragma once

#include "IrReplayRecordState.h"
#include "../repositories/ChartRepository.h"

#include <cstddef>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

namespace ir {

struct IrUploadCandidate {
  ReplaySummary replay;
  ChartMetaRecord chart;
  IrRecordState state = IrRecordState::Hidden;

  [[nodiscard]] int replayId() const noexcept { return replay.id; }
};

struct IrUploadCandidateProjection {
  std::vector<IrUploadCandidate> candidates;
  std::size_t omittedRows = 0;
  std::string diagnostic;
};

[[nodiscard]] IrUploadCandidateProjection projectIrUploadCandidates(
    std::span<const ReplaySummary> replays,
    std::span<const ChartMetaRecord> charts) noexcept;

void intersectIrUploadSelection(
    std::unordered_set<int> &selectedReplayIds,
    std::span<const IrUploadCandidate> candidates);

} // namespace ir
