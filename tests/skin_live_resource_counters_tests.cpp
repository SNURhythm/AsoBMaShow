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
  counters.generatedPixmapCreated(64);
  counters.movieCreated(256);
  counters.audioCreated(512);
  require(counters.snapshot() ==
              skin::SkinLiveResourceSnapshot{.liveTextures = 2,
                                              .liveResources = 1,
                                              .liveCpuPixmaps = 1,
                                              .liveCpuPixmapBytes = 64,
                                              .liveMovies = 1,
                                              .liveMovieBytes = 256,
                                              .liveAudioIdentities = 1,
                                              .liveAudioDecodedBytes = 512},
          "CPU Pixmap, GPU, movie, and audio ownership are all visible");

  counters.audioDestroyed(512);
  counters.movieDestroyed(256);
  counters.generatedPixmapDestroyed(64);
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
      counters.generatedPixmapCreated(16);
      counters.movieCreated(32);
      counters.audioCreated(64);
      counters.audioDestroyed(64);
      counters.movieDestroyed(32);
      counters.generatedPixmapDestroyed(16);
      counters.textureDestroyed();
      counters.resourceDestroyed();
    }
    finished.store(true, std::memory_order_release);
  });

  while (!finished.load(std::memory_order_acquire)) {
    const auto snapshot = counters.snapshot();
    require(snapshot.liveTextures <= 1 && snapshot.liveResources <= 1 &&
                snapshot.liveCpuPixmaps <= 1 &&
                snapshot.liveCpuPixmapBytes <= 16 &&
                snapshot.liveMovies <= 1 && snapshot.liveMovieBytes <= 32 &&
                snapshot.liveAudioIdentities <= 1 &&
                snapshot.liveAudioDecodedBytes <= 64,
            "one concurrent ownership cycle keeps every resource kind bounded");
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
