#include <SDL3/SDL.h>
#import <Cocoa/Cocoa.h>
#include <cmath>
#include <initializer_list>
#include <cstdio>
#include <cstdlib>

// Exercise the same pre-dispatch and window-listener path as Cocoa's event
// pump, using a hidden window without moving or clicking the desktop cursor.
extern "C" {
struct SDL_VideoDevice;
SDL_VideoDevice *SDL_GetVideoDevice(void);
void Cocoa_HandleMouseEvent(SDL_VideoDevice *, NSEvent *);
}

static void require(bool condition, const char *message) {
  if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}

int main() {
  @autoreleasepool {
    require(SDL_Init(SDL_INIT_VIDEO), SDL_GetError());
    SDL_Window *window = SDL_CreateWindow("Cocoa pointer regression", 640, 480, SDL_WINDOW_HIDDEN);
    require(window != nullptr, SDL_GetError());
    NSWindow *nativeWindow = (__bridge NSWindow *)SDL_GetPointerProperty(
        SDL_GetWindowProperties(window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
    require(nativeWindow != nil, "SDL provides its Cocoa window");
    NSResponder *listener = (NSResponder *)nativeWindow.delegate;
    require(SDL_GetKeyboardFocus() != window, "fixture must remain unfocused");
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
    for (const auto type : {NSEventTypeLeftMouseDown, NSEventTypeLeftMouseUp}) {
      NSEvent *event = [NSEvent mouseEventWithType:type location:NSMakePoint(230, 310)
          modifierFlags:0 timestamp:NSProcessInfo.processInfo.systemUptime
          windowNumber:nativeWindow.windowNumber context:nil eventNumber:0 clickCount:1 pressure:1];
      Cocoa_HandleMouseEvent(SDL_GetVideoDevice(), event);
      if (type == NSEventTypeLeftMouseDown) [listener mouseDown:event];
      else [listener mouseUp:event];
    }
    SDL_Event event;
    int count = 0;
    // Peep rather than pump: this test owns its synthetic event sequence.
    while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_EVENT_MOUSE_BUTTON_DOWN,
                         SDL_EVENT_MOUSE_BUTTON_UP) == 1) {
      require(event.button.type == (count == 0 ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP),
              "one ordered press/release pair reaches SDL");
      require(std::abs(event.button.x - 230) < 0.01F && std::abs(event.button.y - 170) < 0.01F,
              "unfocused Cocoa clicks preserve native coordinates on both edges");
      ++count;
    }
    require(count == 2, "both button edges arrive exactly once");
    SDL_DestroyWindow(window);
    SDL_Quit();
  }
}
