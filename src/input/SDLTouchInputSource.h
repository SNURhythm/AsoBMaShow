#pragma once
#include "IInputSource.h"
#include <map>
#include <functional>
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
  using RawEventCallback = std::function<void(const SDL_Event &, std::uint64_t)>;
  void setRawEventCallback(RawEventCallback callback);
  void pumpPendingEvents();
  void discardPendingEvents();
  bool isListening = false;

private:
  int dispatchEvent(SDL_Event *event, std::uint64_t timestampMicros);
  void dispatchFinger(Uint32 phase, SDL_FingerID finger, Vector3 point,
                      std::uint64_t timestampMicros);
  RawEventCallback rawEventCallback;
  const bool deferEvents;
  std::mutex pendingMutex;
  struct PendingEvent { SDL_Event event; std::uint64_t timestampMicros; };
  std::vector<PendingEvent> pendingEvents;
  std::vector<PendingEvent> drainingEvents;
  bool pendingOverflow = false;
  std::size_t discardSerial = 0;
  // Main-thread ownership only. Following overflow, orphan moves/releases
  // cannot revive a touch whose Down was dropped.
  std::map<SDL_FingerID, Vector3> activeTouches;
};
