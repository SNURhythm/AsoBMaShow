#pragma once

namespace test_support {

// Link AllocationFailure.cpp only into test executables. The next ordinary
// allocation on this thread throws once; exception cleanup can allocate.
class FailNextAllocation final {
public:
  FailNextAllocation() noexcept;
  ~FailNextAllocation();
  FailNextAllocation(const FailNextAllocation &) = delete;
  FailNextAllocation &operator=(const FailNextAllocation &) = delete;
};

} // namespace test_support
