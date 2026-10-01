#include "../src/scene/play/GuidedAccessReminder.h"
#include <cassert>
#include <iostream>

using gameplay::GuidedAccessReminder;

int main() {
  assert(GuidedAccessReminder::required(true, true, false, false, 0, false));
  assert(!GuidedAccessReminder::required(false, true, false, false, 0, false));
  assert(!GuidedAccessReminder::required(true, false, false, false, 0, false));
  assert(!GuidedAccessReminder::required(true, true, true, false, 0, false));
  assert(!GuidedAccessReminder::required(true, true, false, true, 0, false));
  assert(!GuidedAccessReminder::required(true, true, false, false, 1, false));
  assert(!GuidedAccessReminder::required(true, true, false, false, 0, true));

  GuidedAccessReminder reminder;
  reminder.update(false, true, 0);
  reminder.update(false, true, 10000);
  assert(!reminder.confirming() && !reminder.completed());
  reminder.update(true, true, 10000);
  assert(reminder.confirming() && reminder.progress() == 0);
  reminder.update(true, true, 10999);
  assert(!reminder.completed());
  reminder.update(true, true, 11000);
  assert(!reminder.completed() && reminder.progress() == 1);
  reminder.update(true, true, 11999);
  assert(!reminder.completed() && reminder.progress() == 1);
  reminder.update(true, true, 12000);
  assert(reminder.completed() && reminder.progress() == 1);

  reminder.reset();
  reminder.update(true, true, 0); // A zero tick is still a valid start time.
  reminder.update(true, true, 500);
  assert(reminder.progress() == 0.5F);
  reminder.update(false, true, 600);
  assert(!reminder.confirming() && reminder.progress() == 0);
  reminder.update(true, true, 10000);
  reminder.update(true, true, 10999);
  assert(!reminder.completed());
  reminder.update(true, true, 11999);
  assert(!reminder.completed());
  reminder.update(true, true, 12000);
  assert(reminder.completed());

  reminder.update(true, false, 12000);
  assert(!reminder.confirming() && !reminder.completed());
  reminder.update(true, false, 50000);
  reminder.update(true, true, 50000);
  reminder.update(true, true, 51999);
  assert(!reminder.completed()); // Background time cannot satisfy the delay.
  reminder.update(true, true, 52000);
  assert(reminder.completed());
  reminder.reset();
  assert(!reminder.confirming() && !reminder.completed());
  std::cout << "Guided Access reminder tests passed\n";
}
