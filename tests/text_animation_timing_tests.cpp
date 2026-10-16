#include "view/TextAnimationTiming.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {
void expect(bool value, const char *message) {
  if (!value) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
}

int main() {
  // 48 px overflow at 48 px/sec: 700 hold, 1000 forward, 850 hold,
  // 1000 reverse, 850 hold. The next cycle adds another 700 ms start hold.
  const auto at = [](std::uint64_t elapsed) {
    return text_animation::marquee(48, 100, 100 + elapsed);
  };
  auto frame = at(0);
  expect(frame.offset == 0 && !frame.moving && frame.redrawAt == 800,
         "marquee starts with a deadline, not animation frames");
  frame = at(700);
  expect(frame.moving && frame.offset == 0,
         "marquee begins moving exactly at the hold boundary");
  expect(std::abs(at(1200).offset - 24) < 0.001f,
         "marquee preserves its forward speed");
  frame = at(1700);
  expect(frame.offset == 48 && !frame.moving && frame.redrawAt == 2650,
         "far edge hold requests its next transition");
  expect(at(2550).moving && std::abs(at(3050).offset - 24) < 0.001f,
         "return movement preserves its speed");
  frame = at(3550);
  expect(!frame.moving && frame.offset == 0 && frame.redrawAt == 5200,
         "adjacent end and start holds share one deadline");
  frame = at(4400);
  expect(!frame.moving && frame.redrawAt == 5200,
         "subsequent marquee cycle keeps the same hold boundary");
  frame = text_animation::marquee(0, 100, 200);
  expect(!frame.moving && frame.redrawAt == 0 && frame.offset == 0,
         "non-overflowing text schedules nothing");
  const auto caret = text_animation::caret(100, 599);
  expect(caret.visible && caret.redrawAt == 600,
         "caret schedules the exact next half-second boundary");
  expect(!text_animation::caret(100, 600).visible &&
             text_animation::caret(100, 600).redrawAt == 1100,
         "caret hides on its boundary and schedules its return");
  expect(text_animation::caret(100, 3600).redrawAt == 4100,
         "late caret rendering advances to the next boundary without drift");
  constexpr std::uint64_t late = (std::uint64_t{1} << 32) + 100;
  expect(text_animation::caret(late, late + 500).redrawAt == late + 1000,
         "caret deadlines retain the full SDL millisecond clock");
  std::cout << "Text animation timing tests passed\n";
}
