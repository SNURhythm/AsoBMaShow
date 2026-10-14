#include "../src/audio/IOSMusicStreamTimeline.h"
#include <cassert>

int main() {
  ios_music_stream::Timeline clock;
  clock.Reset(1000);
  clock.Queue(4096);
  assert(clock.Position(100, 0) == 1100);
  // A running node clock must freeze at the last queued frame on underrun.
  assert(clock.Position(9000, 4096) == 5096);
  assert(!clock.NeedsRestart(4095, 0));
  // Rendering can exhaust the queue before output/time-pitch latency drains.
  assert(clock.NeedsRestart(4096, 0));
  assert(clock.NeedsRestart(-1, 4096));
  assert(!clock.Drained(4095));
  assert(clock.Drained(4096));
  // A played-back acknowledgement remains valid when the render clock vanishes.
  assert(clock.Position(-1, 3000) == 4000);
  // Seeking discards the old timeline and cannot inherit its completion state.
  clock.Reset(22050);
  assert(clock.Position(-1, 0) == 22050);
  assert(!clock.NeedsRestart(0, 0));
  clock.Queue(1024);
  assert(!clock.Drained(0));
  assert(clock.Position(700, 0) == 22750);
  // A new node epoch after an underrun begins at the consumed source position.
  clock.Reset(23074);
  clock.Queue(1024);
  assert(clock.Position(256, 0) == 23330);
}
