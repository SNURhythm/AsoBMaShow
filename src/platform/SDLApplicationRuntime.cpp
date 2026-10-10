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
#ifndef NDEBUG
  std::atomic_uint64_t mainServiceCount{0};
  unsigned ownerIterations = 0;
  bool stallProbeRan = false;
#endif
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
#ifndef NDEBUG
  const bool traceEvents = std::getenv("ASOBMASHOW_EVENT_TRACE") != nullptr;
#endif
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
#ifndef NDEBUG
      if (traceEvents && (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                          event.type == SDL_EVENT_MOUSE_BUTTON_UP))
        SDL_Log("SDL pointer event %u: %.1f, %.1f", event.type, event.button.x, event.button.y);
      if (traceEvents && (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP ||
                          event.type == SDL_EVENT_TEXT_INPUT))
        SDL_Log("SDL keyboard event %u", event.type);
#endif
      state->events.push(event);
      state->wake.notify_one();
    }
    updateViewport(*state, window);
#ifndef NDEBUG
    state->mainServiceCount.fetch_add(1, std::memory_order_relaxed);
#endif
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

void pollApplicationDiagnostics() {
#ifndef NDEBUG
  const auto state = currentRuntime();
  if (!state || state->stallProbeRan || ++state->ownerIterations < 120) return;
  state->stallProbeRan = true;
  if (const char *value = std::getenv("ASOBMASHOW_RENDER_STALL_MS")) {
    const int milliseconds = std::clamp(std::atoi(value), 0, 5000);
    const auto before = state->mainServiceCount.load(std::memory_order_relaxed);
    SDL_Log("Render stall probe begin: %d ms", milliseconds);
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
    SDL_Log("Render stall probe end: SDL main serviced %llu iterations",
        static_cast<unsigned long long>(state->mainServiceCount.load(std::memory_order_relaxed) - before));
    if (std::getenv("ASOBMASHOW_PROBE_QUIT")) {
      SDL_Event quit{}; quit.type = SDL_EVENT_QUIT; SDL_PushEvent(&quit);
    }
  }
#endif
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
