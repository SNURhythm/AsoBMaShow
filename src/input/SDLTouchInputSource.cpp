#include "SDLTouchInputSource.h"
#include "../rendering/common.h"
#include <utility>
int SDLTouchInputSource::EventHandler(void *userdata, SDL_Event *event) {
  auto *source = static_cast<SDLTouchInputSource *>(userdata);
  if (!source->deferEvents) {
    return source->dispatchEvent(event);
  }
  switch (event->type) {
  case SDL_FINGERDOWN: case SDL_FINGERUP: case SDL_FINGERMOTION:
  case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: case SDL_MOUSEMOTION:
    break;
  default:
    return 0;
  }
  std::lock_guard lock(source->pendingMutex);
  if (!source->pendingOverflow) {
    if (source->pendingEvents.size() == 4096) {
      source->pendingEvents.clear();
      source->pendingOverflow = true;
    } else {
      source->pendingEvents.push_back(*event);
    }
  }
  return 0;
}

void SDLTouchInputSource::pumpPendingEvents() {
  drainingEvents.clear();
  bool overflow = false;
  std::size_t serial = 0;
  {
    std::lock_guard lock(pendingMutex);
    drainingEvents.swap(pendingEvents);
    overflow = std::exchange(pendingOverflow, false);
    serial = discardSerial;
  }
  if (overflow) {
    auto cancelled = std::move(activeTouches);
    activeTouches.clear();
    if (handler != nullptr) {
      for (const auto &[finger, point] : cancelled) {
        handler->onFingerUp(finger, point);
      }
    }
    return;
  }
  for (auto &event : drainingEvents) {
    // A callback can pause/reset gameplay while this batch is being drained.
    if (discardSerial != serial) break;
    dispatchEvent(&event);
  }
}

void SDLTouchInputSource::discardPendingEvents() {
  std::lock_guard lock(pendingMutex);
  pendingEvents.clear();
  pendingOverflow = false;
  ++discardSerial;
  activeTouches.clear();
}

void SDLTouchInputSource::dispatchFinger(Uint32 phase, SDL_FingerID finger,
                                        Vector3 point) {
  if (deferEvents) {
    if (phase == SDL_FINGERDOWN) {
      activeTouches[finger] = point;
    } else if (!activeTouches.contains(finger)) {
      return;
    } else if (phase == SDL_FINGERUP) {
      activeTouches.erase(finger);
    } else {
      activeTouches[finger] = point;
    }
  }
  if (phase == SDL_FINGERDOWN) handler->onFingerDown(finger, point);
  else if (phase == SDL_FINGERUP) handler->onFingerUp(finger, point);
  else handler->onFingerMove(finger, point);
}

int SDLTouchInputSource::dispatchEvent(SDL_Event *event) {
  auto *InputSource = this;
  if (InputSource->handler == nullptr) {
    return 0;
  }
  switch (event->type) {
  case SDL_FINGERDOWN: {
    float uiNormX = 0.0f;
    float uiNormY = 0.0f;
    rendering::normalizedToUiNormalized(event->tfinger.x, event->tfinger.y,
                                        uiNormX, uiNormY);
    InputSource->dispatchFinger(SDL_FINGERDOWN, event->tfinger.fingerId,
                                       Vector3(uiNormX, uiNormY, 0.0f));
    break;
  }
  case SDL_FINGERUP: {
    float uiNormX = 0.0f;
    float uiNormY = 0.0f;
    rendering::normalizedToUiNormalized(event->tfinger.x, event->tfinger.y,
                                        uiNormX, uiNormY);
    InputSource->dispatchFinger(SDL_FINGERUP, event->tfinger.fingerId,
                                     Vector3(uiNormX, uiNormY, 0.0f));
    break;
  }
  case SDL_FINGERMOTION: {
    float uiNormX = 0.0f;
    float uiNormY = 0.0f;
    rendering::normalizedToUiNormalized(event->tfinger.x, event->tfinger.y,
                                        uiNormX, uiNormY);
    InputSource->dispatchFinger(SDL_FINGERMOTION, event->tfinger.fingerId,
                                       Vector3(uiNormX, uiNormY, 0.0f));
    break;
  }
    // emulate touch with click
  case SDL_MOUSEBUTTONDOWN: {
    float screenX = static_cast<float>(event->button.x) * rendering::widthScale;
    float screenY =
        static_cast<float>(event->button.y) * rendering::heightScale;
    float uiNormX = 0.0f;
    float uiNormY = 0.0f;
    rendering::screenToUiNormalized(screenX, screenY, uiNormX, uiNormY);
    InputSource->dispatchFinger(SDL_FINGERDOWN, static_cast<SDL_FingerID>(0),
                                       Vector3(uiNormX, uiNormY, 0.0f));
  } break;
  case SDL_MOUSEBUTTONUP: {
    float screenX = static_cast<float>(event->button.x) * rendering::widthScale;
    float screenY =
        static_cast<float>(event->button.y) * rendering::heightScale;
    float uiNormX = 0.0f;
    float uiNormY = 0.0f;
    rendering::screenToUiNormalized(screenX, screenY, uiNormX, uiNormY);
    InputSource->dispatchFinger(SDL_FINGERUP, static_cast<SDL_FingerID>(0),
                                     Vector3(uiNormX, uiNormY, 0.0f));
  } break;
  case SDL_MOUSEMOTION: {
    float screenX = static_cast<float>(event->motion.x) * rendering::widthScale;
    float screenY =
        static_cast<float>(event->motion.y) * rendering::heightScale;
    float uiNormX = 0.0f;
    float uiNormY = 0.0f;
    rendering::screenToUiNormalized(screenX, screenY, uiNormX, uiNormY);
    InputSource->dispatchFinger(SDL_FINGERMOTION, static_cast<SDL_FingerID>(0),
                                       Vector3(uiNormX, uiNormY, 0.0f));
  } break;
    // case SDL_FINGERMOTION:
    //   InputSource->handler->onFingerMove(
    //       event->tfinger.fingerId,
    //       Vector3(event->tfinger.x, event->tfinger.y, 0.0f));
    //   break;
  }
  return 0;
}

SDLTouchInputSource::SDLTouchInputSource(bool deferEvents)
    : deferEvents(deferEvents) {}

SDLTouchInputSource::~SDLTouchInputSource() {
  if (isListening) {
    SDL_DelEventWatch(EventHandler, this);
  }
}

bool SDLTouchInputSource::startListen() {
  if (isListening) {
    return false;
  }
  isListening = true;

  SDL_AddEventWatch(EventHandler, this);
  return true;
}

void SDLTouchInputSource::stopListen() {
  if (!isListening) {
    return;
  }
  isListening = false;
  SDL_DelEventWatch(EventHandler, this);
  discardPendingEvents();
}

void SDLTouchInputSource::setHandler(IInputHandler *handler) {
  this->handler = handler;
}
