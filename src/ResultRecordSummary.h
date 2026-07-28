#pragma once

#include "ir/IrProfileSettings.h"
#include "replay/ReplayCapabilities.h"
#include "repositories/ReplayRepository.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

struct LocalReplayRecordId {
  int replayId = 0;

  bool operator==(const LocalReplayRecordId &) const = default;
};

struct IrRemoteRecordId {
  std::string providerId;
  std::string serverOrigin;
  std::string remoteScoreId;

  bool operator==(const IrRemoteRecordId &) const = default;
};

struct ModernChartRecordId {
  std::string attemptId;

  bool operator==(const ModernChartRecordId &) const = default;
};

struct ModernCourseRecordId {
  std::string attemptId;

  bool operator==(const ModernCourseRecordId &) const = default;
};

struct LegacyChartRecordId {
  int legacyReplayId = 0;

  bool operator==(const LegacyChartRecordId &) const = default;
};

struct LegacyCourseRecordId {
  int legacyCourseReplayId = 0;

  bool operator==(const LegacyCourseRecordId &) const = default;
};

using ResultRecordIdentity =
    std::variant<LocalReplayRecordId, ModernChartRecordId, ModernCourseRecordId,
                 LegacyChartRecordId, LegacyCourseRecordId, IrRemoteRecordId>;

struct ResultRecordIdentityHash {
  [[nodiscard]] std::size_t
  operator()(const ResultRecordIdentity &identity) const noexcept;
};

struct ResultRecordCapabilities {
  bool watch = false;
  bool retrySame = false;
  bool gBattle = false;
  bool practiceGhost = false;
  bool resultRecall = false;
  bool videoExport = false;
  bool shareOrCopy = false;
  bool deleteReplayFile = false;
  bool irUpload = false;

  bool operator==(const ResultRecordCapabilities &) const = default;
};

// Stable keys use length-framed, validated identity components. This bound
// keeps every factory-produced key suitable for RecyclerView identity use
// without introducing collision-prone hashes or sentinel replay IDs.
inline constexpr std::size_t kMaximumResultRecordStableKeyBytes =
    ir::kMaximumIrProviderIdBytes + ir::kMaximumServerOriginBytes +
    ir::kMaximumIrRemoteScoreIdBytes +
    3 * (std::numeric_limits<std::size_t>::digits10 + 1) + 16;

struct ResultRecordSummary {
  ResultRecordIdentity identity;
  ResultRecordCapabilities capabilities;
  bool course = false;
  bool autoPlay = false;
  int score = 0;
  int maxScore = 0;
  std::optional<int> maxCombo;
  int clearRank = kClearTypeFailedRank;
  bool scoreAvailable = true;
  bool maxScoreAvailable = true;
  bool clearRankAvailable = true;
  std::int64_t displayedTimeUnixMillis = 0;
  std::string displayedTime;
  std::optional<std::string> playOption;
  ir::IrRecordState irState = ir::IrRecordState::Hidden;
  std::optional<ReplaySummary> local;
  std::optional<ModernChartResultRecord> modern;
  std::optional<ModernCourseResultRecord> modernCourse;
  replay::ReplayState replayState = replay::ReplayState::NotApplicable;
  std::optional<ir::IrRemoteScore> remote;
  std::optional<LegacyChartResultSummary> legacyChart;
  std::optional<LegacyCourseResultSummary> legacyCourse;

  [[nodiscard]] bool isLocal() const noexcept;
  [[nodiscard]] bool isModernChart() const noexcept;
  [[nodiscard]] bool isModernCourse() const noexcept;
  [[nodiscard]] bool isLegacyChart() const noexcept;
  [[nodiscard]] bool isLegacyCourse() const noexcept;
  [[nodiscard]] bool isRemote() const noexcept;
  [[nodiscard]] std::optional<int> localReplayId() const noexcept;
  [[nodiscard]] std::optional<std::string_view>
  modernAttemptId() const noexcept;
  [[nodiscard]] std::optional<std::string_view> remoteScoreId() const noexcept;
  [[nodiscard]] std::string stableKey() const;
};

[[nodiscard]] ResultRecordSummary makeLocalResultRecord(ReplaySummary summary);

[[nodiscard]] ResultRecordSummary
makeLegacyChartResultRecord(LegacyChartResultSummary summary);

[[nodiscard]] ResultRecordSummary
makeLegacyCourseResultRecord(LegacyCourseResultSummary summary);

[[nodiscard]] ResultRecordSummary
makeModernChartResultRecord(ModernChartResultRecord record,
                            replay::ReplayState replayState,
                            bool postponedIrSnapshotEligible);

[[nodiscard]] ResultRecordSummary
makeModernCourseResultRecord(ModernCourseResultRecord record,
                             replay::ReplayState replayState);

// Throws std::invalid_argument when the provider/origin identity is not
// canonical or when the copied remote score fails stored-model validation.
[[nodiscard]] ResultRecordSummary
makeRemoteResultRecord(std::string_view providerId,
                       std::string_view serverOrigin, ir::IrRemoteScore score);

// Produces the complete newest-first Records projection without mutating the
// remote mirror. Only an exact receipt in the requested scope suppresses its
// linked standalone remote row; all local attempts remain visible.
[[nodiscard]] std::vector<ResultRecordSummary>
mergeResultRecords(std::span<const ReplaySummary> local,
                   std::span<const ir::IrRemoteScore> remote,
                   std::string_view providerId, std::string_view serverOrigin);

[[nodiscard]] std::vector<ResultRecordSummary>
mergeResultRecords(std::span<const ReplaySummary> local,
                   std::span<const ResultRecordSummary> modern,
                   std::span<const ir::IrRemoteScore> remote,
                   std::string_view providerId, std::string_view serverOrigin);
