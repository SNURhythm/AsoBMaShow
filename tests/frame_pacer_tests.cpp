#include "video/FramePacer.h"
#include "video/IdlePresentationPacing.h"
#include "ui/PresentationSchedule.h"
#include "video/IOSPresentationPacing.h"
#include "video/MetalDisplayLinkFrame.h"
#include "video/MetalPresentationTiming.h"

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

void testDisplayLinkWakeDoesNotAddTimerSleep() {
  FramePacer explicitPacer, presentationPacer;
  const auto now = Clock::time_point{};
  explicitPacer.setCap(60);
  require(video::iosFramePacingWait(explicitPacer, presentationPacer, now,
                                   now + 1ms, true) == Clock::duration::zero(),
          "display-link wake must not be followed by another frame-cap sleep");
  const auto fallback = video::iosFramePacingWait(
      explicitPacer, presentationPacer, now + 20ms, now + 21ms, false);
  require(fallback > 12ms && fallback < 13ms,
          "timer fallback retains the explicit cap after a display-link frame");
}

void testDisplayLinkRatePreservesCapSemantics() {
  for (const auto cap : {0U, 120U, 144U, 240U})
    require(video::metalDisplayLinkRate(120.0F, cap) == 120.0F,
            "display-link requests full ProMotion rate within the selected cap");
  require(video::metalDisplayLinkRate(120.0F, 60) == 60.0F &&
              video::metalDisplayLinkRate(120.0F, 30) == 30.0F,
          "display divisors can use lower display-link rates");
  for (const auto cap : {1U, 5U, 15U, 75U, 90U})
    require(video::metalDisplayLinkRate(120.0F, cap) == 0.0F,
            "low and nondivisor caps keep their timer pacing");
  for (const auto rate : {0.0F, -1.0F, 1001.0F,
                          std::numeric_limits<float>::quiet_NaN()})
    require(video::metalDisplayLinkRate(rate, 0) == 0.0F,
            "unknown refresh cannot activate display-link pacing");
  require(video::metalDisplayLinkRate(60.0F, 0) == 60.0F,
          "60 Hz displays retain their native cadence");
}

void testDisplayLinkFrameRejectsStaleAndRepeatedDrawables() {
  video::MetalDisplayLinkFrame frame;
  require(!frame.managed(), "no managed frame before a callback wait");
  frame.begin(4);
  require(!frame.receive(10.0, 9.99) && !frame.ready(),
          "late callback must not start a stale frame");
  require(!frame.receive(10.0, std::numeric_limits<double>::quiet_NaN()),
          "invalid callback deadline cannot start rendering");
  require(frame.receive(10.0, 10.008), "fresh callback permits rendering");
  require(!frame.receive(10.001, 10.009), "only one callback may own an iteration");
  require(frame.claim(true, 4, true),
          "accepted frame may complete even if rendering overruns its deadline");
  require(!frame.claim(true, 4, true) && frame.managed(),
          "second claim is rejected without enabling fallback acquisition");
  frame.finish();
  require(!frame.managed(), "unused or submitted frame releases managed ownership");
  frame.begin(5);
  require(frame.receive(11.0, 11.008) && frame.claim(true, 5, true),
          "finishing the old frame permits a fresh lease");
}

void testDisplayLinkFrameRejectsInterruptionAndResize() {
  for (int failure = 0; failure != 3; ++failure) {
    video::MetalDisplayLinkFrame frame;
    frame.begin(7);
    require(frame.receive(10.0, 10.008), "frame begins before interruption");
    require(!frame.claim(failure != 0, failure == 1 ? 8 : 7, failure != 2),
            "inactive, new lifecycle generation or changed texture rejects the lease");
    require(frame.managed() && !frame.claim(true, 7, true),
            "rejected lease cannot be revived or trigger a second acquisition");
    frame.finish();
    require(!frame.managed(), "early exit releases rejected managed frame");
  }
  video::MetalDisplayLinkFrame idle;
  idle.begin(1);
  idle.receive(1.0, 1.008);
  idle.finish();
  require(!idle.claim(true, 1, true), "idle frame must not leak a drawable to later work");
}

void testMetalTimingRetainsStartupAndBoundsRecentHistory() {
  video::MetalPresentationTiming timing;
  auto first = timing.record(10, 10.008, 10.016, true);
  require(first != nullptr, "startup callback must be retained for resume comparison");
  std::weak_ptr<video::MetalPresentationSample> replaced;
  for (unsigned i = 1; i < 120; ++i)
    require(timing.record(10 + i * .008, 10.008 + i * .008, 10.016 + i * .008, true) != nullptr,
            "initial burst preserves consecutive callback timing");
  require(!timing.record(10.96, 10.968, 10.976, true),
          "steady capture must not install handlers every frame");
  for (unsigned i = 0; i < 100; ++i) {
    auto sample = timing.record(12 + i * 1.01, 12.008 + i * 1.01, 12.016 + i * 1.01, true);
    require(sample != nullptr, "sparse steady samples continue after the startup burst");
    if (i == 0) replaced = sample;
  }
  const auto retained = timing.samples();
  require(retained.size() == 136 && retained.front() == first && replaced.expired(),
          "rolling capture preserves startup and releases overwritten samples without growing");
}

void testMetalTimingSeparatesRequestedTargetsFromObservedPresentation() {
  video::MetalPresentationTiming timing;
  auto onTime = timing.record(10, 10.008, 10.016, true);
  auto late = timing.record(11, 11.008, 11.016, true);
  auto unavailable = timing.record(12, 12.008, 12.016, true);
  require(onTime && late && unavailable, "samples exist for timing classification");
  onTime->request = 10.001;
  onTime->scheduledObserved = 10.009; // Late observer does not prove a late request.
  onTime->presented = 10.016;
  onTime->presentationObserved = true;
  late->request = 11.009;
  late->presented = 11.024;
  late->presentationObserved = true;
  unavailable->request = 12.001;
  unavailable->presentationObserved = true; // Simulator/unsupported timestamp: zero.
  timing.record(13, 12.999, 13.008, false);
  const auto report = timing.report();
  require(report.find("requested=3 requestLate=1") != std::string::npos &&
              report.find("displayed=2 displayLateGt1ms=1 unavailable=1 pending=0") != std::string::npos,
          "missing/zero presentation timestamps must not become on-time displayed frames");
  require(report.find("targetLeadMs[n=4 min=8.000 p50=16.000") != std::string::npos &&
              report.find("displayDeltaMs[n=2 min=0.000 p50=0.000 p95=8.000") != std::string::npos,
          "assigned lead and actual target miss must remain separate measurements");
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
void testIOSDrawableWaitConsumesTheFrameBudget() {
  FramePacer explicitPacer, presentationPacer;
  presentationPacer.setCap(video::iosPresentationPacingCap(120.0F, 0, false));
  const auto period = std::chrono::duration_cast<Clock::duration>(
      std::chrono::duration<double>(1.0 / 120.0));
  auto started = Clock::time_point{};
  // A two-drawable swapchain can block submission until the next refresh.
  // That already spends one frame's budget, even with trivial GPU work.
  for (int frame = 0; frame < 120; ++frame) {
    const auto finished = started + period;
    const auto wait = video::iosFramePacingWait(
        explicitPacer, presentationPacer, started, finished);
    require(wait == Clock::duration::zero(),
            "a refresh spent waiting for a drawable must not add another refresh of sleep");
    started = finished + wait;
  }
  require(started == Clock::time_point{} + period * 120,
          "120 display-limited frames consume 120 refreshes, not 240");
}

void testIOSFrameBudgetPreservesCapsAndIdleWaits() {
  FramePacer explicitPacer, presentationPacer;
  const auto started = Clock::time_point{};
  presentationPacer.setCap(video::iosPresentationPacingCap(120.0F, 0, false));
  auto wait = video::iosFramePacingWait(
      explicitPacer, presentationPacer, started, started + 3ms);
  require(wait > 5ms && wait < 6ms,
          "fast and skipped-idle frames wait only the unused part of the 120 Hz budget");

  const auto nextStart = started + 3ms + wait;
  wait = video::iosFramePacingWait(
      explicitPacer, presentationPacer, nextStart, nextStart + 9ms);
  require(wait == Clock::duration::zero(),
          "a frame over its budget continues without sleeping to another refresh");

  explicitPacer.setCap(30);
  presentationPacer.setCap(video::iosPresentationPacingCap(120.0F, 30, false));
  wait = video::iosFramePacingWait(
      explicitPacer, presentationPacer, started, started + 9ms);
  require(wait > 24ms && wait < 25ms,
          "an explicit lower cap still waits after a drawable stall");

  explicitPacer.setCap(0);
  presentationPacer.setCap(video::iosPresentationPacingCap(120.0F, 0, true));
  require(video::iosFramePacingWait(explicitPacer, presentationPacer,
                                   started, started + 1ms) == Clock::duration::zero(),
          "uncapped export has no interactive presentation wait");
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
  testDisplayLinkWakeDoesNotAddTimerSleep();
  testDisplayLinkRatePreservesCapSemantics();
  testDisplayLinkFrameRejectsStaleAndRepeatedDrawables();
  testDisplayLinkFrameRejectsInterruptionAndResize();
  testMetalTimingRetainsStartupAndBoundsRecentHistory();
  testMetalTimingSeparatesRequestedTargetsFromObservedPresentation();
  testIOSDrawableWaitConsumesTheFrameBudget();
  testIOSFrameBudgetPreservesCapsAndIdleWaits();
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
