#include "ui/PresentationSchedule.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <thread>

namespace {
void present(ui::PresentationSchedule &schedule, std::uint64_t now,
             ui::FrameDemand demand = {}) {
  schedule.didPresent(ui::revisions(), demand, now);
}

void staticFramesSkipDrawingAndWakeOnChanges() {
  ui::PresentationSchedule schedule;
  assert(schedule.shouldRender(100));
  present(schedule, 100);
  int draws = 0;
  for (std::uint64_t now = 108; now < 1'100; now += 8) {
    if (schedule.shouldRender(now)) ++draws;
  }
  assert(draws == 0);
  ui::invalidate();
  assert(schedule.shouldRender(1'099));
  present(schedule, 1'099);
  assert(!schedule.shouldRender(1'100));
  schedule.invalidate();
  assert(schedule.shouldRender(1'100));
}

void animationAndHoldRequestsRetainTheirCadence() {
  ui::PresentationSchedule schedule;
  ui::FrameDemand demand;
  {
    ui::FrameDemandScope frame(demand);
    ui::requestRedrawAt(600);
    ui::requestRedrawAt(500);
    ui::requestRedrawAt(700);
  }
  present(schedule, 100, demand);
  assert(!schedule.shouldRender(499));
  assert(schedule.shouldRender(500));
  demand = {};
  {
    ui::FrameDemandScope frame(demand);
    ui::requestAnimationFrame();
  }
  present(schedule, 500, demand);
  assert(schedule.shouldRender(508));
  assert(schedule.shouldRender(516));
  present(schedule, 516);
  assert(!schedule.shouldRender(524));
  assert(schedule.shouldRender(524, true));
  assert(!schedule.shouldRender(524, false));
}

void changesDuringDrawingAreNotAcknowledgedEarly() {
  ui::PresentationSchedule schedule;
  const auto beforeDrawing = ui::revisions();
  ui::invalidate();
  schedule.didPresent(beforeDrawing, {}, 100);
  assert(schedule.shouldRender(108));
  present(schedule, 108);
  assert(!schedule.shouldRender(116));

  // A render attempt interrupted before presentation has no acknowledgment.
  ui::invalidate();
  (void)ui::revisions();
  assert(schedule.shouldRender(124));
  assert(schedule.shouldRender(132));
  present(schedule, 132);
  assert(!schedule.shouldRender(140));
}

void completionSignalsWakeWithoutWorkerViewAccess() {
  ui::PresentationSchedule schedule;
  present(schedule, 100);
  std::thread worker([] { ui::notifyCompletion(); });
  worker.join();
  assert(schedule.shouldRender(108));
  const auto beforeDrawing = ui::revisions();
  std::thread racingWorker([] { ui::notifyCompletion(); });
  racingWorker.join();
  schedule.didPresent(beforeDrawing, {}, 108);
  assert(schedule.shouldRender(116));
  present(schedule, 116);
  assert(!schedule.shouldRender(124));
}

void ownerScopesAndLocalChangesStayIndependent() {
  ui::FrameDemand owner;
  {
    ui::FrameDemandScope frame(owner);
    ui::requestRedrawAt(900);
    ui::FrameDemand nested;
    {
      ui::FrameDemandScope exportFrame(nested);
      ui::requestAnimationFrame();
    }
    assert(nested.animation);
    ui::requestRedrawAt(700);
  }
  assert(!owner.animation && owner.redrawAt == 700);

  ui::PresentationSchedule schedule;
  present(schedule, 100, owner);
  std::thread exportWorker([] {
    ui::invalidate();
    ui::requestAnimationFrame();
  });
  exportWorker.join();
  assert(!schedule.shouldRender(108));
  assert(schedule.shouldRender(700));
  ui::requestAnimationFrame(); // No active drawing scope: no leaked demand.
  present(schedule, 700);
  assert(!schedule.shouldRender(708));
}

void maintenanceDoesNotBecomeAContinuousAnimation() {
  ui::PresentationSchedule schedule;
  present(schedule, 100);
  assert(!schedule.shouldRender(1'099));
  assert(schedule.shouldRender(1'100));
  assert(schedule.shouldRender(1'101)); // Retry if the resource frame was blocked.
  present(schedule, 1'101);
  assert(!schedule.shouldRender(1'109));
  assert(schedule.shouldRender(2'101));
}
} // namespace

int main() {
  staticFramesSkipDrawingAndWakeOnChanges();
  animationAndHoldRequestsRetainTheirCadence();
  changesDuringDrawingAreNotAcknowledgedEarly();
  completionSignalsWakeWithoutWorkerViewAccess();
  ownerScopesAndLocalChangesStayIndependent();
  maintenanceDoesNotBecomeAContinuousAnimation();
  std::cout << "UI presentation scheduling passed\n";
}
