#pragma once

#include <cstdint>
#include <SDL3/SDL_events.h>

namespace input::android {

enum class TouchPhase { Down, Move, Up, Cancel };

struct RawTouchEvent {
  int pointerId = 0;
  TouchPhase phase = TouchPhase::Move;
  float x = 0, y = 0;
  std::int64_t steadyTimestampMicros = 0;
};

// A gameplay session owns this registration. Dispatch has no dependency on
// SDL's activity, touch, event-watch or event-queue locks. Detachment joins
// in-flight calls before the session's router and worker can be destroyed.
class RawTouchRegistration {
public:
  using Callback = void (*)(const RawTouchEvent &, void *);
  RawTouchRegistration(Callback callback, void *context);
  ~RawTouchRegistration();
  RawTouchRegistration(const RawTouchRegistration &) = delete;
  RawTouchRegistration &operator=(const RawTouchRegistration &) = delete;
  static void dispatch(const RawTouchEvent &event);

private:
  Callback callback_;
  void *context_;
};

inline bool isSdlFingerEvent(Uint32 type) noexcept {
  return type == SDL_EVENT_FINGER_DOWN || type == SDL_EVENT_FINGER_UP ||
         type == SDL_EVENT_FINGER_MOTION || type == SDL_EVENT_FINGER_CANCELED;
}

} // namespace input::android
