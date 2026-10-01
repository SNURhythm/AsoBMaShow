#include "input/NativeKeyboardInputState.h"
#include "input/RealtimeControllerDeviceMap.h"
#include <stdexcept>
#include <vector>

void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}

int main() {
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
  state.setEnabled(true, 200);
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
  state.setEnabled(false, 456789);
  require(events.size() == 4 && events.back().timestampMicros == 456789 &&
              events.back().control.index == SDL_SCANCODE_LSHIFT &&
              events.back().normalizedValue == 0.0F,
          "focus/claim loss releases held native input immediately");
  state.consume(0, SDL_SCANCODE_A, true, 500000);
  require(events.size() == 4, "disabled ingress does not retain global keystrokes");
  state.setEnabled(true, 600000);
  state.consume(0, SDL_SCANCODE_LSHIFT, true, 600001);
  require(events.size() == 5, "new ownership starts without stale held state");
}
