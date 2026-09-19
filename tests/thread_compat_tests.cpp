#include <array>
#include <atomic>
#include <cassert>
#include <future>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>

#if __has_include(<stop_token>)
#include <stop_token>
#endif

// Load the standard library before selecting Android's production fallback on
// desktop. C++17 leaves the native stop types unavailable; no library feature
// macros are overridden and platform macros never affect system headers.
#if defined(ASOBMASHOW_TEST_ANDROID_THREAD_FALLBACK) && !defined(__ANDROID__)
#if defined(__cpp_lib_jthread)
#error The fallback test requires a standard library without native jthread
#endif
#define __ANDROID__ 1
#include "ThreadCompat.h"
#undef __ANDROID__
#else
#include "ThreadCompat.h"
#endif

void testEmptyAndMovedThread() {
  std::jthread empty;
  assert(!empty.request_stop());
  assert(!empty.get_stop_token().stop_possible());

  std::jthread original([] {});
  auto token = original.get_stop_token();
  assert(token.stop_possible() && !token.stop_requested());
  std::jthread moved(std::move(original));
  assert(!original.request_stop());
  assert(!original.get_stop_token().stop_possible());
  assert(moved.request_stop());
  assert(token.stop_requested());
  assert(!moved.request_stop());
  moved.join();
  assert(!moved.request_stop());
}

void testJoinedThreadStillOwnsStopState() {
  std::jthread worker([] {});
  auto token = worker.get_stop_token();
  worker.join();
  assert(!token.stop_requested());
  assert(worker.request_stop());
  assert(token.stop_requested());
  assert(!worker.request_stop());
}

void testConcurrentRequestsHaveOneWinner() {
  std::jthread worker([] {});
  auto token = worker.get_stop_token();
  std::promise<void> release;
  auto released = release.get_future().share();
  std::atomic_int accepted{0};
  std::array<std::thread, 8> callers;
  for (auto &caller : callers) {
    caller = std::thread([&] {
      released.wait();
      if (worker.request_stop()) ++accepted;
      assert(token.stop_requested());
    });
  }
  release.set_value();
  for (auto &caller : callers) caller.join();
  worker.join();
  assert(accepted == 1);
  assert(!worker.request_stop());
}

void testJoinedCleanupDoesNotRequestStop() {
  std::stop_token token;
  {
    std::jthread worker([] {});
    token = worker.get_stop_token();
    worker.join();
  }
  assert(!token.stop_requested());

  std::jthread worker([] {});
  token = worker.get_stop_token();
  worker.join();
  worker = std::jthread{};
  assert(!token.stop_requested());
}

void testDetachedCleanupDoesNotCancelWorker() {
  for (bool replace : {false, true}) {
    std::promise<void> release;
    std::promise<bool> completion;
    auto completed = completion.get_future();
    {
      std::jthread worker([released = release.get_future(),
                           completion = std::move(completion)](std::stop_token stop) mutable {
        released.wait();
        completion.set_value(stop.stop_requested());
      });
      worker.detach();
      if (replace) worker = std::jthread{};
    }
    release.set_value();
    assert(!completed.get());
  }
}

void testJoinableCleanupStopsAndJoins() {
  for (bool replace : {false, true}) {
    bool finished = false;
    {
      std::jthread worker([&](std::stop_token stop) {
        while (!stop.stop_requested()) std::this_thread::yield();
        finished = true;
      });
      if (replace) {
        worker = std::jthread{};
        assert(finished);
      }
    }
    assert(finished);
  }
}

int main() {
  testEmptyAndMovedThread();
  testJoinedThreadStillOwnsStopState();
  testConcurrentRequestsHaveOneWinner();
  testJoinedCleanupDoesNotRequestStop();
  testDetachedCleanupDoesNotCancelWorker();
  testJoinableCleanupStopsAndJoins();
}
