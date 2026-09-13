#include "ChartScanWorkScheduler.h"
#include "view/ImageDecodeCoordinator.h"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <new>
#include <string_view>

namespace {
// Only the constructing thread injects failures. Worker allocations and all
// cleanup allocations stay normal, and each construction fails at most once.
thread_local bool observeAllocations = false;
thread_local std::size_t allocationCount = 0;
thread_local std::size_t failureIndex = static_cast<std::size_t>(-1);

void require(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "%s\n", message);
    std::exit(1);
  }
}
} // namespace

void *operator new(std::size_t size) {
  if (observeAllocations && allocationCount++ == failureIndex) {
    observeAllocations = false;
    throw std::bad_alloc();
  }
  if (void *allocation = std::malloc(size == 0 ? 1 : size)) return allocation;
  throw std::bad_alloc();
}

void operator delete(void *allocation) noexcept { std::free(allocation); }
void operator delete(void *allocation, std::size_t) noexcept {
  std::free(allocation);
}

template <typename Factory>
void testConstructionFailureJoinsStartedWorkers(Factory factory) {
  // Warm runtime initialization before measuring this constructor's allocations.
  factory().reset();
  allocationCount = 0;
  observeAllocations = true;
  auto measured = factory();
  observeAllocations = false;
  const auto constructionAllocations = allocationCount;
  measured.reset();
  require(constructionAllocations >= 3, "three-worker construction must allocate thread state");

  // Sweep every owner-thread allocation, including later thread launches after
  // earlier workers have started. No production factory or mock worker is used.
  for (std::size_t index = 0; index < constructionAllocations; ++index) {
    allocationCount = 0;
    failureIndex = index;
    bool caughtAllocationFailure = false;
    observeAllocations = true;
    try {
      auto unexpectedSuccess = factory();
      observeAllocations = false;
    } catch (const std::bad_alloc &) {
      observeAllocations = false;
      caughtAllocationFailure = true;
    }
    require(caughtAllocationFailure, "construction must propagate its allocation failure");
    // A fresh pool can still be constructed and destroyed after each failure.
    factory().reset();
  }
  failureIndex = static_cast<std::size_t>(-1);
  std::printf("Verified %zu construction allocation failures.\n", constructionAllocations);
}

int main(int argc, char **argv) {
  std::set_terminate([] {
    std::fputs("construction terminated instead of joining started workers\n", stderr);
    std::_Exit(2);
  });
  require(argc == 2, "expected scheduler or image pool argument");
  if (std::string_view(argv[1]) == "scheduler") {
    testConstructionFailureJoinsStartedWorkers([] {
      return std::make_unique<chart_scan::WorkScheduler>(3);
    });
  } else if (std::string_view(argv[1]) == "image") {
    testConstructionFailureJoinsStartedWorkers([] {
      return std::make_unique<image_decode::ImageDecodeCoordinator>(
          image_decode::ImageDecodeCoordinator::Loader{}, 3);
    });
  } else {
    require(false, "unknown worker pool argument");
  }
}
