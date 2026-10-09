#include "SDLTouchInputSource.h"
#include "SDLPointerEvent.h"
#include "InputTimestamp.h"
#include "../rendering/common.h"
#include <utility>
#include <chrono>
bool SDLTouchInputSource::EventHandler(void *userdata, SDL_Event *event) {
  if (sdl_pointer_event::isTouchSynthesizedMouse(*event) ||
      sdl_pointer_event::isMouseSynthesizedTouch(*event)) return 0;
  switch (event->type) {
  case SDL_EVENT_FINGER_DOWN: case SDL_EVENT_FINGER_UP: case SDL_EVENT_FINGER_MOTION:
  case SDL_EVENT_FINGER_CANCELED:
  case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: case SDL_EVENT_MOUSE_MOTION:
    break;
  default:
    return 0;
  }
  const auto receipt = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  const auto timestampMicros = event->common.timestamp == 0 ? receipt
      : input::rebaseTimestampMicros(event->common.timestamp / 1000,
                                     SDL_GetTicksNS() / 1000, receipt);
  auto *source = static_cast<SDLTouchInputSource *>(userdata);
  if (source->rawEventCallback) {
    source->rawEventCallback(*event, timestampMicros);
    return 0;
  }
  if (!source->deferEvents) {
    return source->dispatchEvent(event, timestampMicros);
  }
  std::lock_guard lock(source->pendingMutex);
  if (!source->pendingOverflow) {
    if (source->pendingEvents.size() == 4096) {
      source->pendingEvents.clear();
      source->pendingOverflow = true;
    } else {
      source->pendingEvents.push_back({*event, static_cast<std::uint64_t>(timestampMicros)});
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
    dispatchEvent(&event.event, event.timestampMicros);
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
                                        Vector3 point, std::uint64_t timestampMicros) {
  if (deferEvents) {
    if (phase == SDL_EVENT_FINGER_DOWN) {
      activeTouches[finger] = point;
    } else if (!activeTouches.contains(finger)) {
      return;
    } else if (phase == SDL_EVENT_FINGER_UP || phase == SDL_EVENT_FINGER_CANCELED) {
      activeTouches.erase(finger);
    } else {
      activeTouches[finger] = point;
    }
  }
  handler->dispatchFingerAt(phase, finger, point, timestampMicros);
}

int SDLTouchInputSource::dispatchEvent(SDL_Event *event,
                                        std::uint64_t timestampMicros) {
  auto *InputSource = this;
  if (InputSource->handler == nullptr) {
    return 0;
  }
  switch (event->type) {
  case SDL_EVENT_FINGER_DOWN: {
    float uiNormX = 0.0f;
    float uiNormY = 0.0f;
    rendering::normalizedToUiNormalized(event->tfinger.x, event->tfinger.y,
                                        uiNormX, uiNormY);
    InputSource->dispatchFinger(SDL_EVENT_FINGER_DOWN, event->tfinger.fingerID,
                                       Vector3(uiNormX, uiNormY, 0.0f), timestampMicros);
    break;
  }
  case SDL_EVENT_FINGER_UP:
  case SDL_EVENT_FINGER_CANCELED: {
    float uiNormX = 0.0f;
    float uiNormY = 0.0f;
    rendering::normalizedToUiNormalized(event->tfinger.x, event->tfinger.y,
                                        uiNormX, uiNormY);
    InputSource->dispatchFinger(event->type, event->tfinger.fingerID,
                                     Vector3(uiNormX, uiNormY, 0.0f), timestampMicros);
    break;
  }
  case SDL_EVENT_FINGER_MOTION: {
    float uiNormX = 0.0f;
    float uiNormY = 0.0f;
    rendering::normalizedToUiNormalized(event->tfinger.x, event->tfinger.y,
                                        uiNormX, uiNormY);
    InputSource->dispatchFinger(SDL_EVENT_FINGER_MOTION, event->tfinger.fingerID,
                                       Vector3(uiNormX, uiNormY, 0.0f), timestampMicros);
    break;
  }
    // emulate touch with click
  case SDL_EVENT_MOUSE_BUTTON_DOWN: {
    float screenX = static_cast<float>(event->button.x) * rendering::widthScale;
    float screenY =
        static_cast<float>(event->button.y) * rendering::heightScale;
    float uiNormX = 0.0f;
    float uiNormY = 0.0f;
    rendering::screenToUiNormalized(screenX, screenY, uiNormX, uiNormY);
    InputSource->dispatchFinger(SDL_EVENT_FINGER_DOWN, sdl_pointer_event::kMouseFingerId,
                                       Vector3(uiNormX, uiNormY, 0.0f), timestampMicros);
  } break;
  case SDL_EVENT_MOUSE_BUTTON_UP: {
    float screenX = static_cast<float>(event->button.x) * rendering::widthScale;
    float screenY =
        static_cast<float>(event->button.y) * rendering::heightScale;
    float uiNormX = 0.0f;
    float uiNormY = 0.0f;
    rendering::screenToUiNormalized(screenX, screenY, uiNormX, uiNormY);
    InputSource->dispatchFinger(SDL_EVENT_FINGER_UP, sdl_pointer_event::kMouseFingerId,
                                     Vector3(uiNormX, uiNormY, 0.0f), timestampMicros);
  } break;
  case SDL_EVENT_MOUSE_MOTION: {
    float screenX = static_cast<float>(event->motion.x) * rendering::widthScale;
    float screenY =
        static_cast<float>(event->motion.y) * rendering::heightScale;
    float uiNormX = 0.0f;
    float uiNormY = 0.0f;
    rendering::screenToUiNormalized(screenX, screenY, uiNormX, uiNormY);
    InputSource->dispatchFinger(SDL_EVENT_FINGER_MOTION, sdl_pointer_event::kMouseFingerId,
                                       Vector3(uiNormX, uiNormY, 0.0f), timestampMicros);
  } break;
    // case SDL_EVENT_FINGER_MOTION:
    //   InputSource->handler->onFingerMove(
    //       event->tfinger.fingerID,
    //       Vector3(event->tfinger.x, event->tfinger.y, 0.0f));
    //   break;
  }
  return 0;
}

SDLTouchInputSource::SDLTouchInputSource(bool deferEvents)
    : deferEvents(deferEvents) {}

SDLTouchInputSource::~SDLTouchInputSource() {
  if (isListening) {
    SDL_RemoveEventWatch(EventHandler, this);
  }
}

bool SDLTouchInputSource::startListen() {
  if (isListening) {
    return false;
  }
  isListening = SDL_AddEventWatch(EventHandler, this);
  return isListening;
}

void SDLTouchInputSource::stopListen() {
  if (!isListening) {
    return;
  }
  isListening = false;
  SDL_RemoveEventWatch(EventHandler, this);
  discardPendingEvents();
}

void SDLTouchInputSource::setHandler(IInputHandler *handler) {
  this->handler = handler;
}

void SDLTouchInputSource::setRawEventCallback(RawEventCallback callback) {
  // SDL removes watchers under its callback lock, joining any in-flight call.
  const bool listening = isListening;
  if (listening) stopListen();
  rawEventCallback = std::move(callback);
  discardPendingEvents();
  if (listening) startListen();
}
