#include "RealtimeGameplayAuthorityPolicy.h"

#include <limits>

namespace gameplay {

RealtimeGameplayAuthorityPolicy makeRealtimeGameplayAuthorityPolicy(
    const RealtimeGameplayAuthorityPolicyInput &input) noexcept {
  RealtimeGameplayAuthorityPolicy result;
  const bool sessionBackedPractice = input.practiceRange.has_value();
  const bool legacyPractice = input.practiceMode && !sessionBackedPractice;
  result.eligible =
      !input.replayPlayback && !legacyPractice &&
      (input.autoPlay ||
       (input.nativeManualInputAvailable && input.inputHandlerAvailable));
  if (sessionBackedPractice) {
    result.allowedNoteRange = input.practiceRange;
    result.practiceCompletionSongTimeMicros = input.practiceRange->endMicros;
  } else {
    if (input.startPositionMicros > 0) {
      result.allowedNoteRange = GameplayTimeRange{
          .startMicros = input.startPositionMicros,
          .endMicros = std::numeric_limits<std::int64_t>::max(),
      };
    }
    result.activationSongTimeMicros =
        input.preparationActivationSongTimeMicros;
  }
  return result;
}

} // namespace gameplay
