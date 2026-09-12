#pragma once

#include "GameplayGaugeRules.h"
#include "Judgement.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

inline constexpr std::size_t kSkinNormalDistributionBucketCount = 7;
inline constexpr std::size_t kSkinJudgeDistributionBucketCount = 6;
inline constexpr std::size_t kSkinEarlyLateDistributionBucketCount = 10;
inline constexpr std::size_t kSkinRecentJudgeTimingCapacity = 100;
inline constexpr std::size_t kSkinMaximumNormalGraphSamples = 1638;
inline constexpr std::size_t kSkinMaximumGaugeGraphSamples = 4096;
inline constexpr std::int64_t kSkinEmptyJudgeTimingMillis =
    std::numeric_limits<std::int64_t>::min();
inline constexpr std::uint32_t kInvalidSkinGameplayGraphSourceId =
    std::numeric_limits<std::uint32_t>::max();

using SkinNormalDistribution =
    std::array<int, kSkinNormalDistributionBucketCount>;
using SkinJudgeDistribution =
    std::array<int, kSkinJudgeDistributionBucketCount>;
using SkinEarlyLateDistribution =
    std::array<int, kSkinEarlyLateDistributionBucketCount>;
using SkinGaugeHistoryCollection =
    std::array<std::vector<float>, kGaugeTypeCount>;

[[nodiscard]] constexpr std::uint64_t
skinGameplayGraphSecondCount(std::int64_t timeMicros) noexcept {
  return static_cast<std::uint64_t>(timeMicros > 0 ? timeMicros : 0) /
             1'000'000U +
         1U;
}

[[nodiscard]] constexpr std::size_t
skinGameplayGraphDistributionSize(std::uint64_t seconds) noexcept {
  return seconds < kSkinMaximumNormalGraphSamples
             ? static_cast<std::size_t>(seconds)
             : 0;
}

[[nodiscard]] constexpr bool
skinGameplayGaugeDurationAdmitted(std::uint64_t seconds) noexcept {
  return seconds <= kSkinMaximumGaugeGraphSamples / 2;
}

[[nodiscard]] constexpr std::size_t
skinGameplayGaugeHistoryCapacityHint(std::int64_t timeMicros) noexcept {
  const std::uint64_t samples =
      static_cast<std::uint64_t>(timeMicros > 0 ? timeMicros : 0) / 500'000U + 2U;
  return samples > std::numeric_limits<std::size_t>::max()
             ? std::numeric_limits<std::size_t>::max()
             : static_cast<std::size_t>(samples);
}

struct SkinBpmGraphPoint {
  std::int64_t chartTimeMicros = 0;
  std::uint32_t sourceOrder = 0;
  double bpm = 0.0;
  double scroll = 1.0;
  double bpmTimesScroll = 0.0;
  std::int64_t stopMicros = 0;
  double graphSpeed = 0.0;
  bool emitsGraphPoint = false;
  bool synthetic = false;

  bool operator==(const SkinBpmGraphPoint &) const = default;
};

struct SkinGameplayGraphNote {
  std::uint32_t sourceId = kInvalidSkinGameplayGraphSourceId;
  std::int64_t second = 0;
  bool countsTowardJudgement = false;
  std::uint32_t redirectSourceId = kInvalidSkinGameplayGraphSourceId;

  bool operator==(const SkinGameplayGraphNote &) const = default;
};

struct SkinGameplayChartGraphState {
  std::vector<SkinNormalDistribution> normalDistribution;
  std::uint64_t judgementDistributionSeconds = 0;
  bool distributionOmitted = false;
  bool durationUnavailable = false;
  bool bpmSeriesOmitted = false;
  std::vector<SkinBpmGraphPoint> bpmSeries;
  std::vector<SkinGameplayGraphNote> judgementNotes;
  double mainBpm = 0.0;
  double minimumBpm = 0.0;
  double maximumBpm = 0.0;
  // Beatoraja's SongInformation-backed result properties. These belong to
  // the prepared chart, rather than the mutable gameplay graph samples.
  std::optional<int> normalKeyNotes;
  std::optional<int> longKeyNotes;
  std::optional<int> normalScratchNotes;
  std::optional<int> longScratchNotes;
  std::optional<double> peakDensity;
  std::optional<double> endDensity;
  std::optional<double> averageDensity;
  std::optional<double> totalGauge;
  std::optional<int> selectedLongNoteMode;
  std::optional<bool> hasAnyLongNote;
  std::optional<bool> hasUndefinedLongNote;
  std::optional<bool> hasLongNote;
  std::optional<bool> hasChargeNote;
  std::optional<bool> hasHellChargeNote;
  std::optional<bool> hasBga;
  std::optional<bool> hasRandomSequence;
  std::optional<bool> hasBpmStop;

  bool operator==(const SkinGameplayChartGraphState &) const = default;
};

[[nodiscard]] inline std::optional<std::int64_t>
skinGameplayGraphDurationMicros(const SkinGameplayChartGraphState &chart) noexcept {
  if (chart.durationUnavailable ||
      chart.judgementDistributionSeconds >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) /
              1'000'000U) {
    return std::nullopt;
  }
  return static_cast<std::int64_t>(chart.judgementDistributionSeconds) *
         1'000'000LL;
}

struct SkinJudgeWindow {
  Judgement judgement = None;
  int minimumTimingMillis = 0;
  int maximumTimingMillis = 0;

  bool operator==(const SkinJudgeWindow &) const = default;
};

[[nodiscard]] constexpr std::array<std::int64_t,
                                   kSkinRecentJudgeTimingCapacity>
emptySkinRecentJudgeTimings() noexcept {
  std::array<std::int64_t, kSkinRecentJudgeTimingCapacity> result{};
  result.fill(kSkinEmptyJudgeTimingMillis);
  return result;
}

struct SkinGameplayDynamicGraphState {
  std::vector<SkinJudgeDistribution> judgementDistribution;
  std::vector<SkinEarlyLateDistribution> earlyLateDistribution;
  bool distributionOmitted = false;
  bool gaugeHistoryOmitted = false;
  std::array<std::int64_t, kSkinRecentJudgeTimingCapacity>
      recentJudgeTimingsMillis = emptySkinRecentJudgeTimings();
  // JudgeManager increments the index before storing a timing. Consumers use
  // this exact source index rather than a reordered oldest-first window.
  std::size_t recentJudgeTimingIndex = 0;
  std::array<SkinJudgeWindow, 5> judgeWindows{};
  SkinGaugeHistoryCollection gaugeHistories;
  // Course-result gauge graphs separate each stage with a white line. The
  // values are cumulative history lengths, matching SkinGaugeGraphObject.
  std::vector<std::size_t> gaugeHistorySections;
  GaugeType gaugeType = GaugeType::Normal;
  float gaugeMinimum = 0.0F;
  float gaugeMaximum = 100.0F;
  float gaugeBorder = 0.0F;
  bool gaugeSupported = false;
  // Source-specific monotonic revisions let retained Pixmaps compare exact
  // authority changes without hashing or copying the published spans.
  std::uint64_t judgementRevision = 0;
  std::uint64_t gaugeRevision = 0;

  bool operator==(const SkinGameplayDynamicGraphState &) const = default;
};

struct SkinGameplayGraphState {
  std::shared_ptr<const SkinGameplayChartGraphState> chart;
  std::shared_ptr<const SkinGameplayDynamicGraphState> dynamic;
};

struct SkinGameplayGraphStateView {
  std::span<const SkinNormalDistribution> normalDistribution;
  std::span<const SkinJudgeDistribution> judgementDistribution;
  std::span<const SkinEarlyLateDistribution> earlyLateDistribution;
  std::span<const SkinBpmGraphPoint> bpmSeries;
  double mainBpm = 0.0;
  double minimumBpm = 0.0;
  double maximumBpm = 0.0;
  std::span<const SkinJudgeWindow> judgeWindows;
  std::span<const int> timingDistribution;
  int timingDistributionCenter = 150;
  std::optional<double> timingDistributionAverageMillis;
  std::optional<double> timingDistributionStandardDeviationMillis;
  std::span<const std::int64_t> recentJudgeTimingsMillis;
  std::size_t recentJudgeTimingIndex = 0;
  std::span<const float> gaugeHistory;
  std::span<const std::size_t> gaugeHistorySections;
  int gaugeType = 0;
  float gaugeMinimum = 0.0F;
  float gaugeMaximum = 100.0F;
  float gaugeBorder = 0.0F;
  bool gaugeSupported = false;
  std::uint64_t judgementRevision = 0;
  std::uint64_t gaugeRevision = 0;
};

[[nodiscard]] SkinGameplayGraphStateView
skinGameplayGraphStateView(const SkinGameplayGraphState &) noexcept;

void copySkinGameplayGaugeHistoryForDisplay(
    SkinGameplayDynamicGraphState &target,
    const SkinGaugeHistoryCollection &histories,
    std::span<const float> fallback, GaugeType type);

// Course result consumers concatenate completed chart snapshots. Keeping the
// merge here lets interactive, saved, and exported results share the same
// graph authority.
[[nodiscard]] SkinGameplayGraphState combineSkinGameplayGraphStates(
    std::span<const SkinGameplayGraphState> stages);

class SkinGameplayGraphAccumulator {
public:
  SkinGameplayGraphAccumulator() = default;
  SkinGameplayGraphAccumulator(
      std::vector<SkinGameplayGraphNote> notes, std::uint64_t secondCount,
      std::array<SkinJudgeWindow, 5> judgeWindows,
      std::size_t gaugeHistoryCapacity);

  void reset(std::vector<SkinGameplayGraphNote> notes,
             std::uint64_t secondCount,
             std::array<SkinJudgeWindow, 5> judgeWindows,
             std::size_t gaugeHistoryCapacity);
  void applyJudge(std::uint32_t sourceId, const JudgeResult &judge);
  [[nodiscard]] bool setGauge(GaugeType type,
                              const GameplayGaugeRules &rules) noexcept;
  [[nodiscard]] bool updateGaugeState(
      const std::array<float, kGaugeTypeCount> &values, GaugeType type,
      const GameplayGaugeRules &rules);
  [[nodiscard]] bool advanceGaugeHistoryTo(std::int64_t playTimeMicros);

  [[nodiscard]] const SkinGameplayDynamicGraphState &state() const noexcept {
    return state_;
  }

private:
  struct NoteState {
    SkinGameplayGraphNote definition;
    int state = 0;
    std::int64_t playTimeMillis = 0;
  };

  [[nodiscard]] NoteState *resolvedNote(std::uint32_t sourceId) noexcept;
  [[nodiscard]] static int judgeState(Judgement judgement) noexcept;
  [[nodiscard]] static int earlyLateBucket(int state,
                                           std::int64_t playTimeMillis) noexcept;

  SkinGameplayDynamicGraphState state_;
  std::vector<NoteState> notes_;
  std::unordered_map<std::uint32_t, std::size_t> noteIndices_;
  std::array<float, kGaugeTypeCount> gaugeValues_{};
  std::size_t gaugeHistoryCapacity_ = 0;
  std::int64_t nextGaugeSampleMicros_ = 0;
  bool gaugeValuesInitialized_ = false;
};
