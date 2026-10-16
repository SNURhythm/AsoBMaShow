#include "video/FramePacer.h"
#include "video/IdlePresentationPacing.h"
#include "ui/PresentationSchedule.h"
#include "video/IOSPresentationPacing.h"

#include <chrono>
#include <limits>
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

void testDeadlinesSaturateAtClockMaximum() {
  FramePacer pacer;
  pacer.setCap(100);
  const auto nearMaximum = Clock::time_point::max() - 5ms;
  pacer.reset(nearMaximum);
  pacer.framePresented(nearMaximum);
  require(pacer.remaining(nearMaximum) == 5ms,
          "first deadline saturates instead of overflowing the clock");
  pacer.framePresented(Clock::time_point::max());
  require(pacer.remaining(Clock::time_point::max()) == Clock::duration::zero(),
          "presentation at the clock maximum cannot wrap the deadline");
}

void testMultiDayCatchUpKeepsTheOriginalPhase() {
  FramePacer pacer;
  const auto start = Clock::time_point{};
  pacer.setCap(1000);
  pacer.reset(start);
  pacer.framePresented(start);

  const auto afterThirtyDays = start + std::chrono::hours(24 * 30) + 500us;
  pacer.framePresented(afterThirtyDays);
  require(pacer.remaining(afterThirtyDays) == 500us,
          "multi-day catch-up is phase aligned and bounded");
}

void testIOSDefaultPresentationPacingFollowsDisplay() {
  require(video::iosPresentationPacingCap(60.0F, 0, false) == 60,
          "default iOS presentation follows a 60 Hz display");
  require(video::iosPresentationPacingCap(120.0F, 0, false) == 120,
          "default iOS presentation preserves ProMotion refresh rate");
  require(video::iosPresentationPacingCap(59.94F, 0, false) == 60,
          "fractional refresh rate rounds to the frame pacer's integer rate");
  for (const float invalid : {0.0F, -1.0F, 1000000.0F,
                             std::numeric_limits<float>::quiet_NaN(),
                             std::numeric_limits<float>::infinity()}) {
    require(video::iosPresentationPacingCap(invalid, 0, false) == 60,
            "missing display refresh uses the iOS 60 Hz fallback");
  }
  require(video::iosPresentationPacingCap(60.0F, 120, false) == 0,
          "explicit frame caps retain their existing pacer behavior");
  require(video::iosPresentationPacingCap(120.0F, 0, true) == 0,
          "replay export does not wait for display presentation");
}

void testIOSProMotionPresentationPreservesExplicitCapsAndExport() {
  require(video::iosPresentationPacingCap(120.0F, 0, false) == 120,
          "uncapped menus preserve 120 Hz ProMotion presentation");
  require(video::iosPresentationPacingCap(30.0F, 0, false) == 30,
          "menus never exceed the display refresh rate");
  require(video::iosPresentationPacingCap(120.0F, 144, false) == 0,
          "explicit menu frame settings retain their existing pacer behavior");
  FramePacer explicitPacer;
  FramePacer menuPacer;
  const auto now = Clock::time_point{};
  explicitPacer.setCap(30);
  menuPacer.setCap(video::iosPresentationPacingCap(120.0F, 30, false));
  explicitPacer.framePresented(now);
  menuPacer.framePresented(now);
  require(explicitPacer.remaining(now) > menuPacer.remaining(now),
          "the main loop's longer wait preserves a lower explicit menu cap");
  menuPacer.setCap(video::iosPresentationPacingCap(120.0F, 0, false));
  menuPacer.framePresented(now);
  require(menuPacer.remaining(now) > 8ms && menuPacer.remaining(now) < 9ms,
          "uncapped menu presentation uses a 120 Hz frame deadline");
  menuPacer.setCap(video::iosPresentationPacingCap(120.0F, 0, true));
  menuPacer.framePresented(now);
  require(menuPacer.remaining(now) == Clock::duration::zero(),
          "menu export never inherits the interactive presentation wait");
}
void testIdleGateRetainsWaitWithoutGpuVsync() {
  const auto now = Clock::time_point{};
  ui::PresentationSchedule schedule;
  schedule.didPresent(ui::revisions(), {}, 0);
  FramePacer explicitPacer, idlePacer;
  explicitPacer.framePresented(now);
  idlePacer.setCap(video::idlePresentationPacingCap(120.0F, 0,
                                                   !schedule.shouldRender(8)));
  idlePacer.framePresented(now);
  auto wait = std::max(explicitPacer.remaining(now), idlePacer.remaining(now));
  require(wait > 8ms && wait < 9ms,
          "idle uncapped frames must wait even without a GPU vsync submission");
  ui::invalidate();
  idlePacer.setCap(video::idlePresentationPacingCap(120.0F, 0,
                                                   !schedule.shouldRender(16)));
  idlePacer.framePresented(now);
  require(idlePacer.remaining(now) == Clock::duration::zero(),
          "active uncapped frames retain their existing presentation pacing");
  explicitPacer.setCap(30);
  explicitPacer.framePresented(now);
  idlePacer.setCap(video::idlePresentationPacingCap(120.0F, 30, true));
  idlePacer.framePresented(now);
  wait = std::max(explicitPacer.remaining(now), idlePacer.remaining(now));
  require(wait > 33ms && wait < 34ms, "idle preserves an explicit frame cap");
  explicitPacer.setCap(240);
  explicitPacer.framePresented(now);
  idlePacer.setCap(video::idlePresentationPacingCap(120.0F, 240, true));
  idlePacer.reset(now);
  idlePacer.framePresented(now);
  wait = std::max(explicitPacer.remaining(now), idlePacer.remaining(now));
  require(wait > 8ms && wait < 9ms,
          "skipped VSync frames preserve display cadence with a higher explicit cap");
  idlePacer.setCap(video::idlePresentationPacingCap(120.0F, 240, true, false));
  idlePacer.framePresented(now);
  wait = std::max(explicitPacer.remaining(now), idlePacer.remaining(now));
  require(wait > 4ms && wait < 5ms,
          "VSync disabled preserves a higher explicit cap on skipped frames");
  idlePacer.setCap(video::idlePresentationPacingCap(0.0F, 0, true));
  idlePacer.framePresented(now);
  require(idlePacer.remaining(now) > 8ms,
          "a missing display snapshot must not restore the idle hot loop");
}
} // namespace

int main() {
  testIdleGateRetainsWaitWithoutGpuVsync();
  testUncappedPacingNeverWaits();
  testExactDeadlineAndOverrun();
  testCapChangesAndBackgroundReset();
  testDeadlinesAdvanceWithoutDrift();
  testDeadlinesSaturateAtClockMaximum();
  testMultiDayCatchUpKeepsTheOriginalPhase();
  testIOSDefaultPresentationPacingFollowsDisplay();
  testIOSProMotionPresentationPreservesExplicitCapsAndExport();
  return 0;
}
