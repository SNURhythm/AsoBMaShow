#pragma once

#include <algorithm>
#include <cmath>

namespace lane_cover {

// Percent is retained at sub-percent precision for START + analog scratch.
struct State {
  float laneCoverPercent = 0;
  bool laneCoverEnabled = false;
  bool liftEnabled = false;
  float liftRatio = 0.1F;
  bool hiddenEnabled = false;
  float hiddenRatio = 0.1F;

  bool operator==(const State &) const = default;
};

inline float ratio(float value) noexcept {
  return std::isfinite(value) ? std::clamp(value, 0.0F, 1.0F) : 0.0F;
}

inline bool valid(const State &state) noexcept {
  return std::isfinite(state.laneCoverPercent) && state.laneCoverPercent >= 0 &&
         state.laneCoverPercent <= 100 && std::isfinite(state.liftRatio) &&
         state.liftRatio >= 0 && state.liftRatio <= 1 &&
         std::isfinite(state.hiddenRatio) && state.hiddenRatio >= 0 && state.hiddenRatio <= 1;
}

enum class Target { Sudden, Lift, Hidden };

inline Target adjustmentTarget(const State &state, bool changeLift) noexcept {
  if (state.laneCoverEnabled || (!state.liftEnabled && !state.hiddenEnabled))
    return Target::Sudden;
  return state.liftEnabled && (!state.hiddenEnabled || changeLift)
             ? Target::Lift : Target::Hidden;
}

// Positive input moves the selected edge down, matching ControlInputProcessor.
inline bool adjust(State &state, float deltaPercent, bool changeLift) noexcept {
  if (!std::isfinite(deltaPercent) || deltaPercent == 0) return false;
  const auto previous = state;
  switch (adjustmentTarget(state, changeLift)) {
  case Target::Sudden:
    state.laneCoverPercent = ratio((state.laneCoverPercent + deltaPercent) / 100) * 100;
    break;
  case Target::Lift:
    state.liftRatio = ratio(state.liftRatio - deltaPercent / 100);
    break;
  case Target::Hidden:
    state.hiddenRatio = ratio(state.hiddenRatio - deltaPercent / 100);
    break;
  }
  return state != previous;
}

struct Geometry {
  float judgeY;
  float suddenY;
  float hiddenY;
};

inline Geometry geometry(const State &state, float baseJudgeY, float top) noexcept {
  const float height = std::max(0.0F, top - baseJudgeY);
  const float judge = baseJudgeY + height * (state.liftEnabled ? ratio(state.liftRatio) : 0);
  const float remaining = std::max(0.0F, top - judge);
  return {.judgeY = judge,
          .suddenY = top - remaining * (state.laneCoverEnabled ? ratio(state.laneCoverPercent / 100) : 0),
          .hiddenY = judge + remaining * (state.hiddenEnabled ? ratio(state.hiddenRatio) : 0)};
}

} // namespace lane_cover
