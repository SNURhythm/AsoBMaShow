#pragma once

#include "GameplayGaugeRules.h"
#include "Judgement.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

inline constexpr std::size_t kSkinNormalDistributionBucketCount = 7;
inline constexpr std::size_t kSkinJudgeDistributionBucketCount = 6;
inline constexpr std::size_t kSkinEarlyLateDistributionBucketCount = 10;
inline constexpr std::size_t kSkinRecentJudgeTimingCapacity = 100;
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
  int second = 0;
  bool countsTowardJudgement = false;
  std::uint32_t redirectSourceId = kInvalidSkinGameplayGraphSourceId;

  bool operator==(const SkinGameplayGraphNote &) const = default;
};

struct SkinGameplayChartGraphState {
  std::vector<SkinNormalDistribution> normalDistribution;
  std::size_t judgementDistributionSeconds = 0;
  std::vector<SkinBpmGraphPoint> bpmSeries;
  std::vector<SkinGameplayGraphNote> judgementNotes;
  double mainBpm = 0.0;
  double minimumBpm = 0.0;
  double maximumBpm = 0.0;

  bool operator==(const SkinGameplayChartGraphState &) const = default;
};

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
  std::array<std::int64_t, kSkinRecentJudgeTimingCapacity>
      recentJudgeTimingsMillis = emptySkinRecentJudgeTimings();
  // JudgeManager increments the index before storing a timing. Consumers use
  // this exact source index rather than a reordered oldest-first window.
  std::size_t recentJudgeTimingIndex = 0;
  std::array<SkinJudgeWindow, 5> judgeWindows{};
  std::vector<float> gaugeHistory;
  GaugeType gaugeType = GaugeType::Normal;
  float gaugeMinimum = 0.0F;
  float gaugeMaximum = 100.0F;
  float gaugeBorder = 0.0F;
  bool gaugeSupported = false;

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
  std::span<const std::int64_t> recentJudgeTimingsMillis;
  std::size_t recentJudgeTimingIndex = 0;
  std::span<const float> gaugeHistory;
  int gaugeType = 0;
  float gaugeMinimum = 0.0F;
  float gaugeMaximum = 100.0F;
  float gaugeBorder = 0.0F;
  bool gaugeSupported = false;
};

[[nodiscard]] SkinGameplayGraphStateView
skinGameplayGraphStateView(const SkinGameplayGraphState &) noexcept;

class SkinGameplayGraphAccumulator {
public:
  SkinGameplayGraphAccumulator() = default;
  SkinGameplayGraphAccumulator(
      std::vector<SkinGameplayGraphNote> notes, std::size_t secondCount,
      std::array<SkinJudgeWindow, 5> judgeWindows,
      std::size_t gaugeHistoryCapacity);

  void reset(std::vector<SkinGameplayGraphNote> notes,
             std::size_t secondCount,
             std::array<SkinJudgeWindow, 5> judgeWindows,
             std::size_t gaugeHistoryCapacity);
  void applyJudge(std::uint32_t sourceId, const JudgeResult &judge);
  [[nodiscard]] bool setGauge(GaugeType type,
                              const GameplayGaugeRules &rules) noexcept;
  [[nodiscard]] bool recordGauge(float value, GaugeType type,
                                 const GameplayGaugeRules &rules);

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
  std::size_t gaugeHistoryCapacity_ = 0;
};
