#include "IOSTouchProbe.h"
#import <UIKit/UIKit.h>
#import <objc/runtime.h>
#include <SDL3/SDL_events.h>

void InstallIOSTouchProbe() {
  for (NSString *name in @[@"SDL_uikitview", @"AsoGameplayTouchRecognizer"]) {
    Class cls = NSClassFromString(name);
    if (!cls) continue;
    for (NSString *methodName in @[@"touchesBegan:withEvent:", @"touchesMoved:withEvent:",
                                  @"touchesEnded:withEvent:", @"touchesCancelled:withEvent:"]) {
      SEL selector = NSSelectorFromString(methodName);
      Method method = class_getInstanceMethod(cls, selector);
      if (!method) continue;
      auto original = reinterpret_cast<void (*)(id, SEL, NSSet *, UIEvent *)>(method_getImplementation(method));
      IMP replacement = imp_implementationWithBlock(^(id receiver, NSSet *touches, UIEvent *event) {
        ios_touch_probe::Handler handler;
        original(receiver, selector, touches, event);
      });
      method_setImplementation(method, replacement);
    }
    SDL_Log("iOS touch probe observing %s", name.UTF8String);
  }
  SDL_AddEventWatch(+[](void *, SDL_Event *event) -> bool {
    if (ios_touch_probe::activeHandler && ios_touch_probe::activeHandler->enqueued &&
        (event->type == SDL_EVENT_FINGER_DOWN || event->type == SDL_EVENT_FINGER_UP ||
         event->type == SDL_EVENT_FINGER_MOTION || event->type == SDL_EVENT_FINGER_CANCELED)) {
      ios_touch_probe::sdlFingers.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
  }, nullptr);
}
