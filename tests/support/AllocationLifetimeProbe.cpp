#include "AllocationLifetimeProbe.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>

namespace {
std::array<void *, 512> liveAllocations{};
std::size_t liveCount = 0, allocationCount = 0;
constexpr auto noFailure = std::numeric_limits<std::size_t>::max();
std::size_t failAt = noFailure;
bool tracking = false;

void *allocate(std::size_t size) {
  if (tracking && allocationCount++ == failAt) throw std::bad_alloc{};
  void *memory = std::malloc(size == 0 ? 1 : size);
  if (!memory) throw std::bad_alloc{};
  if (tracking) {
    for (auto &slot : liveAllocations) {
      if (!slot) {
        slot = memory;
        ++liveCount;
        return memory;
      }
    }
    std::abort();
  }
  return memory;
}

void release(void *memory) noexcept {
  if (!memory) return;
  for (auto &slot : liveAllocations) {
    if (slot == memory) {
      slot = nullptr;
      --liveCount;
      break;
    }
  }
  std::free(memory);
}

class TrackingScope {
public:
  explicit TrackingScope(std::size_t failure = noFailure) {
    if (tracking || liveCount != 0) std::abort();
    allocationCount = 0;
    failAt = failure;
    tracking = true;
  }
  TrackingScope(const TrackingScope &) = delete;
  TrackingScope &operator=(const TrackingScope &) = delete;
  ~TrackingScope() {
    tracking = false;
    failAt = noFailure;
  }
};
} // namespace

void *operator new(std::size_t size) { return allocate(size); }
void *operator new[](std::size_t size) { return allocate(size); }
void operator delete(void *memory) noexcept { release(memory); }
void operator delete[](void *memory) noexcept { release(memory); }
void operator delete(void *memory, std::size_t) noexcept { release(memory); }
void operator delete[](void *memory, std::size_t) noexcept { release(memory); }

std::size_t test_support::checkAllocationFailures(
    const std::function<void()> &operation) {
  operation();
  {
    TrackingScope scope;
    operation();
  }
  const auto allocations = allocationCount;
  if (allocations == 0 || liveCount != 0) {
    std::fprintf(stderr, "Successful operation: allocations=%zu, live=%zu\n",
                 allocations, liveCount);
    std::abort();
  }
  for (std::size_t index = 0; index < allocations; ++index) {
    bool threw = false;
    {
      TrackingScope scope(index);
      try { operation(); }
      catch (const std::bad_alloc &) { threw = true; }
    }
    if (!threw || liveCount != 0) {
      std::fprintf(stderr, "Allocation %zu: threw=%d, live=%zu\n",
                   index, threw, liveCount);
      std::abort();
    }
  }
  return allocations;
}
