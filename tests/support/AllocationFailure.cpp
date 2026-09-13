#include "AllocationFailure.h"

#include <cstdlib>
#include <limits>
#include <new>

namespace {
constexpr auto noFailure = std::numeric_limits<std::size_t>::max();
thread_local std::size_t allocationsBeforeFailure = noFailure;
}

test_support::FailAllocationAfter::FailAllocationAfter(
    std::size_t successfulAllocations) noexcept {
  allocationsBeforeFailure = successfulAllocations;
}

test_support::FailAllocationAfter::~FailAllocationAfter() {
  allocationsBeforeFailure = noFailure;
}

void *operator new(std::size_t size) {
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
