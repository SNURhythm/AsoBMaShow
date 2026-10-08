#include "view/ScrollMomentum.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

ScrollMomentum fling(std::uint32_t start = 1000, int samples = 10,
                     float unitsPerMillisecond = 1.0F) {
  ScrollMomentum momentum;
  momentum.beginDrag(start);
  std::uint32_t previous = 0;
  for (int sample = 1; sample <= samples; ++sample) {
    const auto elapsed = static_cast<std::uint32_t>(sample * 100 / samples);
    momentum.recordDragDelta((elapsed - previous) * unitsPerMillisecond,
                             start + elapsed);
    previous = elapsed;
  }
  momentum.release(start + 100);
  return momentum;
}

float coast(ScrollMomentum momentum, std::uint32_t releaseTime,
            const std::vector<std::uint32_t> &frameTimes) {
  float distance = 0.0F;
  for (const auto time : frameTimes) {
    float delta = 0.0F;
    if (momentum.step(releaseTime + time, delta)) distance += delta;
  }
  return distance;
}

float coastFor100Milliseconds(int frames, int samples = 10) {
  std::vector<std::uint32_t> times;
  for (int frame = 1; frame <= frames; ++frame) {
    times.push_back(frame * 100 / frames);
  }
  return coast(fling(1000, samples), 1100, times);
}

void testFrameAndTouchRateIndependence() {
  const float at60Hz = coastFor100Milliseconds(6);
  require(at60Hz > 80.0F && at60Hz < 100.0F,
          "a 1000-unit/second fling decelerates during its first 100 ms");
  for (const int frames : {12, 24, 100, 400}) {
    require(std::fabs(coastFor100Milliseconds(frames) - at60Hz) < 0.01F,
            "uncapped rendering must not accelerate the same fling");
  }
  for (const int samples : {5, 6, 12, 24, 100}) {
    require(std::fabs(coastFor100Milliseconds(24, samples) - at60Hz) < 0.01F,
            "touch sampling rate must not change release velocity");
  }
  require(std::fabs(coast(fling(), 1100, {1, 3, 37, 37, 55, 100}) - at60Hz) < 0.01F,
          "uneven frames and repeated timestamps preserve elapsed-time motion");
  auto momentum = fling();
  float delta = 0.0F;
  require(!momentum.step(1100, delta), "a frame at release time must not move");
  require(momentum.step(1110, delta) && delta > 0.0F && delta <= 10.0F,
          "finger-up must not boost velocity beyond the preceding drag");
  require(std::fabs(coast(fling(1000, 10, -1.0F), 1100, {100}) + at60Hz) < 0.01F,
          "both scroll directions use the same decay");
}

void testBatchedEventsAndClockWrap() {
  ScrollMomentum batched;
  batched.beginDrag(1000);
  batched.recordDragDelta(10.0F, 1000);
  batched.recordDragDelta(10.0F, 1020);
  batched.release(1020);
  ScrollMomentum combined;
  combined.beginDrag(1000);
  combined.recordDragDelta(20.0F, 1020);
  combined.release(1020);
  require(std::fabs(coast(batched, 1020, {100}) - coast(combined, 1020, {100})) < 0.01F,
          "same-millisecond touch events accumulate without dividing by zero");
  batched.beginDrag(1000);
  batched.recordDragDelta(20.0F, 1020);
  batched.recordDragDelta(-40.0F, 1020);
  batched.release(1020);
  combined.beginDrag(1000);
  combined.recordDragDelta(-20.0F, 1020);
  combined.release(1020);
  require(std::fabs(coast(batched, 1020, {100}) - coast(combined, 1020, {100})) < 0.01F,
          "release includes a final same-millisecond reversal");
  const auto start = std::numeric_limits<std::uint32_t>::max() - 50;
  require(std::fabs(coast(fling(start), start + 100, {16, 33, 50, 67, 83, 100}) -
                    coastFor100Milliseconds(6)) < 0.01F,
          "SDL tick wraparound does not change fling velocity or distance");
}

void testStoppingAndNewGestures() {
  auto momentum = fling();
  momentum.release(1201);
  float delta = 0.0F;
  require(!momentum.step(1217, delta), "holding still before release must not fling");
  momentum = fling(1000, 10, 0.05F);
  require(!momentum.step(1117, delta), "slow drags must not gain release momentum");
  momentum = fling();
  momentum.stop();
  require(!momentum.step(1117, delta), "cancelling a gesture stops momentum");
  momentum = fling();
  momentum.beginDrag(1110);
  require(!momentum.step(1120, delta), "a new finger-down interrupts the old fling");
  momentum.release(1120);
  require(!momentum.step(1130, delta), "a tap must not inherit the old fling velocity");
  momentum = fling();
  int frames = 0;
  while (momentum.step(1100 + (++frames * 16), delta) && frames < 300) {}
  require(frames < 300, "momentum eventually stops");
}
} // namespace

int main() {
  try {
    testFrameAndTouchRateIndependence();
    testBatchedEventsAndClockWrap();
    testStoppingAndNewGestures();
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
