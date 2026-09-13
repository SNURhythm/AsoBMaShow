#include "music_select/MusicSelectFolderStatusLoader.h"
#include "support/AllocationFailure.h"

#include <cassert>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <new>
#include <stdexcept>

using namespace std::chrono_literals;

namespace {

MusicSelectBar folder(std::string id) {
  return {.id = {std::move(id)}, .kind = skin::MusicSelectBarKind::Folder};
}

std::vector<MusicSelectFolderStatusLoader::Result> waitForResults(
    MusicSelectFolderStatusLoader &loader, std::size_t count) {
  std::vector<MusicSelectFolderStatusLoader::Result> results;
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (results.size() < count && std::chrono::steady_clock::now() < deadline) {
    for (auto &result : loader.takeResults()) results.push_back(std::move(result));
    std::this_thread::yield();
  }
  assert(results.size() == count);
  return results;
}

void testAllocationFailuresAllowIdenticalRequestRetry() {
  const std::string id = "startup-" + std::string(80, 'x');
  std::size_t failures = 0;
  bool completedWithoutFailure = false;
  // Walk actual caller allocations until request succeeds without reaching
  // the fault. This covers row copies, processor ownership, and thread startup
  // without relying on a standard library's allocation count.
  for (std::size_t allocation = 0; allocation < 64; ++allocation) {
    std::atomic_int calls{0};
    MusicSelectFolderStatusLoader loader;
    auto payload = std::make_shared<int>(7);
    const std::weak_ptr<int> retained = payload;
    MusicSelectFolderStatusLoader::Processor process =
        [&, payload](const MusicSelectBar &, std::stop_token) {
          ++calls;
          return skin::MusicSelectBarFrame{};
        };
    payload.reset();
    auto bars = std::vector{folder(id)};
    std::string mode = "ALL";
    bool threw = false;
    bool admitted = false;
    {
      test_support::FailAllocationAfter fault(allocation);
      try {
        admitted = loader.request(std::move(bars), std::move(mode), 1,
                                  std::move(process));
      } catch (const std::bad_alloc &) {
        threw = true;
      }
    }
    if (threw) {
      ++failures;
      assert(calls == 0 && retained.expired());
      assert(loader.takeResults().empty());
      admitted = loader.request({folder(id)}, "ALL", 1,
          [&](const MusicSelectBar &, std::stop_token) {
            ++calls;
            return skin::MusicSelectBarFrame{};
          });
      assert(admitted && "an identical request retries after allocation failure");
    }
    assert(admitted);
    const auto results = waitForResults(loader, 1);
    assert(results.front().id.value == id && results.front().error.empty());
    assert(calls == 1);
    loader.cancel();
    if (!threw) {
      completedWithoutFailure = true;
      break;
    }
  }
  assert(completedWithoutFailure && failures >= 3);
}

void testSupersessionInterruptsActiveProcessor() {
  MusicSelectFolderStatusLoader loader;
  std::promise<void> entered;
  std::promise<void> interrupted;
  std::promise<void> replacement;
  assert(loader.request({folder("old")}, "ALL", 1,
      [&](const MusicSelectBar &, std::stop_token stop) {
        std::mutex mutex;
        std::condition_variable_any condition;
        std::unique_lock lock(mutex);
        entered.set_value();
        condition.wait(lock, stop, [] { return false; });
        interrupted.set_value();
        return skin::MusicSelectBarFrame{};
      }));
  assert(entered.get_future().wait_for(2s) == std::future_status::ready);
  assert(loader.request({folder("new")}, "ALL", 1,
      [&](const MusicSelectBar &, std::stop_token) {
        replacement.set_value();
        return skin::MusicSelectBarFrame{};
      }));
  assert(interrupted.get_future().wait_for(2s) == std::future_status::ready);
  assert(replacement.get_future().wait_for(2s) == std::future_status::ready);
  const auto results = waitForResults(loader, 1);
  assert(results.front().id.value == "new");
  assert(results.front().error.empty());
  loader.cancel();
  assert(loader.takeResults().empty());
}

void testTeardownInterruptsActiveProcessor() {
  std::promise<void> entered;
  std::promise<void> interrupted;
  auto loader = std::make_unique<MusicSelectFolderStatusLoader>();
  assert(loader->request({folder("old")}, "ALL", 1,
      [&](const MusicSelectBar &, std::stop_token stop) {
        std::mutex mutex;
        std::condition_variable_any condition;
        std::unique_lock lock(mutex);
        entered.set_value();
        condition.wait(lock, stop, [] { return false; });
        interrupted.set_value();
        return skin::MusicSelectBarFrame{};
      }));
  assert(entered.get_future().wait_for(2s) == std::future_status::ready);
  auto destroyed = std::async(std::launch::async, [&] { loader.reset(); });
  assert(interrupted.get_future().wait_for(2s) == std::future_status::ready);
  assert(destroyed.wait_for(2s) == std::future_status::ready);
}

void testTransientFailureRetriesWithoutLosingSuccessfulRows() {
  MusicSelectFolderStatusLoader loader;
  std::atomic<int> successfulCalls{0};
  std::atomic<int> failedCalls{0};
  std::promise<void> completed;
  assert(loader.request({folder("good"), folder("retry")}, "ALL", 1,
      [&](const MusicSelectBar &bar) {
        if (bar.id.value == "good") {
          ++successfulCalls;
        } else if (++failedCalls == 1) {
          throw std::runtime_error("session open temporarily failed");
        } else {
          completed.set_value();
        }
        return skin::MusicSelectBarFrame{};
      }));
  assert(completed.get_future().wait_for(2s) == std::future_status::ready);
  assert(successfulCalls == 1);
  assert(failedCalls == 2);
  const auto results = waitForResults(loader, 2);
  assert(results[0].id.value == "good" && results[0].error.empty());
  assert(results[1].id.value == "retry" && results[1].error.empty());
  assert(!loader.request({folder("good"), folder("retry")}, "ALL", 1,
      [](const MusicSelectBar &) { return skin::MusicSelectBarFrame{}; }));
}

void testPersistentFailureIsBoundedAndSameDirectoryCanRetryLater() {
  MusicSelectFolderStatusLoader loader;
  std::atomic<int> attempts{0};
  const auto process = [&](const MusicSelectBar &) -> skin::MusicSelectBarFrame {
    ++attempts;
    throw std::runtime_error("status temporarily unavailable");
  };
  assert(loader.request({folder("retry")}, "ALL", 1, process));
  const auto failed = waitForResults(loader, 1);
  assert(!failed.front().error.empty());
  assert(attempts == 2);
  assert(!loader.retryReady());
  for (int attempt = 0; attempt < 100; ++attempt) {
    assert(!loader.request({folder("retry")}, "ALL", 1, process));
  }
  std::this_thread::sleep_for(1100ms);
  assert(attempts == 2);
  assert(loader.retryReady());
  bool admitted = false;
  std::atomic_int recoveryCalls{0};
  for (std::size_t allocation = 0; allocation < 64; ++allocation) {
    auto bars = std::vector{folder("retry")};
    std::string mode = "ALL";
    MusicSelectFolderStatusLoader::Processor process =
        [&](const MusicSelectBar &, std::stop_token) {
          ++recoveryCalls;
          return skin::MusicSelectBarFrame{};
        };
    bool threw = false;
    {
      test_support::FailAllocationAfter fault(allocation);
      try {
        admitted = loader.request(std::move(bars), std::move(mode), 1,
                                  std::move(process));
      } catch (const std::bad_alloc &) {
        threw = true;
      }
    }
    if (!threw) break;
    assert(loader.retryReady() && recoveryCalls == 0);
    assert(loader.takeResults().empty());
  }
  assert(admitted);
  const auto recovered = waitForResults(loader, 1);
  assert(recovered.front().id.value == "retry" && recovered.front().error.empty());
  assert(recoveryCalls == 1);
  assert(!loader.retryReady());
}

}

int main() {
  testAllocationFailuresAllowIdenticalRequestRetry();
  {
    MusicSelectFolderStatusLoader loader;
    assert(loader.request({}, "ALL", 1, {}));
  }
  testSupersessionInterruptsActiveProcessor();
  testTeardownInterruptsActiveProcessor();
  testTransientFailureRetriesWithoutLosingSuccessfulRows();
  testPersistentFailureIsBoundedAndSameDirectoryCanRetryLater();
}
