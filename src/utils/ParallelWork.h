#pragma once

#include "../ThreadCompat.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <thread>
#include <utility>
#include <vector>

namespace parallel_work_detail {
constexpr unsigned int workerCount(std::size_t count, unsigned int hardware) {
  if (count == 0) return 0;
  if (hardware == 0) hardware = 4;

  // Keep headroom for render/audio/main threads to reduce frame-time spikes.
  unsigned int reserved = 1;
  if (hardware > 8) {
    reserved = 4;
  } else if (hardware > 4) {
    reserved = 2;
  }
  const unsigned int workers = hardware > reserved ? hardware - reserved : 1;
  return static_cast<unsigned int>(std::min<std::size_t>(workers, count));
}
} // namespace parallel_work_detail

inline unsigned int parallel_worker_count(std::size_t count) {
  return parallel_work_detail::workerCount(count, std::thread::hardware_concurrency());
}

// Borrows the callable until every index has run. Work owns its error handling;
// joining thread owners also preserve that borrow if a later launch fails.
template <typename Func>
void parallel_for_each_index(std::size_t count, Func &&func) {
  const unsigned int workers = parallel_worker_count(count);
  if (workers == 0) return;
  if (workers <= 1) {
    for (std::size_t index = 0; index < count; ++index) func(index);
    return;
  }

  auto &&work = std::forward<Func>(func);
  std::atomic_size_t nextIndex{0};
  std::vector<std::jthread> threads;
  threads.reserve(workers);
  for (unsigned int worker = 0; worker < workers; ++worker) {
    threads.emplace_back([&]() {
      for (;;) {
        const std::size_t index = nextIndex.fetch_add(1, std::memory_order_relaxed);
        if (index >= count) return;
        work(index);
      }
    });
  }
}
