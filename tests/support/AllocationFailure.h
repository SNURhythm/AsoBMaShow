#pragma once

#include <cstddef>

namespace test_support {

// Link AllocationFailure.cpp only into test executables. After the specified
// number of successful ordinary allocations on this thread, throw once.
// Exception cleanup can allocate; the default fails the next allocation.
class FailAllocationAfter final {
public:
  explicit FailAllocationAfter(std::size_t successfulAllocations = 0) noexcept;
  ~FailAllocationAfter();
  FailAllocationAfter(const FailAllocationAfter &) = delete;
  FailAllocationAfter &operator=(const FailAllocationAfter &) = delete;
};

using FailNextAllocation = FailAllocationAfter;

} // namespace test_support
