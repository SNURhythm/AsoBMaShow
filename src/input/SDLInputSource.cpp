//
// Created by XF on 9/4/2024.
//

#include "SDLInputSource.h"

bool SDLInputSource::EventHandler(void *userdata, SDL_Event *event) {
  auto *InputSource = (SDLInputSource *)userdata;
  if (InputSource->handler == nullptr) {
    return 0;
  }
  switch (event->type) {
  case SDL_EVENT_KEY_DOWN:
    InputSource->handler->onKeyDown(event->key.scancode, ScanCode);
    break;
  case SDL_EVENT_KEY_UP:
    InputSource->handler->onKeyUp(event->key.scancode, ScanCode);
    break;
  }
  return 0;
}

SDLInputSource::SDLInputSource() { handler = nullptr; }

SDLInputSource::~SDLInputSource() {
  if (isListening) {
    SDL_RemoveEventWatch(EventHandler, this);
  }
}

bool SDLInputSource::startListen() {
  if (isListening) {
    return false;
  }
  isListening = SDL_AddEventWatch(EventHandler, this);
  return isListening;
}

void SDLInputSource::stopListen() {
  if (!isListening) {
    return;
  }
  isListening = false;
  SDL_RemoveEventWatch(EventHandler, this);
}

void SDLInputSource::setHandler(IInputHandler *handler) {
  this->handler = handler;
}