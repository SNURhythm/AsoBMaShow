#include "utils/ParallelWork.h"

#include <array>
#include <cassert>
#include <chrono>
#include <future>
#include <limits>
#include <memory>
#include <thread>

void testWorkerCountHeadroomAndWidth() {
  struct Policy { unsigned int hardware, expected; };
  for (const auto policy : std::array<Policy, 9>{{
           {0, 3}, {1, 1}, {2, 1}, {4, 3}, {5, 3},
           {8, 6}, {9, 5}, {16, 12}, {32, 28}}}) {
    assert(parallel_work_detail::workerCount(0, policy.hardware) == 0);
    assert(parallel_work_detail::workerCount(1, policy.hardware) == 1);
    assert(parallel_work_detail::workerCount(100, policy.hardware) == policy.expected);
    assert(parallel_work_detail::workerCount(2, policy.hardware) == std::min(2u, policy.expected));
    assert(parallel_work_detail::workerCount(std::numeric_limits<std::size_t>::max(), policy.hardware)
           == policy.expected);
    if constexpr (std::numeric_limits<std::size_t>::max() > std::numeric_limits<unsigned int>::max()) {
      const auto beyondUnsigned = static_cast<std::size_t>(std::numeric_limits<unsigned int>::max()) + 1;
      assert(parallel_work_detail::workerCount(beyondUnsigned, policy.hardware) == policy.expected);
    }
  }
  for (const std::size_t count : {0u, 1u, 2u, 100u}) {
    assert(parallel_worker_count(count) ==
           parallel_work_detail::workerCount(count, std::thread::hardware_concurrency()));
  }
}

void testEmptyAndSequentialWork() {
  bool called = false;
  parallel_for_each_index(0, [&](std::size_t) { called = true; });
  assert(!called);
  const auto caller = std::this_thread::get_id();
  parallel_for_each_index(1, [&](std::size_t index) {
    assert(index == 0 && std::this_thread::get_id() == caller);
    called = true;
  });
  assert(called);
}

void testExactlyOnceAndBorrowedMoveOnlyWork() {
  constexpr std::size_t count = 257;
  std::array<std::atomic_int, count> visits{};
  struct Work {
    std::array<std::atomic_int, count> &visits;
    std::unique_ptr<int> token = std::make_unique<int>(42);
    void operator()(std::size_t index) {
      assert(token && *token == 42 && index < visits.size());
      ++visits[index];
    }
  } work{visits};
  parallel_for_each_index(count, std::move(work));
  assert(work.token && *work.token == 42);
  for (const auto &visit : visits) assert(visit == 1);
}

void testCompletionWaitsForBorrowedWork() {
  std::atomic_bool entered = false;
  std::atomic_int visits = 0;
  std::promise<void> release;
  auto released = release.get_future().share();
  auto completed = std::async(std::launch::async, [&] {
    parallel_for_each_index(23, [&](std::size_t) {
      entered = true;
      released.wait();
      ++visits;
    });
    assert(visits == 23);
  });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!entered) {
    assert(std::chrono::steady_clock::now() < deadline);
    std::this_thread::yield();
  }
  assert(completed.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
  release.set_value();
  assert(completed.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
  completed.get();
}

int main() {
  testWorkerCountHeadroomAndWidth();
  testEmptyAndSequentialWork();
  testExactlyOnceAndBorrowedMoveOnlyWork();
  testCompletionWaitsForBorrowedWork();
}
