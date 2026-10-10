#pragma once

#include "../targets.h"
#include <cstdint>
#include <functional>
#include <optional>
#include <type_traits>
#include <utility>

#if TARGET_OS_IPHONE
#include <SDL3/SDL.h>

struct IOSWindowSnapshot {
  SDL_Window *window = nullptr;
  SDL_WindowID id = 0;
  int width = 0, height = 0;
  int pixelWidth = 0, pixelHeight = 0;
  float refreshRate = 0;
  std::uint64_t generation = 0;
};

// Main owns the native window throughout this call. It services UIKit and SDL
// until the application worker has completed cleanup, then joins that worker.
int RunIOSApplication(SDL_Window *window, std::function<int()> application);
void RunIOSMainThread(std::function<void()> operation);
bool IsIOSMainThread();
bool PollIOSApplicationEvent(SDL_Event *event);
bool WaitIOSApplicationEvent(SDL_Event *event, int timeoutMs);
bool IOSApplicationActive();
std::optional<IOSWindowSnapshot> GetIOSWindowSnapshot(SDL_Window *window);
void ResumeIOSGameplayTouchInput();
#endif

namespace platform {
inline bool isMainThread() {
#if TARGET_OS_IPHONE
  return IsIOSMainThread();
#else
  return true;
#endif
}
// Operations must not capture scene pointers into asynchronous work. This
// synchronous direction is worker -> main only; main never waits for worker.
template <typename F> auto onMain(F &&operation) -> std::invoke_result_t<F> {
#if TARGET_OS_IPHONE
  using Result = std::invoke_result_t<F>;
  if constexpr (std::is_void_v<Result>) {
    RunIOSMainThread([&] { operation(); });
  } else {
    static_assert(!std::is_reference_v<Result>, "Return owned state across the main-thread boundary");
    std::optional<Result> result;
    RunIOSMainThread([&] { result.emplace(operation()); });
    return std::move(*result);
  }
#else
  return operation();
#endif
}
}
