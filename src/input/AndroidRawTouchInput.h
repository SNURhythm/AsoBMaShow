#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <SDL3/SDL_events.h>

namespace input::android {

using TouchEpoch = std::uint64_t;
inline constexpr std::size_t kUiTouchQueueCapacity = 4096;
inline constexpr std::size_t kMaximumUiTouchContacts = 64;

enum class TouchPhase { Down, Move, Up, Cancel };

struct RawTouchEvent {
  int pointerId = 0;
  TouchPhase phase = TouchPhase::Move;
  float x = 0, y = 0;
  std::int64_t steadyTimestampMicros = 0;
};

struct UiTouchEvent {
  TouchEpoch epoch = 0;
  RawTouchEvent touch;
};

struct UiCancellationBatch {
  std::array<RawTouchEvent, kMaximumUiTouchContacts> events{};
  std::size_t size = 0;
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
  [[nodiscard]] TouchEpoch epoch() const noexcept { return epoch_; }
  [[nodiscard]] static TouchEpoch activeEpoch();
  [[nodiscard]] static bool isCurrentEpoch(TouchEpoch epoch);
  static void dispatch(TouchEpoch epoch, const RawTouchEvent &event);
  static void dispatch(const RawTouchEvent &event);
  // Main-thread polling returns values only. Dispatch them after this returns,
  // never under the registration lock, and recheck epoch before each callback.
  static bool pollUiEvent(UiTouchEvent &event);
  [[nodiscard]] UiCancellationBatch cancelUiTouches();

private:
  struct UiState;
  std::unique_ptr<UiState> ui_;
  TouchEpoch epoch_ = 0;
  Callback callback_;
  void *context_;
};

inline bool isSdlFingerEvent(Uint32 type) noexcept {
  return type == SDL_EVENT_FINGER_DOWN || type == SDL_EVENT_FINGER_UP ||
         type == SDL_EVENT_FINGER_MOTION || type == SDL_EVENT_FINGER_CANCELED;
}

} // namespace input::android
