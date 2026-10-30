#pragma once

#include "../ThreadCompat.h"
#include "../input/InputLifecycle.h"
#include <cstdint>
#include <mutex>
#include <optional>

namespace platform {
struct IOSPresentationState {
  bool active = true;
  std::uint64_t generation = 0;
};

// SDL maps UIKit resignActive (including permission alerts) to WILL_BACKGROUND.
// Only DID_BACKGROUND and termination cancel owner work. Presentation stops at
// the earlier notification and resumes only after UIKit becomes active again.
class IOSApplicationLifecycle {
public:
  void observe(const SDL_Event &event) {
    std::optional<std::stop_source> cancel;
    {
      const std::lock_guard lock(mutex);
      if (event.type == SDL_EVENT_TERMINATING || event.type == SDL_EVENT_QUIT)
        terminating = true;
      if (event.type == SDL_EVENT_WILL_ENTER_BACKGROUND ||
          event.type == SDL_EVENT_DID_ENTER_BACKGROUND) interrupted = true;
      if (event.type == SDL_EVENT_DID_ENTER_BACKGROUND) background = true;
      if (event.type == SDL_EVENT_DID_ENTER_FOREGROUND) {
        interrupted = false;
        background = false;
      }
      if (input::isBackgroundLifecycleEvent(event) || terminating) {
        if (state.active) ++state.generation;
        state.active = false;
      } else if (!interrupted && !background && !terminating &&
                 event.type != SDL_EVENT_WILL_ENTER_FOREGROUND &&
                 input::isForegroundLifecycleEvent(event)) {
        state.active = true;
      }
      if (background || terminating) cancel = exportStop;
    }
    // Stop callbacks may call into native code; never run them under our lock.
    if (cancel) cancel->request_stop();
  }
  IOSPresentationState presentation() const {
    const std::lock_guard lock(mutex);
    return state;
  }
  void beginExport(std::stop_source stop) {
    bool cancel;
    {
      const std::lock_guard lock(mutex);
      exportStop = stop;
      cancel = background || terminating;
    }
    if (cancel) stop.request_stop();
  }
  void endExport() {
    const std::lock_guard lock(mutex);
    exportStop.reset();
  }
  bool exporting() const {
    const std::lock_guard lock(mutex);
    return exportStop.has_value();
  }
private:
  mutable std::mutex mutex;
  IOSPresentationState state;
  bool interrupted = false, background = false, terminating = false;
  std::optional<std::stop_source> exportStop;
};

// bgfx::discard() only resets encoder state. Submitted draws require
// frame(BGFX_FRAME_DISCARD), which still runs Metal and must wait for activity.
// Keep the old frame isolated until it is retired, before new owner work.
class IOSPreparedFrame {
public:
  void begin(IOSPresentationState state) {
    generation = state.generation;
    beganActive = state.active;
  }
  bool pending() const { return prepared; }
  template <typename Submit> bool retire(IOSPresentationState state, Submit submit) {
    if (!prepared) return true;
    if (!state.active) return false;
    submit(true);
    prepared = false;
    return true;
  }
  template <typename Submit> bool finish(IOSPresentationState state, Submit submit) {
    prepared = true;
    if (!state.active) return false;
    const bool discard = !beganActive || generation != state.generation;
    submit(discard);
    prepared = false;
    return !discard;
  }
private:
  std::uint64_t generation = 0;
  bool prepared = false, beganActive = false;
};
}
