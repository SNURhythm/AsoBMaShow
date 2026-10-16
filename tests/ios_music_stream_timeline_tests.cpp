#include "../src/audio/IOSMusicStreamTimeline.h"
#include "../src/audio/IOSMusicStreamWake.h"
#include <cassert>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>

static void TestFeederWaitsForWork() {
  using namespace std::chrono_literals;
  ios_music_stream::FeederWake wake;
  std::mutex mutex;
  std::promise<void> waiting;
  auto entered = waiting.get_future();
  auto worker = std::async(std::launch::async, [&] {
    std::unique_lock lock(mutex);
    waiting.set_value();
    wake.Wait(lock);
    assert(lock.owns_lock());
  });
  entered.wait();
  // A full queue or EOF tail must sleep until a completion/control signal.
  assert(worker.wait_for(50ms) == std::future_status::timeout);
  // Control operations must be able to acquire the transport lock while idle.
  {
    std::lock_guard lock(mutex);
    wake.Notify();
  }
  assert(worker.wait_for(1s) == std::future_status::ready);
  worker.get();
}

static void TestCompletionBeforeWaitIsRetained() {
  using namespace std::chrono_literals;
  auto wake = std::make_shared<ios_music_stream::FeederWake>();
  // A callback can finish between checking queued frames and entering Wait.
  wake->Notify();
  auto worker = std::async(std::launch::async, [wake] {
    std::mutex mutex;
    std::unique_lock lock(mutex);
    wake->Wait(lock);
  });
  assert(worker.wait_for(1s) == std::future_status::ready);
  worker.get();

  // A late completion retains only its wake state after the adapter goes away.
  auto lateCompletion = [retained = wake] { retained->Notify(); };
  wake.reset();
  lateCompletion();
}

int main() {
  TestFeederWaitsForWork();
  TestCompletionBeforeWaitIsRetained();
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
