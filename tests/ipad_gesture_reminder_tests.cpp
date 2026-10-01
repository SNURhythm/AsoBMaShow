#include "../src/scene/play/IpadGestureReminder.h"
#include <cassert>
#include <iostream>

using gameplay::IpadGestureReminder;

void fingersDown(IpadGestureReminder &gesture, int count = 4) {
  for (int i = 0; i < count; ++i) gesture.down(i, 0.3F + i * 0.1F, 0.7F);
}
void swipe(IpadGestureReminder &gesture, float x, float y, int count = 4) {
  for (int i = 0; i < count; ++i) gesture.move(i, x + i * 0.1F, y);
}
void lift(IpadGestureReminder &gesture, int count = 4) {
  for (int i = 0; i < count; ++i) gesture.up(i);
}
int main() {
  assert(IpadGestureReminder::required(true, true, false, false, 0));
  assert(!IpadGestureReminder::required(false, true, false, false, 0));
  assert(!IpadGestureReminder::required(true, false, false, false, 0));
  assert(!IpadGestureReminder::required(true, true, true, false, 0));
  assert(!IpadGestureReminder::required(true, true, false, true, 0));
  assert(!IpadGestureReminder::required(true, true, false, false, 1));
  IpadGestureReminder gesture;
  fingersDown(gesture);
  swipe(gesture, 0.3F, 0.45F);
  assert(!gesture.completed()); // Never start while the swipe fingers are held.
  for (int i = 0; i < 3; ++i) gesture.up(i);
  assert(!gesture.completed());
  gesture.up(3);
  assert(gesture.completed());

  gesture.reset();
  fingersDown(gesture);
  swipe(gesture, 0.3F, 0.665F); // A short 3.5% upward swipe should count.
  assert(!gesture.completed());
  for (int i = 0; i < 3; ++i) gesture.up(i);
  assert(!gesture.completed()); // Sensitivity does not weaken all-fingers-up gating.
  gesture.up(3);
  assert(gesture.completed());

  gesture.reset();
  fingersDown(gesture, 3);
  swipe(gesture, 0.3F, 0.45F, 3);
  lift(gesture, 3);
  assert(!gesture.completed());
  fingersDown(gesture);
  swipe(gesture, 0.3F, 0.69F); // Tiny touch jitter is still too short.
  lift(gesture);
  assert(!gesture.completed());
  fingersDown(gesture);
  swipe(gesture, 0.6F, 0.65F); // Sideways.
  lift(gesture);
  assert(!gesture.completed());
  fingersDown(gesture);
  for (int i = 0; i < 3; ++i) gesture.move(i, 0.3F + i * 0.1F, 0.45F);
  lift(gesture);
  assert(!gesture.completed()); // All four fingers must move up.
  fingersDown(gesture, 5);
  swipe(gesture, 0.3F, 0.45F, 5);
  lift(gesture, 5);
  assert(gesture.completed());
  gesture.reset();
  fingersDown(gesture, 6);
  swipe(gesture, 0.3F, 0.45F, 6);
  lift(gesture, 6);
  assert(gesture.completed());
  gesture.reset();
  fingersDown(gesture);
  swipe(gesture, 0.3F, 0.45F);
  gesture.reset(); // Backgrounding must discard an otherwise successful swipe.
  lift(gesture);
  assert(!gesture.completed());
  fingersDown(gesture);
  swipe(gesture, 0.3F, 0.45F);
  lift(gesture);
  assert(gesture.completed());
  gesture.down(0, 0.3F, 0.7F);
  assert(!gesture.completed()); // A new held touch must invalidate completion.
  std::cout << "iPad gesture reminder tests passed\n";
}
