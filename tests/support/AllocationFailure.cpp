#include "AllocationFailure.h"

#include <cstdlib>
#include <new>

namespace {
thread_local bool failNextAllocation = false;
}

test_support::FailNextAllocation::FailNextAllocation() noexcept {
  failNextAllocation = true;
}

test_support::FailNextAllocation::~FailNextAllocation() {
  failNextAllocation = false;
}

void *operator new(std::size_t size) {
  if (failNextAllocation) {
    failNextAllocation = false;
    throw std::bad_alloc();
  }
  if (void *memory = std::malloc(size == 0 ? 1 : size)) return memory;
  throw std::bad_alloc();
}

void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete[](void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void *memory, std::size_t) noexcept { std::free(memory); }
