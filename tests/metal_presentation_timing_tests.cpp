#include "video/MetalPresentationTiming.h"
#include <cassert>
#include <thread>
#include <iostream>
#include <limits>
#include "support/AllocationFailure.h"
using namespace video::presentationTiming;
namespace cpu = video::frameCpuTiming;
namespace {
double clockSeconds = 1;
unsigned clockCalls = 0;
double testClock() noexcept { ++clockCalls; return clockSeconds; }
const auto sceneUpdate = static_cast<unsigned>(cpu::Stage::SceneUpdate);
const auto captureStage = static_cast<unsigned>(cpu::Stage::Capture);
const auto updateBit = 1U << sceneUpdate;

void testCpuStagesAndPhases() {
  const auto before = clockCalls;
  { cpu::Scope disabled(cpu::Stage::SceneUpdate); }
  assert(clockCalls == before);
  cpu::beginFrame(42, testClock);
  cpu::observeGameplay(cpu::GameplayPhase::Active, cpu::GameplayMode::Manual, true);
  {
    cpu::Scope outer(cpu::Stage::SceneUpdate);
    clockSeconds = 2;
    { cpu::Scope nested(cpu::Stage::Capture); clockSeconds = 3; }
    clockSeconds = 4;
    { cpu::Scope repeated(cpu::Stage::Capture); clockSeconds = 5; repeated.finish(); }
    cpu::observeGameplay(cpu::GameplayPhase::Ending, cpu::GameplayMode::Manual, true);
    clockSeconds = 6;
  }
  const auto data = cpu::endFrame(42);
  assert(data.collected && data.openStages == 0 && data.invalidStages == 0);
  assert(data.stages[sceneUpdate].seconds == 5 && data.stages[sceneUpdate].count == 1);
  assert(data.stages[captureStage].seconds == 2 && data.stages[captureStage].count == 2);
  assert(data.phaseFirst == cpu::GameplayPhase::Active && data.phaseLast == cpu::GameplayPhase::Ending);
  assert(data.phaseMask == ((1U << static_cast<unsigned>(cpu::GameplayPhase::Active)) |
                            (1U << static_cast<unsigned>(cpu::GameplayPhase::Ending))));
  assert(data.mode == cpu::GameplayMode::Manual && data.skinSelected);
  assert(!cpu::endFrame(42).collected);

  Capture<1> recorder;
  const auto first = recorder.begin(1, 2, 3, 1);
  recorder.recordCpuFrame(first, data, 6);
  auto rows = recorder.snapshot();
  assert(rows[0].cpu.stages[sceneUpdate].seconds == 5);
  assert(rows[0].times[static_cast<unsigned>(Event::FrameEnd)] == 6);
  const auto text = exportText(rows, 0);
  assert(text.find("active,ending,") != std::string::npos);
  assert(text.find(",5.000000000,1,") != std::string::npos);
  const std::string prefix = "frame,generation,gameplay,callback,deadline,target,preparation_start,preparation_end,render_submit,gpu_start,gpu_end,present_request,present_scheduled_handler,displayed,frame_end,";
  const auto headerStart = text.find(prefix);
  assert(headerStart != std::string::npos);
  std::istringstream csv(text.substr(headerStart));
  std::string header, row;
  std::getline(csv, header);
  std::getline(csv, row);
  assert(std::count(header.begin(), header.end(), ',') == 48);
  assert(std::count(row.begin(), row.end(), ',') == 48);
  const auto next = recorder.begin(7, 8, 9, 2);
  recorder.recordCpuFrame(first, data, 999);
  rows = recorder.snapshot();
  assert(rows[0].token == next && !rows[0].cpu.collected);
  assert(rows[0].times[static_cast<unsigned>(Event::FrameEnd)] == 0);
}

void testCpuInterruptionAndInvalidClock() {
  cpu::beginFrame(51, testClock);
  cpu::Scope stale(cpu::Stage::SceneUpdate);
  auto interrupted = cpu::endFrame(51);
  assert(interrupted.collected && interrupted.openStages == updateBit);
  assert(interrupted.stages[sceneUpdate].count == 0);
  cpu::beginFrame(51, testClock); // Even a reused token is a different scope lifetime.
  stale.finish();
  auto fresh = cpu::endFrame(51);
  assert(fresh.openStages == 0 && fresh.stages[sceneUpdate].count == 0);
  assert(fresh.phaseFirst == cpu::GameplayPhase::Unknown && !fresh.skinSelected);
  cpu::beginFrame(52, testClock);
  assert(!cpu::endFrame(99).collected && cpu::active());
  const auto earlyReturn = [] { cpu::Scope scope(cpu::Stage::Capture); clockSeconds += 1; return; };
  earlyReturn();
  { cpu::Scope bad(cpu::Stage::SceneUpdate); clockSeconds -= 1; }
  clockSeconds = std::numeric_limits<double>::quiet_NaN();
  { cpu::Scope badStart(cpu::Stage::Projection); }
  fresh = cpu::endFrame(52);
  assert(fresh.stages[captureStage].count == 1 && fresh.stages[captureStage].seconds == 1);
  assert(fresh.stages[sceneUpdate].count == 0 && (fresh.invalidStages & updateBit));
  assert(fresh.openStages == 0 && (fresh.invalidStages & (1U << static_cast<unsigned>(cpu::Stage::Projection))));
  clockSeconds = 10;
  cpu::beginFrame(0, testClock);
  const auto before = clockCalls;
  { cpu::Scope disabled(cpu::Stage::Capture); }
  assert(!cpu::active() && clockCalls == before);
}

void testCpuThreadIsolationAndNoAllocation() {
  clockSeconds = 10;
  cpu::beginFrame(61, testClock);
  const auto before = clockCalls;
  std::thread other([] { cpu::Scope scope(cpu::Stage::Capture); });
  other.join();
  assert(clockCalls == before);
  assert(cpu::endFrame(61).stages[captureStage].count == 0);
  Capture<2> recorder;
  test_support::AllocationSizeObserver allocations;
  for (unsigned i = 1; i <= 1000; ++i) {
    const auto token = recorder.begin(10, 11, 12, 1);
    cpu::beginFrame(token, testClock);
    { cpu::Scope scope(cpu::Stage::Capture); clockSeconds += .001; }
    cpu::observeGameplay(cpu::GameplayPhase::Active, cpu::GameplayMode::Replay, true);
    recorder.recordCpuFrame(token, cpu::endFrame(token), clockSeconds);
  }
  assert(allocations.count() == 0);
}
} // namespace

int main() {
  testCpuStagesAndPhases();
  testCpuInterruptionAndInvalidClock();
  testCpuThreadIsolationAndNoAllocation();
  Capture<2> capture;
  const auto first = capture.begin(10, 10.008, 10.016, 7);
  capture.record(first, Event::PreparationStart, 10.001);
  capture.record(first, Event::GpuEnd, 10.004);
  capture.record(first, Event::Displayed, 10.016);
  auto rows = capture.snapshot();
  assert(rows.size() == 1 && rows[0].token == first && rows[0].generation == 7);
  assert(rows[0].times[static_cast<unsigned>(Event::Displayed)] == 10.016);
  assert(rows[0].times[static_cast<unsigned>(Event::PresentScheduled)] == 0);
  capture.begin(11, 11.008, 11.016, 8);
  const auto third = capture.begin(12, 12.008, 12.016, 9);
  capture.record(first, Event::Displayed, 999);
  rows = capture.snapshot();
  assert(rows.size() == 2 && rows.back().token == third);
  assert(rows.back().times[static_cast<unsigned>(Event::Displayed)] == 0);
  capture.record(third, Event::GpuEnd, -1);
  assert(capture.snapshot().back().times[static_cast<unsigned>(Event::GpuEnd)] == 0);
  Capture<512> concurrent;
  auto token = concurrent.begin(20,20.008,20.016,2);
  std::thread gpu([&] { concurrent.record(token,Event::GpuEnd,20.003); });
  std::thread display([&] { concurrent.record(token,Event::Displayed,20.016); });
  gpu.join(); display.join();
  auto stable = concurrent.snapshot();
  // Contention may drop an update but must account for it, never fabricate data.
  unsigned recorded = (stable[0].times[static_cast<unsigned>(Event::GpuEnd)] != 0) +
                      (stable[0].times[static_cast<unsigned>(Event::Displayed)] != 0);
  assert(recorded + concurrent.droppedUpdates() == 2);
  Capture<4> rolling;
  std::atomic<bool> finished{false};
  std::thread writer([&] {
    for (unsigned i = 1; i <= 4096; ++i) {
      const auto id = rolling.begin(i, i + .25, i + .5, i);
      rolling.record(id, Event::GpuEnd, i + .125);
      cpu::FrameData cpuData;
      cpuData.collected = true;
      cpuData.stages[sceneUpdate] = {static_cast<double>(i), 1};
      rolling.recordCpuFrame(id, cpuData, i + .25);
    }
    finished.store(true, std::memory_order_release);
  });
  do {
    for (const auto &row : rolling.snapshot()) {
      assert(row.times[static_cast<unsigned>(Event::Callback)] == row.generation);
      const auto gpuEnd = row.times[static_cast<unsigned>(Event::GpuEnd)];
      assert(gpuEnd == 0 || gpuEnd == row.generation + .125);
      if (row.cpu.collected) {
        assert(row.cpu.stages[sceneUpdate].seconds == row.generation);
        assert(row.times[static_cast<unsigned>(Event::FrameEnd)] == row.generation + .25);
      }
    }
  } while (!finished.load(std::memory_order_acquire));
  writer.join();
  const auto text = exportText(rows, capture.droppedUpdates());
  assert(text.find("Metal presentation timing v2") != std::string::npos);
  assert(text.find("cpu_scene_update_seconds,cpu_scene_update_count") != std::string::npos);
  assert(text.find("gameplay_phase_first,gameplay_phase_last") != std::string::npos);
  assert(text.find("clock=CAMediaTime_seconds") != std::string::npos);
  assert(text.find("present_scheduled_handler") != std::string::npos);
  assert(text.find("12.016000000") != std::string::npos);
  std::cout << "Presentation timing correlation and rollover passed\n";
}
