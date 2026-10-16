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

// Reject oversized ordinary allocations without putting the test process at risk.
class AllocationSizeLimit final {
public:
  explicit AllocationSizeLimit(std::size_t maximumBytes) noexcept;
  ~AllocationSizeLimit();
  AllocationSizeLimit(const AllocationSizeLimit &) = delete;
  AllocationSizeLimit &operator=(const AllocationSizeLimit &) = delete;
private:
  std::size_t previousLimit;
};

// Observe ordinary C++ allocations on this thread without changing admission.
class AllocationSizeObserver final {
public:
  AllocationSizeObserver() noexcept;
  ~AllocationSizeObserver();
  [[nodiscard]] std::size_t largest() const noexcept;
  [[nodiscard]] std::size_t count() const noexcept;
  [[nodiscard]] std::size_t totalBytes() const noexcept;
  AllocationSizeObserver(const AllocationSizeObserver &) = delete;
  AllocationSizeObserver &operator=(const AllocationSizeObserver &) = delete;
};

} // namespace test_support
