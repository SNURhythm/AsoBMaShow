#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>

struct ScrollbarPresentation {
  float alpha = 0.0f;
  bool animate = false;
  std::optional<std::uint64_t> redrawAt;
};

// Shared by the visible scrollbar and its redraw demand so a fully transparent
// first fade-in sample still schedules the next frame.
inline ScrollbarPresentation scrollbarPresentation(std::uint64_t now,
    std::uint64_t fadeInStartedAt, std::uint64_t lastActivityAt, bool dragging) {
  if (lastActivityAt == 0) return {};
  if (dragging) return {.alpha = 1.0f};
  constexpr std::uint64_t fadeInMs = 120;
  constexpr std::uint64_t holdMs = 650;
  constexpr std::uint64_t fadeOutMs = 480;
  const auto elapsed = now > fadeInStartedAt ? now - fadeInStartedAt : 0;
  const auto inactive = now > lastActivityAt ? now - lastActivityAt : 0;
  const float fadeIn = fadeInStartedAt == 0 ? 1.0f
      : std::min(1.0f, static_cast<float>(elapsed) / fadeInMs);
  const float fadeOut = inactive <= holdMs ? 1.0f
      : 1.0f - std::min(1.0f, static_cast<float>(inactive - holdMs) / fadeOutMs);
  const float linear = std::clamp(std::min(fadeIn, fadeOut), 0.0f, 1.0f);
  ScrollbarPresentation result{.alpha = linear * linear * (3.0f - 2.0f * linear)};
  if (inactive >= holdMs + fadeOutMs) return result;
  result.animate = fadeIn < 1.0f || inactive >= holdMs;
  if (!result.animate) result.redrawAt = lastActivityAt + holdMs;
  return result;
}
