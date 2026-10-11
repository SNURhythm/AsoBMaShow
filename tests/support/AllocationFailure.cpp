#include "AllocationFailure.h"

#include <cstdlib>
#include <limits>
#include <new>

namespace {
constexpr auto noFailure = std::numeric_limits<std::size_t>::max();
thread_local std::size_t allocationsBeforeFailure = noFailure;
thread_local bool observeSizes = false;
thread_local std::size_t largestAllocation = 0;
thread_local std::size_t allocationCount = 0;
thread_local std::size_t allocationBytes = 0;
}

test_support::FailAllocationAfter::FailAllocationAfter(
    std::size_t successfulAllocations) noexcept {
  allocationsBeforeFailure = successfulAllocations;
}

test_support::FailAllocationAfter::~FailAllocationAfter() {
  allocationsBeforeFailure = noFailure;
}

test_support::AllocationSizeObserver::AllocationSizeObserver() noexcept {
  largestAllocation = 0;
  allocationCount = 0;
  allocationBytes = 0;
  observeSizes = true;
}
test_support::AllocationSizeObserver::~AllocationSizeObserver() {
  observeSizes = false;
}
std::size_t test_support::AllocationSizeObserver::largest() const noexcept {
  return largestAllocation;
}
std::size_t test_support::AllocationSizeObserver::count() const noexcept {
  return allocationCount;
}
std::size_t test_support::AllocationSizeObserver::totalBytes() const noexcept {
  return allocationBytes;
}

void *operator new(std::size_t size) {
  if (observeSizes) {
    if (size > largestAllocation) largestAllocation = size;
    ++allocationCount;
    allocationBytes += size;
  }
  if (allocationsBeforeFailure == 0) {
    allocationsBeforeFailure = noFailure;
    throw std::bad_alloc();
  }
  if (allocationsBeforeFailure != noFailure) --allocationsBeforeFailure;
  if (void *memory = std::malloc(size == 0 ? 1 : size)) return memory;
  throw std::bad_alloc();
}

void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete[](void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void *memory, std::size_t) noexcept { std::free(memory); }
