#pragma once

#include "BeatorajaSkinModel.h"
#include "../../audio/PlaybackRate.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace skin {

// Pinned Beatoraja c2ed5db1a46145ed10790c3872f717e95b59db9d BMSPlayer keeps
// STATE_PLAY alive for this period after its last playable note, with
// TIMER_ENDOFNOTE_1P enabled.
inline constexpr std::int32_t kBeatorajaEndOfNotesMarginMillis = 5'000;

[[nodiscard]] constexpr std::int64_t
nonnegativeAnimationMicros(std::int32_t milliseconds) noexcept {
  return milliseconds > 0
             ? static_cast<std::int64_t>(milliseconds) * 1'000
             : 0;
}

[[nodiscard]] constexpr std::int64_t
saturatingAnimationAdd(std::int64_t left, std::int64_t right) noexcept {
  return right > 0 && left > std::numeric_limits<std::int64_t>::max() - right
             ? std::numeric_limits<std::int64_t>::max()
             : left + right;
}

[[nodiscard]] constexpr std::int64_t
beatorajaGameplayStatePlayDeadlineMicros(std::int64_t terminalMicros) noexcept {
  return saturatingAnimationAdd(
      std::max<std::int64_t>(0, terminalMicros),
      static_cast<std::int64_t>(kBeatorajaEndOfNotesMarginMillis) * 1'000);
}

// The deadline is the source BMSPlayer lifecycle: last-note time, its fixed
// end-of-notes phase, then the selected PlaySkin's finish margin and fadeout.
// Negative authored margins have no positive wait because BMSPlayer's strict
// greater-than checks advance them on the next update.
[[nodiscard]] constexpr std::int64_t
gameplaySkinAnimationCompletionDeadlineMicros(
    std::int64_t terminalMicros,
    const SkinGameplayTiming &timing) noexcept {
  std::int64_t deadline =
      beatorajaGameplayStatePlayDeadlineMicros(terminalMicros);
  deadline = saturatingAnimationAdd(
      deadline, nonnegativeAnimationMicros(timing.finishMarginMillis));
  return saturatingAnimationAdd(deadline,
                                 nonnegativeAnimationMicros(timing.fadeoutMillis));
}

// AudioWrapper intentionally clamps its presentation clock when its scheduled
// audio ends. BMSPlayer's TIMER_PLAY instead continues from the main timer at
// the current frequency for its post-audio state machine. The continuation is
// anchored at the observed audio-end time, not gameplay start.
[[nodiscard]] inline std::int64_t beatorajaGameplayFrameClockMicros(
    std::int64_t observedGameplayMicros,
    std::int64_t continuationGameplayStartMicros,
    std::int64_t endingSteadyStartMicros, std::int64_t currentSteadyMicros,
    audio::PlaybackRate playbackRate) noexcept {
  if (observedGameplayMicros < continuationGameplayStartMicros) {
    return observedGameplayMicros;
  }
  const std::int64_t steadyElapsedMicros =
      std::max<std::int64_t>(0, currentSteadyMicros - endingSteadyStartMicros);
  const std::int64_t continuedGameplayMicros = saturatingAnimationAdd(
      continuationGameplayStartMicros,
      playbackRate.chartMicrosFromReal(steadyElapsedMicros));
  return std::max(observedGameplayMicros, continuedGameplayMicros);
}

// Kept separate from selected-skin timing because BMSPlayer's STATE_PLAY
// transition applies to ordinary gameplay as well. Its source condition is
// `playtime < ptime`, where ptime is the integer-millisecond TIMER_PLAY.
[[nodiscard]] constexpr bool
beatorajaGameplayStateFinished(std::int64_t gameplayTimeMicros,
                               std::int32_t playtimeMillis) noexcept {
  return gameplayTimeMicros / 1'000 >
         static_cast<std::int64_t>(playtimeMillis);
}

} // namespace skin
