#include "skin/beatoraja/SkinLiveResourceCounters.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

void testSnapshotsTrackOnlyBalancedOwnershipTransitions() {
  skin::SkinLiveResourceCounters counters;
  require(counters.snapshot() == skin::SkinLiveResourceSnapshot{},
          "new counters begin at the baseline");

  counters.resourceCreated();
  counters.textureCreated();
  counters.textureCreated();
  require(counters.snapshot() ==
              skin::SkinLiveResourceSnapshot{.liveTextures = 2,
                                              .liveResources = 1},
          "one catalog resource and two unique physical textures are visible");

  counters.textureDestroyed();
  counters.textureDestroyed();
  counters.resourceDestroyed();
  require(counters.snapshot() == skin::SkinLiveResourceSnapshot{},
          "balanced destruction returns exactly to the baseline");
}

void testSnapshotsRemainBoundedDuringConcurrentTransitions() {
  skin::SkinLiveResourceCounters counters;
  std::atomic_bool finished{false};
  std::thread worker([&] {
    for (std::uint32_t index = 0; index < 50'000; ++index) {
      counters.resourceCreated();
      counters.textureCreated();
      counters.textureDestroyed();
      counters.resourceDestroyed();
    }
    finished.store(true, std::memory_order_release);
  });

  while (!finished.load(std::memory_order_acquire)) {
    const auto snapshot = counters.snapshot();
    require(snapshot.liveTextures <= 1 && snapshot.liveResources <= 1,
            "one concurrent ownership cycle keeps each packed counter bounded");
  }
  worker.join();
  require(counters.snapshot() == skin::SkinLiveResourceSnapshot{},
          "concurrent reads leave no residual ownership counts");
}

} // namespace

int main() {
  testSnapshotsTrackOnlyBalancedOwnershipTransitions();
  testSnapshotsRemainBoundedDuringConcurrentTransitions();
  return 0;
}
