#include "practice/PracticeConfiguration.h"

#include <cassert>

int main() {
  practice::RangeSelection selection{.startMicros = 1'000'000,
                                     .endMicros = 5'000'000,
                                     .active = practice::Marker::End};
  selection.placeActiveMarker(500'000, 8'000'000);
  assert(selection.startMicros == 500'000);
  assert(selection.endMicros == 1'000'000);
  assert(selection.active == practice::Marker::Start);

  selection.placeActiveMarker(9'000'000, 8'000'000);
  assert(selection.startMicros == 1'000'000);
  assert(selection.endMicros == 8'000'000);
  assert(selection.active == practice::Marker::End);
}
