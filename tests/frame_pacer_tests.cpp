#include "video/FramePacer.h"

#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

void require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

void testUncappedPacingNeverWaits() {
  FramePacer pacer;
  const auto now = Clock::time_point{};
  pacer.setCap(0);
  pacer.reset(now);
  require(pacer.remaining(now) == Clock::duration::zero(),
          "uncapped reset has no deadline");
  pacer.framePresented(now);
  require(pacer.remaining(now + 1h) == Clock::duration::zero(),
          "uncapped presentation never creates a deadline");
}

void testExactDeadlineAndOverrun() {
  FramePacer pacer;
  const auto now = Clock::time_point{};
  pacer.setCap(100);
  pacer.reset(now);
  require(pacer.remaining(now) == Clock::duration::zero(),
          "first frame after reset is immediate");
  pacer.framePresented(now);
  require(pacer.remaining(now) == 10ms,
          "presentation creates the exact frame deadline");
  require(pacer.remaining(now + 3ms) == 7ms,
          "remaining duration counts down to the exact deadline");
  require(pacer.remaining(now + 10ms) == Clock::duration::zero(),
          "deadline itself has no remaining wait");
  require(pacer.remaining(now + 35ms) == Clock::duration::zero(),
          "an overrun never requests a negative wait");
  pacer.framePresented(now + 35ms);
  require(pacer.remaining(now + 35ms) == 5ms,
          "overrun advances to the next phase-aligned deadline");
}

void testCapChangesAndBackgroundReset() {
  FramePacer pacer;
  const auto now = Clock::time_point{};
  pacer.setCap(100);
  pacer.reset(now);
  pacer.framePresented(now);

  pacer.setCap(50);
  require(pacer.remaining(now + 1ms) == Clock::duration::zero(),
          "changing cap invalidates the old deadline");
  pacer.framePresented(now + 1ms);
  require(pacer.remaining(now + 1ms) == 20ms,
          "new cap starts from the first subsequent presentation");

  const auto foreground = now + 5s;
  pacer.reset(foreground);
  require(pacer.remaining(foreground) == Clock::duration::zero(),
          "foreground reset permits an immediate frame");
  pacer.framePresented(foreground);
  require(pacer.remaining(foreground) == 20ms,
          "foreground reset preserves the selected cap");
}

void testDeadlinesAdvanceWithoutDrift() {
  FramePacer pacer;
  const auto start = Clock::time_point{};
  pacer.setCap(100);
  pacer.reset(start);
  pacer.framePresented(start);

  for (int frame = 1; frame <= 100; ++frame) {
    const auto latePresentation = start + frame * 10ms + 2ms;
    pacer.framePresented(latePresentation);
    const auto expectedDeadline = start + (frame + 1) * 10ms;
    require(pacer.remaining(latePresentation) ==
                expectedDeadline - latePresentation,
            "late frames stay aligned to the original pacing phase");
  }
}
} // namespace

int main() {
  testUncappedPacingNeverWaits();
  testExactDeadlineAndOverrun();
  testCapChangesAndBackgroundReset();
  testDeadlinesAdvanceWithoutDrift();
  return 0;
}
