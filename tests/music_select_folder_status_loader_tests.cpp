#include "music_select/MusicSelectFolderStatusLoader.h"

#include <cassert>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
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
  assert(loader.request({folder("retry")}, "ALL", 1,
      [](const MusicSelectBar &) { return skin::MusicSelectBarFrame{}; }));
  const auto recovered = waitForResults(loader, 1);
  assert(recovered.front().error.empty());
  assert(!loader.retryReady());
}

}

int main() {
  {
    MusicSelectFolderStatusLoader loader;
    assert(loader.request({}, "ALL", 1, {}));
  }
  testSupersessionInterruptsActiveProcessor();
  testTeardownInterruptsActiveProcessor();
  testTransientFailureRetriesWithoutLosingSuccessfulRows();
  testPersistentFailureIsBoundedAndSameDirectoryCanRetryLater();
}
