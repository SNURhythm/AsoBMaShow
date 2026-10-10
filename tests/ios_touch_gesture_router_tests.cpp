#include "input/IOSTouchGestureRouter.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <vector>

using namespace input::native_touch;
using input::ios::TouchGestureRouter;

static void require(bool value, const char *message) {
  if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
static void collect(const RawTouchEvent &event, void *context) {
  static_cast<std::vector<RawTouchEvent> *>(context)->push_back(event);
}

static void testOwnershipIsLatchedBeforeDelivery() {
  TouchGestureRouter router;
  require(!router.claim(11), "menus do not acquire gameplay touch ownership");
  std::vector<RawTouchEvent> first, second;
  auto original = std::make_unique<RawTouchRegistration>(&collect, &first);
  require(router.claim(11), "active gameplay can acquire a UIKit contact");
  original.reset();
  RawTouchRegistration replacement(&collect, &second);
  router.dispatch({11, TouchPhase::Down, .2F, .3F, 1000});
  router.dispatch({11, TouchPhase::Move, .4F, .3F, 2000});
  require(router.contactCount() == 1 && first.empty() && second.empty(),
          "a gesture admitted before Retry must not enter the replacement session");
  router.dispatch({11, TouchPhase::Up, .4F, .3F, 2000});
  require(router.contactCount() == 0 && second.empty(),
          "stale contact is retained until its terminal edge, without late fallback");
  require(router.claim(11), "a fresh UIKit contact may acquire the new session");
  router.dispatch({11, TouchPhase::Down, .6F, .7F, 3000});
  require(second.size() == 1 && second[0].x == .6F,
          "new contact reaches the current gameplay registration");
}

static void testRealHistoryAndChordIdentitiesSurvive() {
  std::vector<RawTouchEvent> events;
  RawTouchRegistration registration(&collect, &events);
  TouchGestureRouter router;
  constexpr std::int64_t first = 0x100000001LL, second = 0x200000001LL;
  require(router.claim(first) && router.claim(second), "both chord contacts are owned");
  router.dispatch({first, TouchPhase::Down, .2F, .5F, 1000});
  router.dispatch({second, TouchPhase::Down, .8F, .5F, 1000});
  router.dispatch({first, TouchPhase::Move, .4F, .5F, 2000});
  router.dispatch({first, TouchPhase::Move, .3F, .5F, 3000});
  router.dispatch({first, TouchPhase::Move, .5F, .5F, 4000});
  router.dispatch({first, TouchPhase::Move, .5F, .5F, 4000});
  router.dispatch({first, TouchPhase::Move, .1F, .5F, 2500});
  router.dispatch({first, TouchPhase::Up, .5F, .5F, 4000});
  router.dispatch({second, TouchPhase::Up, .8F, .5F, 4000});
  require(events.size() == 7 && router.contactCount() == 0,
          "history preserves reversals, drops duplicate/old samples, and keeps same-time Up");
  require(events[0].pointerId == first && events[1].pointerId == second &&
              events[2].x == .4F && events[2].steadyTimestampMicros == 2000 &&
              events[3].x == .3F && events[3].steadyTimestampMicros == 3000 &&
              events[4].x == .5F && events[4].steadyTimestampMicros == 4000,
          "coalesced samples retain the original contact identity and individual timestamp");
  UiTouchEvent ui;
  for (const auto &expected : events) {
    require(RawTouchRegistration::pollUiEvent(ui) &&
                ui.touch.pointerId == expected.pointerId &&
                ui.touch.phase == expected.phase && ui.touch.x == expected.x &&
                ui.touch.steadyTimestampMicros == expected.steadyTimestampMicros,
            "native gameplay and deferred scene UI retain the same ordered history");
  }
  require(!RawTouchRegistration::pollUiEvent(ui), "history is never delivered twice");
}

static void testResetCancelsOnlyStartedContacts() {
  std::vector<RawTouchEvent> events;
  RawTouchRegistration registration(&collect, &events);
  TouchGestureRouter router;
  require(router.claim(11) && router.claim(12), "recognizer admits both touches");
  router.dispatch({11, TouchPhase::Down, .2F, .3F, 1000});
  router.dispatch({11, TouchPhase::Move, .4F, .5F, 2000});
  router.cancelAll(3000);
  router.cancelAll(4000);
  router.dispatch({11, TouchPhase::Up, .4F, .5F, 5000});
  require(router.contactCount() == 0 && events.size() == 3 &&
              events.back().phase == TouchPhase::Cancel && events.back().x == .4F &&
              events.back().y == .5F && events.back().steadyTimestampMicros == 3000,
          "system cancellation releases each started contact exactly once at its last location");
  router.dispatch({12, TouchPhase::Down, .8F, .3F, 6000});
  require(events.size() == 3, "reset also revokes a contact admitted but not yet begun");
}

static void testMenuGestureCannotBecomeGameplayMidStream() {
  TouchGestureRouter router;
  require(!router.claim(9), "menu finger stays with SDL");
  std::vector<RawTouchEvent> events;
  RawTouchRegistration registration(&collect, &events);
  router.dispatch({9, TouchPhase::Move, .1F, .2F, 1000});
  router.dispatch({9, TouchPhase::Up, .1F, .2F, 2000});
  require(events.empty(), "activating gameplay cannot acquire the tail of a menu touch");
}

static void testInterleavedFingerHistoryIsChronological() {
  std::vector<RawTouchEvent> events;
  RawTouchRegistration registration(&collect, &events);
  TouchGestureRouter router;
  require(router.claim(11) && router.claim(12), "both historical contacts are admitted");
  router.dispatch({11, TouchPhase::Down, .2F, .5F, 1000});
  router.dispatch({12, TouchPhase::Down, .8F, .5F, 1000});
  std::array batch{
      RawTouchEvent{11, TouchPhase::Move, .3F, .5F, 2000},
      RawTouchEvent{11, TouchPhase::Move, .4F, .5F, 3000},
      RawTouchEvent{11, TouchPhase::Up, .4F, .5F, 3000},
      RawTouchEvent{12, TouchPhase::Move, .7F, .5F, 2000},
      RawTouchEvent{12, TouchPhase::Move, .6F, .5F, 3000},
      RawTouchEvent{12, TouchPhase::Up, .6F, .5F, 3000}};
  router.dispatchBatch(batch);
  require(events.size() == 8 && events[2].pointerId == 11 &&
              events[3].pointerId == 12 && events[3].steadyTimestampMicros == 2000 &&
              events[4].phase == TouchPhase::Move && events[5].phase == TouchPhase::Move &&
              events[6].phase == TouchPhase::Up && events[7].phase == TouchPhase::Up,
          "all contact histories merge by time, with equal-time release after movement");
  require(router.contactCount() == 0, "merged terminal batch releases both contacts");
}

int main() {
  testInterleavedFingerHistoryIsChronological();
  testOwnershipIsLatchedBeforeDelivery();
  testRealHistoryAndChordIdentitiesSurvive();
  testResetCancelsOnlyStartedContacts();
  testMenuGestureCannotBecomeGameplayMidStream();
}
