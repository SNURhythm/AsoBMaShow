#pragma once

#include "RealtimeTouchInputRouter.h"
#include "../../input/SDLPointerEvent.h"
#include <SDL_events.h>

namespace gameplay {

// Convert without consulting mutable rendering globals on the SDL producer.
inline std::optional<RealtimeTouchSample> realtimeTouchSampleFromSdl(
    const SDL_Event &event, std::int64_t timestampMicros,
    const RealtimeTouchUiTransform &transform) {
  RealtimeTouchSample sample{.steadyTimestampMicros = timestampMicros};
  switch (event.type) {
  case SDL_FINGERDOWN: case SDL_FINGERUP: case SDL_FINGERMOTION:
    sample.fingerId = event.tfinger.fingerId;
    sample.normalizedX = event.tfinger.x;
    sample.normalizedY = event.tfinger.y;
    sample.phase = event.type == SDL_FINGERDOWN ? RealtimeTouchPhase::Down
        : event.type == SDL_FINGERUP ? RealtimeTouchPhase::Up : RealtimeTouchPhase::Move;
    break;
  case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: case SDL_MOUSEMOTION: {
    if (transform.renderWidth <= 0 || transform.renderHeight <= 0) return std::nullopt;
    const bool motion = event.type == SDL_MOUSEMOTION;
    sample.fingerId = sdl_pointer_event::kMouseFingerId;
    sample.normalizedX = (motion ? event.motion.x : event.button.x) *
        transform.inputScaleX / transform.renderWidth;
    sample.normalizedY = (motion ? event.motion.y : event.button.y) *
        transform.inputScaleY / transform.renderHeight;
    sample.phase = motion ? RealtimeTouchPhase::Move
        : event.type == SDL_MOUSEBUTTONDOWN ? RealtimeTouchPhase::Down : RealtimeTouchPhase::Up;
    break;
  }
  default:
    return std::nullopt;
  }
  return sample;
}

} // namespace gameplay
