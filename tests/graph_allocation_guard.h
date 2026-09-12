#pragma once

#include <cstddef>
#include <cstdlib>
#include <new>

namespace graph_test {

inline thread_local bool allocationGuardActive = false;
inline thread_local std::size_t remainingAllocationBytes = 0;
inline thread_local std::size_t rejectedAllocationBytes = 0;
inline constexpr std::size_t maximumAllocationBytes = 256 * 1024;

struct AllocationGuard {
  AllocationGuard() {
    remainingAllocationBytes = 8 * 1024 * 1024;
    rejectedAllocationBytes = 0;
    allocationGuardActive = true;
  }

  ~AllocationGuard() { allocationGuardActive = false; }

  AllocationGuard(const AllocationGuard &) = delete;
  AllocationGuard &operator=(const AllocationGuard &) = delete;
};

inline void *allocate(std::size_t bytes) {
  if (allocationGuardActive) {
    if (bytes > maximumAllocationBytes || bytes > remainingAllocationBytes) {
      rejectedAllocationBytes = bytes;
      throw std::bad_alloc();
    }
    remainingAllocationBytes -= bytes;
  }
  if (void *memory = std::malloc(bytes == 0 ? 1 : bytes)) {
    return memory;
  }
  throw std::bad_alloc();
}

}

void *operator new(std::size_t bytes) { return graph_test::allocate(bytes); }
void *operator new[](std::size_t bytes) { return graph_test::allocate(bytes); }
void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete[](void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void *memory, std::size_t) noexcept { std::free(memory); }
