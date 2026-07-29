#include "ReplayCapabilities.h"

namespace replay {
namespace {

bool invalidFileIsPresent(ReplayState state) noexcept {
  return state == ReplayState::Corrupt ||
         state == ReplayState::Mismatched ||
         state == ReplayState::UnsupportedExtension;
}

void addVerifiedOwnedReplayCapabilities(ReplayCapabilities &value,
                                        bool chart) noexcept {
  value.watch = true;
  value.retrySame = true;
  value.gBattle = chart;
  value.practiceGhost = chart;
  value.videoExport = true;
  value.shareOrCopy = true;
  value.deleteReplayFile = true;
  value.profileDuplicateReplay = true;
  value.profileArchiveReplay = true;
}

} // namespace

ReplayCapabilities capabilitiesFor(ReplayCapabilityInput input) noexcept {
  ReplayCapabilities result;
  switch (input.origin) {
  case RecordOrigin::ModernChartResult:
  case RecordOrigin::ModernCourseResult: {
    result.recordsList = true;
    result.viewResult = true;
    result.irUpload = input.postponedIrSnapshotEligible;
    result.profileDuplicateRecord = true;
    result.profileArchiveRecord = true;
    if (input.replayState == ReplayState::Verified) {
      addVerifiedOwnedReplayCapabilities(
          result, input.origin == RecordOrigin::ModernChartResult);
    } else if (invalidFileIsPresent(input.replayState)) {
      result.deleteReplayFile = true;
    }
    return result;
  }
  case RecordOrigin::LegacyChartSummary:
  case RecordOrigin::LegacyCourseSummary:
    result.recordsList = true;
    result.profileDuplicateRecord = true;
    result.profileArchiveRecord = true;
    return result;
  case RecordOrigin::ImportedStockBrd:
    if (input.replayState == ReplayState::Verified) {
      result.watch = true;
      result.shareOrCopy = true;
      result.deleteReplayFile = true;
      result.profileDuplicateReplay = true;
      result.profileArchiveReplay = true;
    } else if (invalidFileIsPresent(input.replayState)) {
      result.deleteReplayFile = true;
    }
    return result;
  case RecordOrigin::ImportedRemoteResult:
    result.recordsList = true;
    result.viewResult = true;
    result.profileDuplicateRecord = true;
    result.profileArchiveRecord = true;
    return result;
  }
  return result;
}

} // namespace replay
