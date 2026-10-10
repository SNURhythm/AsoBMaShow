#include "IOSApplicationRuntime.h"
#if TARGET_OS_IPHONE
#include "ApplicationEventQueue.h"
#include "../iOSNatives.hpp"
#include "../input/IOSTouchInput.h"
#include "../input/InputLifecycle.h"

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>

namespace {
struct Runtime {
  platform::ApplicationEventQueue events;
  std::atomic_bool active{true};
  std::atomic_bool complete{false};
  std::mutex viewportMutex;
  IOSWindowSnapshot viewport;
  std::mutex wakeMutex;
  std::condition_variable wake;
  int result = EXIT_FAILURE;
};
std::mutex runtimeMutex;
std::shared_ptr<Runtime> runtime;
std::shared_ptr<Runtime> currentRuntime() {
  const std::lock_guard lock(runtimeMutex);
  return runtime;
}
void publishRuntime(std::shared_ptr<Runtime> state) {
  const std::lock_guard lock(runtimeMutex);
  runtime = std::move(state);
}
// Poll callers retain the returned payload until their next poll, just as SDL
// does on its pumping thread. No native pointer escapes the producer's pump.
thread_local platform::OwnedApplicationEvent currentEvent;

void updateViewport(Runtime &state, SDL_Window *window) {
  IOSWindowSnapshot next;
  next.window = window;
  next.id = SDL_GetWindowID(window);
  SDL_GetWindowSize(window, &next.width, &next.height);
  SDL_GetWindowSizeInPixels(window, &next.pixelWidth, &next.pixelHeight);
  if (const auto mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window))) {
    next.refreshRate = mode->refresh_rate;
  }
  const std::lock_guard lock(state.viewportMutex);
  next.generation = state.viewport.generation + 1;
  state.viewport = next;
}
}

bool IsIOSMainThread() { return [NSThread isMainThread]; }

void RunIOSMainThread(std::function<void()> operation) {
  if ([NSThread isMainThread]) {
    operation();
    return;
  }
  // Never propagate a C++ exception through dispatch's Objective-C frame.
  __block std::exception_ptr failure;
  dispatch_sync(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      try { operation(); } catch (...) { failure = std::current_exception(); }
    }
  });
  if (failure) std::rethrow_exception(failure);
}

int RunIOSApplication(SDL_Window *window, std::function<int()> application) {
  NSCAssert([NSThread isMainThread], @"Application bootstrap must own UIKit");
  auto state = std::make_shared<Runtime>();
  updateViewport(*state, window);
  publishRuntime(state);
  std::thread worker;
  try {
    worker = std::thread([state, application = std::move(application)] {
      @autoreleasepool {
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        try {
          state->result = application();
        } catch (const std::exception &error) {
          SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "iOS application worker failed: %s", error.what());
        } catch (...) {
          SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "iOS application worker failed");
        }
      }
      state->complete.store(true, std::memory_order_release);
    });
  } catch (...) {
    publishRuntime(nullptr);
    throw;
  }
  while (!state->complete.load(std::memory_order_acquire)) {
    @autoreleasepool {
      SDL_Event event{};
      while (SDL_PollEvent(&event)) {
        if (input::isBackgroundLifecycleEvent(event) || event.type == SDL_EVENT_TERMINATING ||
            event.type == SDL_EVENT_QUIT) {
          state->active.store(false, std::memory_order_release);
          SetIOSGameplayTouchInputEnabled(false);
        } else if (input::isForegroundLifecycleEvent(event)) {
          updateViewport(*state, window);
          state->active.store(true, std::memory_order_release);
          // The application acknowledges viewport restoration before ingress
          // reopens. Main never waits for that acknowledgement.
        }
        if (!state->events.push(event)) SetIOSGameplayTouchInputEnabled(false);
        state->wake.notify_one();
      }
      updateViewport(*state, window);
      WaitIOSMainRunLoopForMicros(1000);
    }
  }
  SetIOSGameplayTouchInputEnabled(false);
  worker.join(); // Completion published only after all worker cleanup.
  publishRuntime(nullptr);
  return state->result;
}

bool PollIOSApplicationEvent(SDL_Event *event) {
  auto state = currentRuntime();
  if (!state) return SDL_PollEvent(event);
  if (!state->events.poll(currentEvent)) return false;
  if (event) *event = currentEvent.event();
  return true;
}

bool WaitIOSApplicationEvent(SDL_Event *event, int timeoutMs) {
  auto state = currentRuntime();
  if (!state) return SDL_WaitEventTimeout(event, timeoutMs);
  if (PollIOSApplicationEvent(event)) return true;
  std::unique_lock lock(state->wakeMutex);
  // Main never locks wakeMutex. A missed notification can delay at most one
  // short slice, while retaining background audio and shutdown responsiveness.
  state->wake.wait_for(lock, std::chrono::milliseconds(std::min(timeoutMs, 16)));
  return PollIOSApplicationEvent(event);
}

bool IOSApplicationActive() {
  const auto state = currentRuntime();
  return !state || state->active.load(std::memory_order_acquire);
}

std::optional<IOSWindowSnapshot> GetIOSWindowSnapshot(SDL_Window *window) {
  const auto state = currentRuntime();
  if (!state) return std::nullopt;
  const std::lock_guard lock(state->viewportMutex);
  if (window != state->viewport.window) return std::nullopt;
  return state->viewport;
}

void ResumeIOSGameplayTouchInput() {
  RunIOSMainThread([] {
    if (IOSApplicationActive()) SetIOSGameplayTouchInputEnabled(true);
  });
}
#endif
