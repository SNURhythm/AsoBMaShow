#pragma once

#include "../targets.h"
#include <cstdint>
#include <functional>
#include <optional>
#include <type_traits>
#include <utility>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_error.h>
#include <exception>
#include <stdexcept>

#if TARGET_OS_IPHONE
#include <SDL3/SDL.h>
#include "../ThreadCompat.h"
#include "IOSApplicationLifecycle.h"
#include <string>

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
platform::IOSPresentationState GetIOSPresentationState();
std::optional<IOSWindowSnapshot> GetIOSWindowSnapshot(SDL_Window *window);
std::uint64_t GetIOSCompletedPumpLifecycleGeneration();
void ResumeIOSGameplayTouchInput(std::uint64_t expectedGeneration);
bool TakeIOSApplicationOverflow();
void SetIOSApplicationEventDiscardHandler(std::function<void(const SDL_Event &)> handler);
void PostIOSApplicationWork(std::function<void()> work);
bool PollIOSApplicationWork();
void BeginIOSReplayExport(std::stop_source stop);
void UpdateIOSReplayExport(double fraction, const std::string &message);
void EndIOSReplayExport();
#endif

namespace platform {
inline bool isMainThread() {
#if TARGET_OS_IPHONE
  return IsIOSMainThread();
#else
  return SDL_IsMainThread();
#endif
}
// Operations must not capture scene pointers into asynchronous work. This
// synchronous direction is worker -> main only; main never waits for worker.
inline void runOnMain(std::function<void()> operation) {
#if TARGET_OS_IPHONE
  RunIOSMainThread(std::move(operation));
#else
  if (SDL_IsMainThread()) { operation(); return; }
  struct Invocation {
    std::function<void()> &operation;
    std::exception_ptr failure;
  } invocation{operation, {}};
  const auto invoke = [](void *opaque) {
    auto &call = *static_cast<Invocation *>(opaque);
    try { call.operation(); } catch (...) { call.failure = std::current_exception(); }
  };
  if (!SDL_RunOnMainThread(invoke, &invocation, true))
    throw std::runtime_error(SDL_GetError());
  if (invocation.failure) std::rethrow_exception(invocation.failure);
#endif
}

template <typename F> auto onMain(F &&operation) -> std::invoke_result_t<F> {
  using Result = std::invoke_result_t<F>;
  if constexpr (std::is_void_v<Result>) {
    runOnMain([&] { operation(); });
  } else {
    static_assert(!std::is_reference_v<Result>, "Return owned state across the main-thread boundary");
    std::optional<Result> result;
    runOnMain([&] { result.emplace(operation()); });
    return std::move(*result);
  }
}
}
