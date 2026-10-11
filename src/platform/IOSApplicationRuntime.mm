#include "IOSApplicationRuntime.h"
#if TARGET_OS_IPHONE
#include "ApplicationEventQueue.h"
#include "ApplicationThreadHost.h"
#include "../RAII.h"
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
#include <deque>

namespace {
struct Runtime {
  platform::ApplicationEventQueue events;
  std::function<void(const SDL_Event &)> discardEvent; // UIKit main only.
  bool inputSuppressed = false, finishInputSuppression = false; // UIKit main only.
  platform::IOSApplicationLifecycle lifecycle;
  std::atomic_bool ingressPaused{false};
  std::atomic_uint64_t completedPumpLifecycleGeneration{0};
#ifndef NDEBUG
  std::atomic_uint64_t mainServiceCount{0};
  unsigned ownerIterations = 0;
  bool stallProbeRan = false;
#endif
  std::mutex viewportMutex;
  IOSWindowSnapshot viewport;
  std::mutex wakeMutex;
  std::condition_variable wake;
  // Work is posted and consumed only by the application owner.
  std::deque<std::function<void()>> work;
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

bool SDLCALL lifecycleWatch(void *opaque, SDL_Event *event) {
  auto &state = *static_cast<Runtime *>(opaque);
  state.lifecycle.observe(*event);
  return true;
}

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
  if (!SDL_AddEventWatch(lifecycleWatch, state.get())) return EXIT_FAILURE;
  publishRuntime(state);
  auto cleanup = makeScopeExit([&] {
    SDL_RemoveEventWatch(lifecycleWatch, state.get());
    SetIOSGameplayTouchInputEnabled(false);
    publishRuntime(nullptr);
  });
  return platform::runApplicationThread([application = std::move(application)] {
    @autoreleasepool {
      pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
      pthread_setname_np("AsoBMaShow application/render");
      return application();
    }
  }, [&] {
    @autoreleasepool {
      // Capture before polling: a lifecycle watch can advance during the pump,
      // before its queued events and viewport have reached the owner.
      const auto pumpGeneration = state->lifecycle.presentation().generation;
      state->events.beginProducerBatch();
      SDL_Event event{};
      while (SDL_PollEvent(&event)) {
        if (input::isBackgroundLifecycleEvent(event) || event.type == SDL_EVENT_TERMINATING ||
            event.type == SDL_EVENT_QUIT) {
          state->ingressPaused.store(true, std::memory_order_release);
          SetIOSGameplayTouchInputEnabled(false);
        } else if (input::isForegroundLifecycleEvent(event)) {
          updateViewport(*state, window);
          // The application acknowledges viewport restoration before ingress
          // reopens. Main never waits for that acknowledgement.
        }
        // Publish geometry before the application can consume its resize event.
        if ((event.type >= SDL_EVENT_WINDOW_FIRST && event.type <= SDL_EVENT_WINDOW_LAST) ||
            (event.type >= SDL_EVENT_DISPLAY_FIRST && event.type <= SDL_EVENT_DISPLAY_LAST))
          updateViewport(*state, window);
        if (!state->events.push(event, state->inputSuppressed, state->discardEvent)) {
          state->ingressPaused.store(true, std::memory_order_release);
          SetIOSGameplayTouchInputEnabled(false);
        }
        state->wake.notify_one();
      }
      updateViewport(*state, window);
      state->events.endProducerBatch();
      if (state->finishInputSuppression) {
        state->inputSuppressed = false;
        state->finishInputSuppression = false;
      }
      state->completedPumpLifecycleGeneration.store(pumpGeneration, std::memory_order_release);
#ifndef NDEBUG
      state->mainServiceCount.fetch_add(1, std::memory_order_relaxed);
#endif
      WaitIOSMainRunLoopForMicros(1000);
    }
  }, [](std::exception_ptr failure) {
    try { std::rethrow_exception(failure); }
    catch (const std::exception &error) {
      SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "iOS application worker failed: %s", error.what());
    } catch (...) {
      SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "iOS application worker failed");
    }
  });
}

void SetIOSApplicationEventDiscardHandler(std::function<void(const SDL_Event &)> handler) {
  RunIOSMainThread([handler = std::move(handler)]() mutable {
    if (const auto state = currentRuntime()) state->discardEvent = std::move(handler);
  });
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
  return GetIOSPresentationState().active;
}

platform::IOSPresentationState GetIOSPresentationState() {
  const auto state = currentRuntime();
  return state ? state->lifecycle.presentation() : platform::IOSPresentationState{};
}

std::optional<IOSWindowSnapshot> GetIOSWindowSnapshot(SDL_Window *window) {
  const auto state = currentRuntime();
  if (!state) return std::nullopt;
  const std::lock_guard lock(state->viewportMutex);
  if (window != state->viewport.window) return std::nullopt;
  return state->viewport;
}

std::uint64_t GetIOSCompletedPumpLifecycleGeneration() {
  const auto state = currentRuntime();
  return state ? state->completedPumpLifecycleGeneration.load(std::memory_order_acquire) : 0;
}

void ResumeIOSGameplayTouchInput(std::uint64_t expectedGeneration) {
  const auto state = currentRuntime();
  if (!state || !state->ingressPaused.load(std::memory_order_acquire)) return;
  RunIOSMainThread([expectedGeneration] {
    const auto state = currentRuntime();
    if (!state) return;
    const auto presentation = state->lifecycle.presentation();
    if (presentation.active && presentation.generation == expectedGeneration &&
        !state->lifecycle.exporting() && !state->inputSuppressed) {
      SetIOSGameplayTouchInputEnabled(true);
      state->ingressPaused.store(false, std::memory_order_release);
    }
  });
}
bool TakeIOSApplicationOverflow() {
  const auto state = currentRuntime();
  return state && state->events.takeOverflow();
}

void PostIOSApplicationWork(std::function<void()> work) {
  const auto state = currentRuntime();
  if (!state || IsIOSMainThread()) throw std::logic_error("Missing iOS application owner");
  state->work.push_back(std::move(work));
}

bool PollIOSApplicationWork() {
  const auto state = currentRuntime();
  if (!state) return false;
#ifndef NDEBUG
  // Opt-in acceptance probe, bounded and absent from release builds. Main's
  // service count measures scheduling independence, not hardware input latency.
  ++state->ownerIterations;
  const char *afterTouches = std::getenv("ASOBMASHOW_IOS_RENDER_STALL_AFTER_TOUCHES");
  const bool probeDue = afterTouches
      ? IOSGameplayTouchProbeCount() >= static_cast<unsigned>(std::max(1, std::atoi(afterTouches)))
      : state->ownerIterations >= 120;
  if (!state->stallProbeRan && probeDue) {
    state->stallProbeRan = true;
    if (const char *value = std::getenv("ASOBMASHOW_IOS_RENDER_STALL_MS")) {
      const int milliseconds = std::clamp(std::atoi(value), 0, 5000);
      const auto before = state->mainServiceCount.load(std::memory_order_relaxed);
      SDL_Log("iOS render stall probe begin: %d ms", milliseconds);
      std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
      SDL_Log("iOS render stall probe end: main serviced %llu iterations",
          static_cast<unsigned long long>(state->mainServiceCount.load(std::memory_order_relaxed) - before));
      if (std::getenv("ASOBMASHOW_IOS_PROBE_QUIT")) {
        SDL_Event quit{};
        quit.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&quit);
      }
    }
  }
#endif
  if (state->work.empty()) return false;
  // One job per safe scene-loop boundary. Destruction invalidates pending jobs.
  auto work = std::move(state->work.front());
  state->work.pop_front();
  work();
  return true;
}

void BeginIOSReplayExport(std::stop_source stop) {
  RunIOSMainThread([stop] {
    const auto state = currentRuntime();
    if (!state) return;
    state->lifecycle.beginExport(stop);
    state->inputSuppressed = true;
    state->finishInputSuppression = false;
    state->events.discardUserInput(state->discardEvent);
    state->ingressPaused.store(true, std::memory_order_release);
    SetIOSGameplayTouchInputEnabled(false);
    ShowIOSReplayExportProgress([stop]() mutable { stop.request_stop(); });
  });
}

void UpdateIOSReplayExport(double fraction, const std::string &message) {
  RunIOSMainThread([&] { SetIOSReplayExportProgress(fraction, message); });
}

void EndIOSReplayExport() {
  RunIOSMainThread([] {
    HideIOSReplayExportProgress();
    const auto state = currentRuntime();
    if (state) {
      state->lifecycle.endExport();
      // Resume admission only after the SDL batch containing this request has
      // drained; remaining events still belong to the export interval.
      state->finishInputSuppression = true;
    }
  });
  // Reopen input only after the scene loop has consumed pending lifecycle state.
}
#endif
