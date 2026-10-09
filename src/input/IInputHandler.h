//
// Created by XF on 9/4/2024.
//

#pragma once
#include "InputNormalizer.h"
#include <cstdint>
#include "../math/Vector3.h"
class IInputHandler {
public:
  // Steady-clock ingress time is scoped to this callback, including nested dispatch.
  void dispatchFingerAt(Uint32 phase, SDL_FingerID finger, Vector3 location,
                        std::uint64_t timestampMicros) {
    struct Restore {
      std::uint64_t &value;
      std::uint64_t previous;
      ~Restore() { value = previous; }
    } restore{touchTimestampMicros_, touchTimestampMicros_};
    touchTimestampMicros_ = timestampMicros;
    if (phase == SDL_EVENT_FINGER_DOWN) onFingerDown(finger, location);
    else if (phase == SDL_EVENT_FINGER_UP) onFingerUp(finger, location);
    else if (phase == SDL_EVENT_FINGER_CANCELED) onFingerCancel(finger, location);
    else if (phase == SDL_EVENT_FINGER_MOTION) onFingerMove(finger, location);
  }
  std::uint64_t touchEventTimestampMicros() const { return touchTimestampMicros_; }
  virtual ~IInputHandler() = default;
  virtual void onKeyDown(int keyCode, KeySource keySource) = 0;
  virtual void onKeyUp(int keyCode, KeySource keySource) = 0;
  virtual void onFingerDown(SDL_FingerID fingerIndex, Vector3 location) = 0;
  virtual void onFingerUp(SDL_FingerID fingerIndex, Vector3 location) = 0;
  virtual void onFingerCancel(SDL_FingerID fingerIndex, Vector3 location) {
    onFingerUp(fingerIndex, location);
  }
  virtual void onFingerMove(SDL_FingerID fingerIndex, Vector3 location) = 0;
private:
  std::uint64_t touchTimestampMicros_ = 0;
};
