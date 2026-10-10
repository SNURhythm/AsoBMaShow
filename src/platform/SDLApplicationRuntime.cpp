#include "SDLApplicationRuntime.h"
#if !TARGET_OS_IPHONE
#include "ApplicationEventQueue.h"
#include "ApplicationThreadHost.h"
#include "../input/InputLifecycle.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace platform {
namespace {
struct Runtime {
  ApplicationEventQueue events;
  std::atomic_bool active{true};
  std::mutex viewportMutex;
  WindowSnapshot viewport;
  std::mutex wakeMutex;
  std::condition_variable wake;
};
std::mutex runtimeMutex;
std::shared_ptr<Runtime> runtime;
thread_local OwnedApplicationEvent currentEvent;

std::shared_ptr<Runtime> currentRuntime() {
  const std::lock_guard lock(runtimeMutex);
  return runtime;
}
void publishRuntime(std::shared_ptr<Runtime> state) {
  const std::lock_guard lock(runtimeMutex);
  runtime = std::move(state);
}
void updateViewport(Runtime &state, SDL_Window *window) {
  WindowSnapshot next;
  next.window = window;
  next.id = SDL_GetWindowID(window);
  SDL_GetWindowSize(window, &next.width, &next.height);
  SDL_GetWindowSizeInPixels(window, &next.pixelWidth, &next.pixelHeight);
  if (const auto *mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window)))
    next.refreshRate = mode->refresh_rate;
  const std::lock_guard lock(state.viewportMutex);
  next.generation = state.viewport.generation + 1;
  state.viewport = next;
}
bool SDLCALL lifecycleWatch(void *opaque, SDL_Event *event) {
  auto &state = *static_cast<Runtime *>(opaque);
  if (input::isBackgroundLifecycleEvent(*event) || event->type == SDL_EVENT_QUIT ||
      event->type == SDL_EVENT_TERMINATING)
    state.active.store(false, std::memory_order_release);
  else if (input::isForegroundLifecycleEvent(*event))
    state.active.store(true, std::memory_order_release);
  return true;
}
}

int runSDLApplication(SDL_Window *window, std::function<int()> application) {
  if (!SDL_IsMainThread()) throw std::logic_error("SDL bootstrap must own event pumping");
  auto state = std::make_shared<Runtime>();
  updateViewport(*state, window);
  if (!SDL_AddEventWatch(lifecycleWatch, state.get())) return EXIT_FAILURE;
  publishRuntime(state);
  struct Cleanup {
    Runtime *state;
    ~Cleanup() {
      SDL_RemoveEventWatch(lifecycleWatch, state);
      publishRuntime(nullptr);
    }
  } cleanup{state.get()};
  return runApplicationThread(std::move(application), [&] {
    SDL_Event event{};
    while (SDL_PollEvent(&event)) {
      state->events.push(event);
      state->wake.notify_one();
    }
    updateViewport(*state, window);
    // SDL_PumpEvents services synchronous worker requests as well as native
    // events. Never wait for rendering or take a scene-owned lock here.
    SDL_Delay(1);
  }, [](std::exception_ptr failure) {
    try { std::rethrow_exception(failure); }
    catch (const std::exception &error) {
      SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Application worker failed: %s", error.what());
    } catch (...) {
      SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Application worker failed");
    }
  });
}

bool pollApplicationEvent(SDL_Event *event) {
  const auto state = currentRuntime();
  if (!state) return SDL_PollEvent(event);
  if (!state->events.poll(currentEvent)) return false;
  if (event) *event = currentEvent.event();
  return true;
}

bool waitApplicationEvent(SDL_Event *event, int timeoutMs) {
  const auto state = currentRuntime();
  if (!state) return SDL_WaitEventTimeout(event, timeoutMs);
  if (pollApplicationEvent(event)) return true;
  std::unique_lock lock(state->wakeMutex);
  // Bound a missed notification and background shutdown latency.
  state->wake.wait_for(lock, std::chrono::milliseconds(std::clamp(timeoutMs, 0, 16)));
  return pollApplicationEvent(event);
}

bool applicationActive() {
  const auto state = currentRuntime();
  return !state || state->active.load(std::memory_order_acquire);
}

bool takeApplicationOverflow() {
  const auto state = currentRuntime();
  return state && state->events.takeOverflow();
}

std::optional<WindowSnapshot> getWindowSnapshot(SDL_Window *window) {
  const auto state = currentRuntime();
  if (!state) return std::nullopt;
  const std::lock_guard lock(state->viewportMutex);
  if (window != state->viewport.window) return std::nullopt;
  return state->viewport;
}
}
#endif
