#pragma once

#include <cstdint>

namespace gameplay_chart_entity_render_budget {

inline constexpr uint32_t kMaxRectanglesPerFrame = 60'000U;
inline constexpr uint32_t kSingleRectangleEntityCost = 1U;
inline constexpr uint32_t kLongNoteReservationCost = 3U;
inline constexpr uint32_t kReplayGhostOutlineCost = 4U;
inline constexpr uint32_t kReplayMissMarkerCost = 14U;

class Budget {
public:
  [[nodiscard]] bool tryConsume(uint32_t rectangleCount) {
    if (isExhausted || rectangleCount > remainingRectangles) {
      isExhausted = true;
      return false;
    }
    remainingRectangles -= rectangleCount;
    isExhausted = remainingRectangles == 0U;
    return true;
  }

  void reset() {
    remainingRectangles = kMaxRectanglesPerFrame;
    isExhausted = false;
  }

  [[nodiscard]] uint32_t remaining() const { return remainingRectangles; }
  [[nodiscard]] bool exhausted() const { return isExhausted; }

private:
  uint32_t remainingRectangles = kMaxRectanglesPerFrame;
  bool isExhausted = false;
};

} // namespace gameplay_chart_entity_render_budget
