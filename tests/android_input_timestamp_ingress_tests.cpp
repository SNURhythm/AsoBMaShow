#include "input/AndroidInputTimestamp.h"

#include <SDL3/SDL.h>
#include <array>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
struct Observed {
  std::array<Uint64, 3> timestamps{};
  std::array<std::thread::id, 3> threads{};
};

void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}

bool SDLCALL watch(void *context, SDL_Event *event) {
  if (event->type == SDL_EVENT_FINGER_DOWN &&
      event->tfinger.fingerID >= 1 && event->tfinger.fingerID <= 3) {
    auto &observed = *static_cast<Observed *>(context);
    const auto index = static_cast<std::size_t>(event->tfinger.fingerID - 1);
    observed.timestamps[index] = event->common.timestamp;
    observed.threads[index] = std::this_thread::get_id();
  }
  return true;
}

void push(SDL_FingerID finger, Uint64 timestamp) {
  SDL_Event event{};
  event.type = SDL_EVENT_FINGER_DOWN;
  event.tfinger.fingerID = finger;
  event.tfinger.timestamp = timestamp;
  require(SDL_PushEvent(&event), "SDL accepts the input sample");
}
} // namespace

int main() {
  try {
    require(SDL_Init(SDL_INIT_EVENTS), "SDL event subsystem initializes");
    Observed observed;
    SDL_SetEventFilter(&input::android::timestampFilter, nullptr);
    require(SDL_AddEventWatch(watch, &observed), "SDL event watch registers");
    std::thread::id nativeThread;
    std::thread producer([&] {
      nativeThread = std::this_thread::get_id();
      input::android::setInputTimestamp(92'000'123, 100'000'000, 50'000'000);
      push(1, 50'000'000);
      // A concurrent producer must not inherit the Java thread's timestamp.
      std::thread otherProducer([] { push(2, 51'000'000); });
      otherProducer.join();
      input::android::setInputTimestamp(0, 0, 0);
      push(3, 52'000'000);
    });
    producer.join();
    require(observed.timestamps == std::array<Uint64, 3>{42'000'123, 51'000'000, 52'000'000},
            "watchers see corrected native time before any render-thread pump, scoped per producer");
    require(observed.threads[0] == nativeThread && observed.threads[2] == nativeThread,
            "input watches execute synchronously on the native producer");
    std::array<Uint64, 3> queued{};
    SDL_Event event{};
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_EVENT_FINGER_DOWN) {
        queued[static_cast<std::size_t>(event.tfinger.fingerID - 1)] = event.common.timestamp;
      }
    }
    require(queued == observed.timestamps, "ordinary scene delivery retains the same native timestamps");
    SDL_RemoveEventWatch(watch, &observed);
    SDL_SetEventFilter(nullptr, nullptr);
    SDL_Quit();
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    SDL_Quit();
    return 1;
  }
}
