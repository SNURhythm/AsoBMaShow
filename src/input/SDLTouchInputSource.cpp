#include "SDLTouchInputSource.h"
#include "../rendering/common.h"
int SDLTouchInputSource::EventHandler(void *userdata, SDL_Event *event) {
  auto *InputSource = (SDLTouchInputSource *)userdata;
  if (InputSource->handler == nullptr) {
    return 0;
  }
  switch (event->type) {
  case SDL_FINGERDOWN: {
    float uiNormX = 0.0f;
    float uiNormY = 0.0f;
    rendering::normalizedToUiNormalized(event->tfinger.x, event->tfinger.y,
                                        uiNormX, uiNormY);
    InputSource->handler->onFingerDown(event->tfinger.fingerId,
                                       Vector3(uiNormX, uiNormY, 0.0f));
    break;
  }
  case SDL_FINGERUP: {
    float uiNormX = 0.0f;
    float uiNormY = 0.0f;
    rendering::normalizedToUiNormalized(event->tfinger.x, event->tfinger.y,
                                        uiNormX, uiNormY);
    InputSource->handler->onFingerUp(event->tfinger.fingerId,
                                     Vector3(uiNormX, uiNormY, 0.0f));
    break;
  }
  case SDL_FINGERMOTION: {
    float uiNormX = 0.0f;
    float uiNormY = 0.0f;
    rendering::normalizedToUiNormalized(event->tfinger.x, event->tfinger.y,
                                        uiNormX, uiNormY);
    InputSource->handler->onFingerMove(event->tfinger.fingerId,
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
    InputSource->handler->onFingerDown(0, Vector3(uiNormX, uiNormY, 0.0f));
  } break;
  case SDL_MOUSEBUTTONUP: {
    float screenX = static_cast<float>(event->button.x) * rendering::widthScale;
    float screenY =
        static_cast<float>(event->button.y) * rendering::heightScale;
    float uiNormX = 0.0f;
    float uiNormY = 0.0f;
    rendering::screenToUiNormalized(screenX, screenY, uiNormX, uiNormY);
    InputSource->handler->onFingerUp(0, Vector3(uiNormX, uiNormY, 0.0f));
  } break;
  case SDL_MOUSEMOTION: {
    float screenX = static_cast<float>(event->motion.x) * rendering::widthScale;
    float screenY =
        static_cast<float>(event->motion.y) * rendering::heightScale;
    float uiNormX = 0.0f;
    float uiNormY = 0.0f;
    rendering::screenToUiNormalized(screenX, screenY, uiNormX, uiNormY);
    InputSource->handler->onFingerMove(0, Vector3(uiNormX, uiNormY, 0.0f));
  } break;
    // case SDL_FINGERMOTION:
    //   InputSource->handler->onFingerMove(
    //       event->tfinger.fingerId,
    //       Vector3(event->tfinger.x, event->tfinger.y, 0.0f));
    //   break;
  }
  return 0;
}

SDLTouchInputSource::SDLTouchInputSource() { handler = nullptr; }

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
}

void SDLTouchInputSource::setHandler(IInputHandler *handler) {
  this->handler = handler;
}