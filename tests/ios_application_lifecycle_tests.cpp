#include "platform/IOSApplicationLifecycle.h"
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {
void require(bool value, const char *message) {
  if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
void send(platform::IOSApplicationLifecycle &state, Uint32 type) {
  SDL_Event event{};
  event.type = type;
  state.observe(event);
}
void testPermissionAndBackgroundCancellation() {
  platform::IOSApplicationLifecycle state;
  std::stop_source exportStop;
  state.beginExport(exportStop);
  const auto before = state.presentation();
  send(state, SDL_EVENT_WINDOW_MINIMIZED);
  send(state, SDL_EVENT_WINDOW_FOCUS_LOST);
  send(state, SDL_EVENT_WILL_ENTER_BACKGROUND);
  require(!state.presentation().active && !exportStop.stop_requested(),
          "permission alert must stop presentation without cancelling export");
  send(state, SDL_EVENT_WILL_ENTER_FOREGROUND);
  send(state, SDL_EVENT_WINDOW_FOCUS_GAINED);
  require(!state.presentation().active, "foreground transition is not yet active");
  send(state, SDL_EVENT_DID_ENTER_FOREGROUND);
  require(state.presentation().active && !exportStop.stop_requested(),
          "granting permission must let the original export continue");
  require(state.presentation().generation != before.generation,
          "short inactive pulse must invalidate a prepared frame");
  send(state, SDL_EVENT_DID_ENTER_BACKGROUND);
  require(exportStop.stop_requested(), "actual background must cancel occupied owner");
  state.endExport();
  std::stop_source pending;
  state.beginExport(pending);
  require(pending.stop_requested(), "work starting after background must be cancelled");
  state.endExport();
  send(state, SDL_EVENT_DID_ENTER_FOREGROUND);
  std::stop_source next;
  state.beginExport(next);
  require(!next.stop_requested(), "background cancellation must not poison next export");
  send(state, SDL_EVENT_TERMINATING);
  require(next.stop_requested(), "termination must cancel occupied owner");
  send(state, SDL_EVENT_DID_ENTER_FOREGROUND);
  require(!state.presentation().active, "termination must not reopen presentation");
}
void testExportStartingDuringPermissionInterruption() {
  platform::IOSApplicationLifecycle state;
  send(state, SDL_EVENT_WILL_ENTER_BACKGROUND);
  std::stop_source stop;
  state.beginExport(stop);
  require(!stop.stop_requested(), "temporary inactivity must not cancel pending export");
  send(state, SDL_EVENT_QUIT);
  require(stop.stop_requested(), "quit must cancel pending export");
}
void testInterruptedFrameRetiresBeforeFreshDraws() {
  platform::IOSApplicationLifecycle state;
  platform::IOSPreparedFrame frame;
  std::vector<int> queued, presented;
  const auto submit = [&](bool discard) {
    require(state.presentation().active, "frame disposition touched GPU while inactive");
    if (!discard) presented.insert(presented.end(), queued.begin(), queued.end());
    queued.clear();
  };
  frame.begin(state.presentation());
  queued.push_back(1);
  send(state, SDL_EVENT_WILL_ENTER_BACKGROUND);
  require(!frame.finish(state.presentation(), submit) && frame.pending(),
          "interrupted draws must remain pending until safe to retire");
  require(!frame.retire(state.presentation(), submit) && queued.size() == 1,
          "inactive loop must not submit even a discarded frame");
  send(state, SDL_EVENT_DID_ENTER_FOREGROUND);
  require(frame.retire(state.presentation(), submit) && queued.empty(),
          "resume must retire all old draws before scene or resource changes");
  frame.begin(state.presentation());
  queued.push_back(2);
  require(frame.finish(state.presentation(), submit), "fresh frame should present");
  require(presented == std::vector<int>{2}, "stale draws leaked into foreground frame");
  frame.begin(state.presentation());
  queued.push_back(3);
  send(state, SDL_EVENT_WILL_ENTER_BACKGROUND);
  send(state, SDL_EVENT_DID_ENTER_FOREGROUND);
  require(!frame.finish(state.presentation(), submit) && !frame.pending() && queued.empty(),
          "inactive pulse during preparation must discard even after immediate reactivation");
  require(presented == std::vector<int>{2}, "short inactive pulse presented stale draws");
  send(state, SDL_EVENT_WILL_ENTER_BACKGROUND);
  frame.begin(state.presentation());
  queued.push_back(4);
  send(state, SDL_EVENT_DID_ENTER_FOREGROUND);
  require(!frame.finish(state.presentation(), submit) && queued.empty(),
          "preparation beginning inactive must never be presented");
}
}
int main() {
  testPermissionAndBackgroundCancellation();
  testExportStartingDuringPermissionInterruption();
  testInterruptedFrameRetiresBeforeFreshDraws();
  std::cout << "iOS application lifecycle tests passed\n";
}
