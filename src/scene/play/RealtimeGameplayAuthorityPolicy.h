#pragma once

#include "GameplaySimulation.h"

#include <cstdint>
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

[[nodiscard]] RealtimeGameplayTerminalAction
classifyRealtimeGameplayTerminal(GameplayTerminalReason reason,
                                 bool sessionBackedPractice) noexcept;

[[nodiscard]] constexpr bool preparationInputUsesVisualOnlyPath(
    bool indicatorActive, bool sessionBackedPractice) noexcept {
  return indicatorActive && !sessionBackedPractice;
}

} // namespace gameplay
