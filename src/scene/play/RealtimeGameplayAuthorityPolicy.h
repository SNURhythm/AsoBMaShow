#pragma once

#include "GameplaySimulation.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace gameplay {

struct RealtimeGameplayAuthorityPolicyInput {
  bool nativeManualInputAvailable = false;
  bool autoPlay = false;
  bool inputHandlerAvailable = false;
  bool replayPlayback = false;
  bool practiceMode = false;
  std::optional<GameplayTimeRange> practiceRange;
  std::int64_t startPositionMicros = 0;
  std::optional<std::int64_t> preparationActivationSongTimeMicros;
};

struct RealtimeGameplayAuthorityPolicy {
  bool eligible = false;
  std::optional<GameplayTimeRange> allowedNoteRange;
  std::optional<std::int64_t> practiceCompletionSongTimeMicros;
  std::optional<std::int64_t> activationSongTimeMicros;
};

enum class RealtimeGameplayTerminalAction {
  Wait,
  CompleteChart,
  CompletePractice,
  SurvivalGaugeFailed,
  IntegrityFailure,
};

[[nodiscard]] RealtimeGameplayAuthorityPolicy
makeRealtimeGameplayAuthorityPolicy(
    const RealtimeGameplayAuthorityPolicyInput &input) noexcept;

[[nodiscard]] constexpr bool shouldAttemptRealtimeGameplayReset(
    bool laneControllerAvailable, bool inputHandlerAvailable,
    bool autoPlay) noexcept {
  return laneControllerAvailable && (inputHandlerAvailable || autoPlay);
}

[[nodiscard]] constexpr bool
shouldSuspendRealtimeGameplayForPause(bool pausePlayback) noexcept {
  return pausePlayback;
}

// A skin session publishes authored lane and control geometry only after its
// first submitted frame. iOS raw input authority retries at that boundary,
// never against an empty startup layout.
[[nodiscard]] constexpr bool
shouldRetryRealtimeGameplayAuthorityAfterSkinFrame(
    bool awaitingSkinGeometry, bool skinLaneGeometryPublished) noexcept {
  return awaitingSkinGeometry && skinLaneGeometryPublished;
}

[[nodiscard]] constexpr std::size_t realtimeGameplayReplayCapacity(
    std::size_t noteCount,
    std::int64_t finalTimelineTimeMicros) noexcept {
  constexpr std::size_t baseCapacity = 4096;
  constexpr std::size_t eventsPerSecond = 256;
  constexpr std::size_t durationCapacityLimit = 1U << 20U;
  constexpr std::size_t maximum = std::numeric_limits<std::size_t>::max();

  const std::size_t noteCapacity =
      noteCount > (maximum - 1024) / 3 ? maximum : noteCount * 3 + 1024;
  const std::uint64_t durationMicros =
      finalTimelineTimeMicros > 0
          ? static_cast<std::uint64_t>(finalTimelineTimeMicros)
          : 0;
  const std::uint64_t durationSeconds =
      durationMicros / 1'000'000U +
      (durationMicros % 1'000'000U != 0 ? 1U : 0U);
  const std::size_t maximumDurationSeconds =
      (durationCapacityLimit - baseCapacity) / eventsPerSecond;
  const std::size_t boundedDurationSeconds =
      durationSeconds > maximumDurationSeconds
          ? maximumDurationSeconds
          : static_cast<std::size_t>(durationSeconds);
  const std::size_t durationCapacity =
      baseCapacity + boundedDurationSeconds * eventsPerSecond;
  return noteCapacity > durationCapacity ? noteCapacity : durationCapacity;
}

[[nodiscard]] RealtimeGameplayTerminalAction
classifyRealtimeGameplayTerminal(GameplayTerminalReason reason,
                                 bool sessionBackedPractice,
                                 bool sourcePlaytimeElapsed = false) noexcept;

[[nodiscard]] constexpr bool preparationInputUsesVisualOnlyPath(
    bool indicatorActive, bool sessionBackedPractice) noexcept {
  return indicatorActive && !sessionBackedPractice;
}

} // namespace gameplay
