#pragma once

#include "RealtimeTouchInputRouter.h"
#include "../../input/SDLPointerEvent.h"
#include <SDL3/SDL_events.h>

namespace gameplay {

// Convert without consulting mutable rendering globals on the SDL producer.
inline std::optional<RealtimeTouchSample> realtimeTouchSampleFromSdl(
    const SDL_Event &event, std::int64_t timestampMicros,
    const RealtimeTouchUiTransform &transform) {
  RealtimeTouchSample sample{.steadyTimestampMicros = timestampMicros};
  switch (event.type) {
  case SDL_EVENT_FINGER_DOWN: case SDL_EVENT_FINGER_UP: case SDL_EVENT_FINGER_MOTION:
  case SDL_EVENT_FINGER_CANCELED:
    sample.fingerId = event.tfinger.fingerID;
    sample.normalizedX = event.tfinger.x;
    sample.normalizedY = event.tfinger.y;
    sample.phase = event.type == SDL_EVENT_FINGER_DOWN ? RealtimeTouchPhase::Down
        : event.type == SDL_EVENT_FINGER_UP ? RealtimeTouchPhase::Up
        : event.type == SDL_EVENT_FINGER_CANCELED ? RealtimeTouchPhase::Cancel : RealtimeTouchPhase::Move;
    break;
  case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: case SDL_EVENT_MOUSE_MOTION: {
    if (transform.renderWidth <= 0 || transform.renderHeight <= 0) return std::nullopt;
    const bool motion = event.type == SDL_EVENT_MOUSE_MOTION;
    sample.fingerId = sdl_pointer_event::kMouseFingerId;
    sample.normalizedX = (motion ? event.motion.x : event.button.x) *
        transform.inputScaleX / transform.renderWidth;
    sample.normalizedY = (motion ? event.motion.y : event.button.y) *
        transform.inputScaleY / transform.renderHeight;
    sample.phase = motion ? RealtimeTouchPhase::Move
        : event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? RealtimeTouchPhase::Down : RealtimeTouchPhase::Up;
    break;
  }
  default:
    return std::nullopt;
  }
  return sample;
}

} // namespace gameplay
