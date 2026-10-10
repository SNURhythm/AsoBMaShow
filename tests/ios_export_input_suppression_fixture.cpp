#include "platform/ApplicationEventQueue.h"
#include "platform/IOSApplicationLifecycle.h"
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <utility>

struct IOSWindowSnapshot {};
RUNTIME_DEFINITIONS

void require(bool value, const char *message) {
  if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
bool touchEnabled = true;
void RunIOSMainThread(std::function<void()> operation) { operation(); }
void SetIOSGameplayTouchInputEnabled(bool enabled) { touchEnabled = enabled; }
void ShowIOSReplayExportProgress(std::function<void()>) {}
void HideIOSReplayExportProgress() {}
void updateViewport(Runtime &, SDL_Window *) {}

std::deque<SDL_Event> nativeEvents;
std::function<void()> beforeNativePoll;
bool SDLCALL SDL_PollEvent(SDL_Event *event) {
  if (auto operation = std::exchange(beforeNativePoll, {})) operation();
  if (nativeEvents.empty()) return false;
  *event = nativeEvents.front();
  nativeEvents.pop_front();
  return true;
}

RUNTIME_METHODS

void pump() {
  const auto state = currentRuntime();
  SDL_Window *window = nullptr;
  PRODUCER_PUMP
}
void send(Uint32 type) {
  SDL_Event event{};
  event.type = type;
  currentRuntime()->lifecycle.observe(event);
  nativeEvents.push_back(event);
}

int main() {
  const auto state = std::make_shared<Runtime>();
  publishRuntime(state);
  int discarded = 0;
  state->discardEvent = [&](const SDL_Event &event) {
    if (event.type == SDL_EVENT_KEY_DOWN) ++discarded;
  };
  send(SDL_EVENT_KEY_DOWN);
  send(SDL_EVENT_GAMEPAD_REMOVED);
  pump();
  std::stop_source stop;
  BeginIOSReplayExport(stop);
  require(discarded == 1, "iOS export start replayed buffered input");
  require(!touchEnabled, "iOS export start retained realtime touch ingress");
  send(SDL_EVENT_KEY_DOWN);
  send(SDL_EVENT_LOW_MEMORY);
  pump();
  require(discarded == 2, "iOS export admitted input while owner was occupied");
  send(SDL_EVENT_KEY_DOWN);
  beforeNativePoll = [&] {
    EndIOSReplayExport();
    ResumeIOSGameplayTouchInput(state->lifecycle.presentation().generation);
    require(!touchEnabled, "iOS touch ingress reopened before the export tail drained");
  };
  pump();
  require(discarded == 3, "iOS export completion admitted its native input tail");
  SDL_Event event{};
  bool device = false, lowMemory = false;
  while (PollIOSApplicationEvent(&event)) {
    require(event.type != SDL_EVENT_KEY_DOWN, "iOS export replayed user input");
    device |= event.type == SDL_EVENT_GAMEPAD_REMOVED;
    lowMemory |= event.type == SDL_EVENT_LOW_MEMORY;
  }
  require(device && lowMemory, "iOS export lost retained system/device state");
  const auto oldGeneration = state->lifecycle.presentation().generation;
  send(SDL_EVENT_WILL_ENTER_BACKGROUND);
  send(SDL_EVENT_DID_ENTER_FOREGROUND);
  pump();
  require(state->completedPumpLifecycleGeneration.load() != oldGeneration,
          "iOS producer did not publish the new lifecycle generation");
  ResumeIOSGameplayTouchInput(oldGeneration);
  require(!touchEnabled, "stale iOS owner acknowledgement reopened ingress");
  ResumeIOSGameplayTouchInput(state->lifecycle.presentation().generation);
  require(touchEnabled, "iOS input did not resume after tail drain and lifecycle acknowledgement");
  send(SDL_EVENT_KEY_DOWN);
  pump();
  bool fresh = false;
  while (PollIOSApplicationEvent(&event)) fresh |= event.type == SDL_EVENT_KEY_DOWN;
  require(fresh && discarded == 3, "iOS input admission remained suppressed after export");
}
