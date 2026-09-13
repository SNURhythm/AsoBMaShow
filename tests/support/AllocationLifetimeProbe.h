#pragma once

#include <cstddef>
#include <functional>

namespace test_support {

// For synchronous operations that release all ordinary allocations on return.
// Warms initialization, checks successful cleanup, then fails every allocation.
// Link the implementation only into single-threaded test runners: it replaces
// global new/delete and does not track over-aligned allocations.
std::size_t checkAllocationFailures(const std::function<void()> &operation);

} // namespace test_support
