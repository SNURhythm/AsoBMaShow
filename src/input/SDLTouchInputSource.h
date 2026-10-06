#pragma once
#include "IInputSource.h"
#include <map>
#include <mutex>
#include <vector>

class SDLTouchInputSource : public IInputSource {
public:
  static int EventHandler(void *userdata, SDL_Event *event);
  IInputHandler *handler = nullptr;
  explicit SDLTouchInputSource(bool deferEvents = false);
  ~SDLTouchInputSource() override;
  bool startListen() override;
  void stopListen() override;
  void setHandler(IInputHandler *handler) override;
  void pumpPendingEvents();
  void discardPendingEvents();
  bool isListening = false;

private:
  int dispatchEvent(SDL_Event *event);
  void dispatchFinger(Uint32 phase, SDL_FingerID finger, Vector3 point);
  const bool deferEvents;
  std::mutex pendingMutex;
  std::vector<SDL_Event> pendingEvents;
  std::vector<SDL_Event> drainingEvents;
  bool pendingOverflow = false;
  std::size_t discardSerial = 0;
  // Main-thread ownership only. Following overflow, orphan moves/releases
  // cannot revive a touch whose Down was dropped.
  std::map<SDL_FingerID, Vector3> activeTouches;
};
