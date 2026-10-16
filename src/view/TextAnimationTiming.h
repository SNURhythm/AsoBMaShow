#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace text_animation {

struct MarqueeFrame {
  float offset = 0;
  bool moving = false;
  std::uint64_t redrawAt = 0;
};

inline MarqueeFrame marquee(int overflowWidth, std::uint64_t startedAt,
                            std::uint64_t now) {
  if (overflowWidth <= 0) return {};
  constexpr std::uint64_t startHold = 700;
  constexpr std::uint64_t edgeHold = 850;
  constexpr float pixelsPerSecond = 48.0f;
  const auto travel = std::max<std::uint64_t>(1,
      static_cast<std::uint64_t>(std::round(overflowWidth / pixelsPerSecond * 1000)));
  const auto cycle = startHold + travel + edgeHold + travel + edgeHold;
  auto phase = (now - startedAt) % cycle;
  if (phase < startHold) return {.redrawAt = now + startHold - phase};
  phase -= startHold;
  if (phase < travel) {
    return {.offset = overflowWidth * static_cast<float>(phase) / travel, .moving = true};
  }
  phase -= travel;
  if (phase < edgeHold) {
    return {.offset = static_cast<float>(overflowWidth), .redrawAt = now + edgeHold - phase};
  }
  phase -= edgeHold;
  if (phase < travel) {
    return {.offset = overflowWidth * (1.0f - static_cast<float>(phase) / travel), .moving = true};
  }
  phase -= travel;
  return {.redrawAt = now + edgeHold - phase + startHold};
}

struct CaretFrame {
  bool visible;
  std::uint64_t redrawAt;
};

inline CaretFrame caret(std::uint64_t startedAt, std::uint64_t now) {
  constexpr std::uint64_t interval = 500;
  const auto elapsed = now - startedAt;
  return {.visible = elapsed % (2 * interval) < interval,
          .redrawAt = now + interval - elapsed % interval};
}

} // namespace text_animation
