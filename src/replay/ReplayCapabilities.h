#pragma once

#include <cstdint>

namespace replay {

enum class RecordOrigin : std::uint8_t {
  ModernChartResult,
  ModernCourseResult,
  LegacyChartSummary,
  LegacyCourseSummary,
  ImportedStockBrd,
  ImportedRemoteResult,
};

enum class ReplayState : std::uint8_t {
  NotApplicable,
  Verified,
  UserDeleted,
  Missing,
  Corrupt,
  Mismatched,
  UnsupportedExtension,
};

struct ReplayCapabilityInput {
  RecordOrigin origin = RecordOrigin::ModernChartResult;
  ReplayState replayState = ReplayState::NotApplicable;
  bool postponedIrSnapshotEligible = false;
};

struct ReplayCapabilities {
  bool recordsList = false;
  bool viewResult = false;
  bool watch = false;
  bool retrySame = false;
  bool gBattle = false;
  bool practiceGhost = false;
  bool videoExport = false;
  bool shareOrCopy = false;
  bool deleteReplayFile = false;
  bool irUpload = false;
  bool profileDuplicateRecord = false;
  bool profileDuplicateReplay = false;
  bool profileArchiveRecord = false;
  bool profileArchiveReplay = false;

  bool operator==(const ReplayCapabilities &) const = default;
};

[[nodiscard]] ReplayCapabilities
capabilitiesFor(ReplayCapabilityInput input) noexcept;

} // namespace replay
