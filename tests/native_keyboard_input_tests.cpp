#include "input/NativeKeyboardInputState.h"
#include "input/RealtimeControllerDeviceMap.h"
#include <stdexcept>
#include <vector>

void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}

int main() {
  {
    std::vector<input::PhysicalInputEvent> events;
    NativeKeyboardInputState state([&](auto event) { events.push_back(event); });
    state.setClaimed(true, 100);
    state.setFocused(true, 300);
    state.consume(0, SDL_SCANCODE_S, true, 200);
    require(events.size() == 1, "an unchanged focus poll preserves queued first input after claim");
    state.setFocused(false, 400);
    require(events.size() == 2 && !events.back().normalizedValue &&
                events.back().timestampMicros == 400,
            "focus loss releases at the observed loss time, not the claim time");
    state.setFocused(true, 600);
    state.consume(0, SDL_SCANCODE_S, true, 500);
    require(events.size() == 2, "focus regain rejects input queued in another application");
    state.consume(0, SDL_SCANCODE_S, true, 700);
    require(events.size() == 3, "fresh input works after focus regain");
  }
  {
    std::vector<input::PhysicalInputEvent> events;
    NativeKeyboardInputState state([&](auto event) { events.push_back(event); });
    state.setClaimed(true, 100);
    state.setFocused(false, 300);
    // A per-process callback can arrive after the main-thread focus notice.
    state.setFocused(true, 200);
    state.consume(0, SDL_SCANCODE_S, true, 200);
    require(events.empty(), "a delayed pre-focus-loss callback cannot resurrect a held key");
    state.setFocused(true, 400);
    state.consume(0, SDL_SCANCODE_S, true, 400);
    require(events.size() == 1, "fresh per-process input can establish regained focus");
  }
  RealtimeControllerDeviceMap ownership;
  ownership.setKeyboardRealtimeAvailable(true);
  ownership.requestKeyboardRealtimeFallback();
  require(ownership.keyboardRealtimeAvailable(),
          "native failure must suppress queued SDL duplicates until main-thread drain");
  int drains = 0;
  ownership.completeKeyboardRealtimeFallback([&] {
    require(ownership.keyboardRealtimeAvailable(), "drain runs before SDL handover");
    ++drains;
  });
  require(drains == 1 && !ownership.keyboardRealtimeAvailable(),
          "SDL fallback starts only after stale keyboard events have been discarded");
  ownership.completeKeyboardRealtimeFallback([&] { ++drains; });
  require(drains == 1, "normal pumps do not discard fresh SDL keys");

  std::vector<input::PhysicalInputEvent> events;
  NativeKeyboardInputState state([&](auto event) { events.push_back(event); });
  state.consume(0, SDL_SCANCODE_S, true, 100);
  require(events.empty(), "inactive native ingress never captures UI keys");
  state.setClaimed(true, 200);
  state.consume(0, SDL_SCANCODE_S, true, 199);
  require(events.empty(), "native OS backlog from before claim activation is rejected");
  state.consume(0, SDL_SCANCODE_S, true, 123456);
  state.consume(0, SDL_SCANCODE_S, true, 123457);
  state.consume(1, SDL_SCANCODE_S, true, 123458);
  state.consume(0, SDL_SCANCODE_S, false, 123459);
  require(events.size() == 1 && events[0].timestampMicros == 123456 &&
              events[0].normalizedValue == 1.0F,
          "source timestamp survives and held second keyboard prevents release");
  state.disconnect(1, 234567);
  require(events.size() == 2 && events[1].timestampMicros == 234567 &&
              events[1].normalizedValue == 0.0F,
          "disconnect releases the last physical owner");
  state.consume(0, SDL_SCANCODE_LSHIFT, true, 345678);
  state.setClaimed(false, 456789);
  require(events.size() == 4 && events.back().timestampMicros == 456789 &&
              events.back().control.index == SDL_SCANCODE_LSHIFT &&
              events.back().normalizedValue == 0.0F,
          "focus/claim loss releases held native input immediately");
  state.consume(0, SDL_SCANCODE_A, true, 500000);
  require(events.size() == 4, "disabled ingress does not retain global keystrokes");
  state.setClaimed(true, 600000);
  state.consume(0, SDL_SCANCODE_LSHIFT, true, 600001);
  require(events.size() == 5, "new ownership starts without stale held state");
}
