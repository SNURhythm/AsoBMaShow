#include "platform/SDLMainThread.h"
#include "platform/SDLApplicationRuntime.h"
#include "platform/ApplicationThreadHost.h"
#include <SDL3/SDL.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

std::atomic_bool interceptViewportRead{false}, viewportReadEntered{false}, releaseViewportRead{false};
constexpr int orderedViewportWidth = 853;
bool runtimeTestGetWindowSize(SDL_Window *window, int *width, int *height) {
  const bool result = SDL_GetWindowSize(window, width, height);
  if (result && *width == orderedViewportWidth && interceptViewportRead.exchange(false)) {
    viewportReadEntered = true;
    while (!releaseViewportRead.load()) SDL_Delay(1);
  }
  return result;
}
namespace {
void require(bool value, const char *message) {
  if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
bool failWithSdlError() { return SDL_SetError("main-thread diagnostic"); }
void testMainDispatch() {
  const auto main = std::this_thread::get_id();
  const int result = platform::runApplicationThread([&] {
    require(!platform::isMainThread(), "worker incorrectly claims SDL main ownership");
    require(platform::onMain([&] { return std::this_thread::get_id() == main; }),
            "platform operation did not run on SDL main");
    require(platform::onMain([] { return std::string("owned result"); }) == "owned result",
            "main return value lost");
    bool caught = false;
    try { platform::onMain([] { throw std::runtime_error("callback failure"); }); }
    catch (const std::runtime_error &error) { caught = std::string(error.what()) == "callback failure"; }
    require(caught, "main exception not propagated to worker");
    SDL_ClearError();
    require(!platform::sdlMain<failWithSdlError>() &&
            std::string(SDL_GetError()) == "main-thread diagnostic", "SDL error not copied to caller");
    return 17;
  }, [] { SDL_PumpEvents(); SDL_Delay(1); }, [](auto) { require(false, "unexpected worker exception"); });
  require(result == 17, "worker return value lost");
}
void testRuntimeKeepsMainAliveAndOwnsPayloads(SDL_Window *window) {
  const auto main = std::this_thread::get_id();
  const auto result = platform::runSDLApplication(window, [&] {
    require(std::this_thread::get_id() != main, "runtime kept application on main");
    SDL_Event event{};
    while (platform::pollApplicationEvent(&event)) {}
    std::string source = "한글 across threads";
    platform::onMain([&] {
      SDL_SetWindowSize(window, 731, 419);
      SDL_Event text{}; text.type = SDL_EVENT_TEXT_INPUT; text.text.text = source.c_str();
      require(SDL_PushEvent(&text), "could not queue text");
    });
    bool found = false;
    for (int i = 0; i < 100 && !found; ++i) {
      if (platform::waitApplicationEvent(&event, 10) && event.type == SDL_EVENT_TEXT_INPUT) found = true;
    }
    require(found, "runtime did not hand off text");
    platform::onMain([&] { source.assign(4096, 'x'); });
    require(std::string(event.text.text) == "한글 across threads", "main pump invalidated owned payload");
    const auto viewport = platform::getWindowSnapshot(window);
    require(viewport && viewport->width == 731 && viewport->height == 419,
            "worker did not receive resized viewport");
    std::atomic_bool mainDuringStall{false};
    require(SDL_RunOnMainThread([](void *value) {
      static_cast<std::atomic_bool *>(value)->store(true);
    }, &mainDuringStall, false), "could not post stall callback");
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    require(mainDuringStall.load(), "render stall stopped main servicing");
    platform::onMain([] {
      SDL_Event background{}; background.type = SDL_EVENT_DID_ENTER_BACKGROUND;
      SDL_PushEvent(&background);
    });
    for (int i = 0; i < 100 && platform::applicationActive(); ++i) SDL_Delay(1);
    require(!platform::applicationActive(), "lifecycle state waited for owner consumption");
    platform::onMain([] {
      SDL_Event foreground{}; foreground.type = SDL_EVENT_DID_ENTER_FOREGROUND;
      SDL_PushEvent(&foreground);
    });
    for (int i = 0; i < 100 && !platform::applicationActive(); ++i) SDL_Delay(1);
    require(platform::applicationActive(), "foreground state not restored");
    while (platform::pollApplicationEvent(&event)) {}
    platform::onMain([] {
      SDL_Event key{}; key.type = SDL_EVENT_KEY_DOWN;
      for (int i = 0; i < 400; ++i) SDL_PushEvent(&key);
      SDL_Event quit{}; quit.type = SDL_EVENT_QUIT; SDL_PushEvent(&quit);
    });
    bool overflow = false;
    for (int i = 0; i < 100 && !overflow; ++i) {
      overflow = platform::takeApplicationOverflow();
      if (!overflow) SDL_Delay(1);
    }
    require(overflow, "queue pressure did not invalidate buffered input");
    // Wait for the pump's next cycle so it has forwarded the entire flood,
    // including quit, before the owner completes recovery.
    platform::onMain([] {});
    bool cancelled = false, quit = false;
    while (platform::pollApplicationEvent(&event)) {
      cancelled |= event.type == SDL_EVENT_WINDOW_FOCUS_LOST;
      quit |= event.type == SDL_EVENT_QUIT;
      require(event.type != SDL_EVENT_KEY_DOWN, "pressure replayed stale key");
    }
    require(cancelled && quit, "pressure lost cancellation or quit");
    return 29;
  });
  require(result == 29, "runtime lost application result");
  require(!platform::getWindowSnapshot(window), "runtime published dead window state");
}
void testResizePublishesViewportBeforeEvent(SDL_Window *window) {
  const int result = platform::runSDLApplication(window, [&] {
    SDL_Event event{};
    while (platform::pollApplicationEvent(&event)) {}
    viewportReadEntered = releaseViewportRead = false;
    interceptViewportRead = true;
    require(SDL_RunOnMainThread([](void *opaque) {
      SDL_SetWindowSize(static_cast<SDL_Window *>(opaque), orderedViewportWidth, 431);
    }, window, false), "could not request resize");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!viewportReadEntered.load() && std::chrono::steady_clock::now() < deadline) SDL_Delay(1);
    require(viewportReadEntered.load(), "viewport publication barrier was not reached");
    bool sawResize = false;
    const auto checkEvent = [&] {
      if (event.type != SDL_EVENT_WINDOW_RESIZED || event.window.data1 != orderedViewportWidth) return;
      int width = 0, height = 0;
      platform::windowSize(window, &width, &height);
      require(width == orderedViewportWidth && height == 431,
              "resize event became visible before matching viewport snapshot");
      sawResize = true;
    };
    while (platform::pollApplicationEvent(&event)) checkEvent();
    releaseViewportRead = true;
    while (!sawResize && std::chrono::steady_clock::now() < deadline) {
      if (platform::waitApplicationEvent(&event, 10)) checkEvent();
    }
    require(sawResize, "resize notification was lost");
    return 0;
  });
  require(result == 0, "resize publication worker failed");
}
void testOwnerWorkKeepsNativeCancellationAlive(SDL_Window *window) {
  const auto result = platform::runSDLApplication(window, [&] {
    require(platform::isApplicationThread(), "application owner identity missing");
    bool ran = false;
    std::stop_source stop;
    platform::postApplicationWork([&] {
      require(platform::isApplicationThread() && !SDL_IsMainThread(),
              "renderer work must stay on the application owner");
      platform::onMain([] {
        SDL_Event focus{}; focus.type = SDL_EVENT_WINDOW_FOCUS_LOST;
        SDL_PushEvent(&focus);
      });
      require(!platform::applicationActive() && !stop.stop_requested(),
              "desktop focus loss must hide progress without cancelling export");
      platform::onMain([&] {
        require(!platform::isApplicationThread(), "SDL main must not claim renderer ownership");
        SDL_Event background{}; background.type = SDL_EVENT_WILL_ENTER_BACKGROUND;
        SDL_PushEvent(&background);
      });
      require(stop.stop_requested(), "native background did not cancel occupied owner");
      ran = true;
    }, stop);
    require(!ran, "renderer work must defer to a safe scene boundary");
    platform::pollApplicationWork();
    require(ran, "queued renderer work was not dispatched");
    platform::onMain([] {
      SDL_Event foreground{}; foreground.type = SDL_EVENT_DID_ENTER_FOREGROUND;
      SDL_PushEvent(&foreground);
    });
    std::stop_source surfaceStop;
    platform::postApplicationWork([&] {
      platform::onMain([] { platform::setApplicationSurfaceAvailable(false); });
      require(surfaceStop.stop_requested() && platform::applicationActive() &&
              !platform::applicationCanPresent(),
              "surface loss must cancel export without inventing application background");
      platform::onMain([] { platform::setApplicationSurfaceAvailable(true); });
      require(platform::applicationActive(), "surface recreation did not restore presentation");
    }, surfaceStop);
    platform::pollApplicationWork();
    std::stop_source next;
    platform::postApplicationWork([&] {
      require(!next.stop_requested(), "old lifecycle cancellation leaked into next export");
      platform::onMain([] {
        SDL_Event quit{}; quit.type = SDL_EVENT_QUIT; SDL_PushEvent(&quit);
      });
      require(next.stop_requested(), "quit did not cancel occupied renderer owner");
    }, next);
    platform::pollApplicationWork();
    return 0;
  });
  require(result == 0 && !platform::isApplicationThread(), "owner work runtime cleanup failed");
}

void testRuntimeServicesUnwinding(SDL_Window *window) {
  bool cleanupRan = false;
  const auto result = platform::runSDLApplication(window, [&]() -> int {
    struct Cleanup {
      bool &done;
      ~Cleanup() { platform::onMain([&] { done = SDL_IsMainThread(); }); }
    } cleanup{cleanupRan};
    throw std::runtime_error("expected worker failure");
  });
  require(result == EXIT_FAILURE && cleanupRan, "runtime joined before main-thread cleanup");
}

}
int main() {
  SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
  require(SDL_Init(SDL_INIT_VIDEO), SDL_GetError());
  testMainDispatch();
  auto *window = SDL_CreateWindow("runtime test", 320, 200, SDL_WINDOW_HIDDEN);
  require(window, SDL_GetError());
  testRuntimeKeepsMainAliveAndOwnsPayloads(window);
  testResizePublishesViewportBeforeEvent(window);
  testOwnerWorkKeepsNativeCancellationAlive(window);
  testRuntimeServicesUnwinding(window);
  SDL_DestroyWindow(window);
  SDL_Quit();
  std::cout << "SDL application runtime tests passed\n";
}
