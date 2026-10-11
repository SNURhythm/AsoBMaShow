#pragma once

namespace judgement_indicator {

// Both live-authority and persisted replay actions use these input names.
// Misses have no input timing; classic LN releases contain a combined scoring
// error, whose head timing has already been recorded at the press.
template <typename Action>
[[nodiscard]] constexpr bool recordsInputTiming(
    Action action, bool classicLongNoteTail) noexcept {
  return action == Action::Press || action == Action::MultiBad ||
         (action == Action::Release && !classicLongNoteTail);
}

} // namespace judgement_indicator
