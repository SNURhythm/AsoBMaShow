#include "video/MetalPresentationTiming.h"
#include <cassert>
#include <thread>
#include <iostream>
using namespace video::presentationTiming;
int main() {
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
    }
    finished.store(true, std::memory_order_release);
  });
  do {
    for (const auto &row : rolling.snapshot()) {
      assert(row.times[static_cast<unsigned>(Event::Callback)] == row.generation);
      const auto gpuEnd = row.times[static_cast<unsigned>(Event::GpuEnd)];
      assert(gpuEnd == 0 || gpuEnd == row.generation + .125);
    }
  } while (!finished.load(std::memory_order_acquire));
  writer.join();
  const auto text = exportText(rows, capture.droppedUpdates());
  assert(text.find("clock=CAMediaTime_seconds") != std::string::npos);
  assert(text.find("present_scheduled_handler") != std::string::npos);
  assert(text.find("12.016000000") != std::string::npos);
  std::cout << "Presentation timing correlation and rollover passed\n";
}
