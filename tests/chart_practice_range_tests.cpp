#include "practice/PracticeConfiguration.h"

#include <cassert>
#include <vector>

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

  const std::vector<long long> timelines = {0, 1'000'000, 1'000'000, 2'500'000,
                                            5'000'000};
  assert(practice::adjacentTimelineMicros(timelines, 1'000'000,
                                          practice::TimelineDirection::Next) ==
         2'500'000);
  assert(practice::adjacentTimelineMicros(
             timelines, 2'500'000, practice::TimelineDirection::Previous) ==
         1'000'000);
  assert(!practice::adjacentTimelineMicros(
      timelines, 0, practice::TimelineDirection::Previous));
  assert(!practice::adjacentTimelineMicros(timelines, 5'000'000,
                                           practice::TimelineDirection::Next));
}
