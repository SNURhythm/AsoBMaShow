#include "music_select/MusicSelectDirectoryLoader.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <utility>

using namespace std::chrono_literals;

namespace {

using Loader = MusicSelectDirectoryLoader;

void waitFor(std::future<void> &event) {
  assert(event.wait_for(2s) == std::future_status::ready);
  event.get();
}

std::vector<Loader::Result> waitForResults(Loader &loader) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  do {
    auto results = loader.takeResults();
    if (!results.empty()) return results;
    std::this_thread::yield();
  } while (std::chrono::steady_clock::now() < deadline);
  assert(false);
  return {};
}

Loader::Content content(std::string id) {
  return {.children = {{.id = {std::move(id)},
                        .kind = skin::MusicSelectBarKind::Folder}}};
}

Loader::Processor completedProcessor(std::promise<void> &completed) {
  auto lifetime = std::shared_ptr<void>(nullptr, [&](void *) {
    completed.set_value();
  });
  return [lifetime = std::move(lifetime)](std::stop_token) {
    return content("child");
  };
}

void testContentIdentityGenerationAndDrain() {
  const auto caller = std::this_thread::get_id();
  auto owner = std::make_shared<int>(42);
  std::shared_ptr<MusicSelectRowProvider> provider(owner, nullptr);
  Loader loader;
  assert(loader.takeResults().empty());
  const auto generation = loader.request({"directory"},
      [caller, provider](std::stop_token stop) {
        assert(std::this_thread::get_id() != caller);
        assert(stop.stop_possible());
        assert(!stop.stop_requested());
        auto loaded = content("child");
        loaded.provider = provider;
        return loaded;
      });
  const auto results = waitForResults(loader);
  assert(generation != 0);
  assert(results.size() == 1);
  assert(results.front().id.value == "directory");
  assert(results.front().generation == generation);
  assert(results.front().error.empty());
  assert(results.front().content.children.size() == 1);
  assert(results.front().content.children.front().id.value == "child");
  assert(!results.front().content.provider.owner_before(provider));
  assert(!provider.owner_before(results.front().content.provider));
  assert(loader.takeResults().empty());
  const auto next = loader.request({"directory"}, [](std::stop_token) {
    return Loader::Content{};
  });
  assert(next > generation);
  const auto empty = waitForResults(loader);
  assert(empty.size() == 1);
  assert(empty.front().generation == next);
  assert(empty.front().error.empty());
  assert(empty.front().content.children.empty());
  assert(!empty.front().content.provider);
}

void testReplacementDoesNotJoinAndOnlyLatestPendingRuns() {
  std::promise<void> entered;
  std::promise<void> released;
  auto release = released.get_future();
  std::atomic<bool> interrupted{false};
  std::atomic<bool> skippedRan{false};
  Loader loader;
  const auto original = loader.request({"old"}, [&](std::stop_token stop) {
    std::stop_callback callback(stop, [&] {
      assert(loader.takeResults().empty());
      interrupted = true;
    });
    entered.set_value();
    waitFor(release);
    assert(stop.stop_requested());
    return content("stale");
  });
  auto started = entered.get_future();
  waitFor(started);
  auto replacement = std::async(std::launch::async, [&] {
    return loader.request({"skipped"}, [&](std::stop_token) {
      skippedRan = true;
      return content("skipped");
    });
  });
  assert(replacement.wait_for(2s) == std::future_status::ready);
  const auto skipped = replacement.get();
  assert(skipped > original);
  assert(interrupted);
  auto latestRequest = std::async(std::launch::async, [&] {
    return loader.request({"latest"}, [](std::stop_token stop) {
      assert(!stop.stop_requested());
      return content("fresh");
    });
  });
  assert(latestRequest.wait_for(2s) == std::future_status::ready);
  const auto latest = latestRequest.get();
  assert(latest > skipped);
  released.set_value();
  const auto results = waitForResults(loader);
  assert(results.size() == 1);
  assert(results.front().id.value == "latest");
  assert(results.front().generation == latest);
  assert(results.front().content.children.front().id.value == "fresh");
  assert(!skippedRan);
}

void testCancelDoesNotJoinAndSuppressesLateFailure() {
  std::promise<void> entered;
  std::promise<void> released;
  auto release = released.get_future();
  std::atomic<bool> interrupted{false};
  Loader loader;
  const auto original = loader.request({"old"}, [&](std::stop_token stop)
      -> Loader::Content {
    std::stop_callback callback(stop, [&] {
      assert(loader.takeResults().empty());
      interrupted = true;
    });
    entered.set_value();
    waitFor(release);
    throw std::runtime_error("stale failure");
  });
  auto started = entered.get_future();
  waitFor(started);
  auto cancelled = std::async(std::launch::async, [&] { loader.cancel(); });
  waitFor(cancelled);
  assert(interrupted);
  const auto next = loader.request({"retry"}, [](std::stop_token stop) {
    assert(!stop.stop_requested());
    return content("recovered");
  });
  assert(next > original);
  released.set_value();
  const auto results = waitForResults(loader);
  assert(results.size() == 1);
  assert(results.front().id.value == "retry");
  assert(results.front().generation == next);
  assert(results.front().error.empty());
}

void testCancelDropsPendingAndLateCompletion() {
  std::promise<void> entered;
  std::promise<void> released;
  std::promise<void> completed;
  auto release = released.get_future();
  std::atomic<bool> pendingRan{false};
  Loader loader;
  loader.request({"old"},
      [&, finish = completedProcessor(completed)](std::stop_token stop) {
        entered.set_value();
        waitFor(release);
        assert(stop.stop_requested());
        return finish(stop);
      });
  auto started = entered.get_future();
  waitFor(started);
  loader.request({"pending"}, [&](std::stop_token) {
    pendingRan = true;
    return Loader::Content{};
  });
  loader.cancel();
  released.set_value();
  auto finished = completed.get_future();
  waitFor(finished);
  assert(loader.takeResults().empty());
  assert(!pendingRan);
}

void testCancelAndReplacementDiscardBufferedResults() {
  for (const bool cancel : {false, true}) {
    std::promise<void> completed;
    Loader loader;
    loader.request({"buffered"}, completedProcessor(completed));
    auto finished = completed.get_future();
    waitFor(finished);
    if (cancel) {
      loader.cancel();
      assert(loader.takeResults().empty());
    }
    const auto latest = loader.request({"latest"}, [](std::stop_token) {
      return content("fresh");
    });
    const auto results = waitForResults(loader);
    assert(results.size() == 1);
    assert(results.front().id.value == "latest");
    assert(results.front().generation == latest);
  }
}

void testFailureAndSameDirectoryRetry() {
  Loader loader;
  std::atomic<int> attempts{0};
  const auto failedGeneration = loader.request({"directory"},
      [&](std::stop_token) -> Loader::Content {
        ++attempts;
        throw std::runtime_error("worker session unavailable");
      });
  const auto failure = waitForResults(loader);
  assert(failure.size() == 1);
  assert(failure.front().id.value == "directory");
  assert(failure.front().generation == failedGeneration);
  assert(failure.front().error == "worker session unavailable");
  assert(failure.front().content.children.empty());
  assert(!failure.front().content.provider);
  assert(attempts == 1);
  const auto retryGeneration = loader.request({"directory"},
      [&](std::stop_token) {
        ++attempts;
        return content("recovered");
      });
  const auto retry = waitForResults(loader);
  assert(retryGeneration > failedGeneration);
  assert(retry.size() == 1);
  assert(retry.front().generation == retryGeneration);
  assert(retry.front().error.empty());
  assert(retry.front().content.children.front().id.value == "recovered");
  assert(attempts == 2);
}

void testUnknownAndEmptyProcessorFailuresDoNotKillWorker() {
  Loader loader;
  loader.request({"unknown"}, [](std::stop_token) -> Loader::Content {
    throw 42;
  });
  const auto unknown = waitForResults(loader);
  assert(unknown.size() == 1);
  assert(!unknown.front().error.empty());
  loader.request({"empty"}, {});
  const auto empty = waitForResults(loader);
  assert(empty.size() == 1);
  assert(empty.front().id.value == "empty");
  assert(!empty.front().error.empty());
  loader.request({"retry"}, [](std::stop_token) { return Loader::Content{}; });
  const auto retry = waitForResults(loader);
  assert(retry.size() == 1);
  assert(retry.front().id.value == "retry");
  assert(retry.front().error.empty());
}

void testExceptionWithEmptyMessageStillReportsFailure() {
  Loader loader;
  loader.request({"failure"}, [](std::stop_token) -> Loader::Content {
    throw std::runtime_error("");
  });
  const auto results = waitForResults(loader);
  assert(results.size() == 1);
  assert(results.front().id.value == "failure");
  assert(!results.front().error.empty());
}

void testDestructorStopsAndJoinsCooperativeWorker() {
  std::promise<void> entered;
  std::promise<void> interrupted;
  std::promise<void> completed;
  auto loader = std::make_unique<Loader>();
  loader->request({"active"},
      [&, finish = completedProcessor(completed)](std::stop_token stop) {
        std::mutex mutex;
        std::condition_variable_any condition;
        std::unique_lock lock(mutex);
        entered.set_value();
        condition.wait(lock, stop, [] { return false; });
        interrupted.set_value();
        return finish(stop);
      });
  auto started = entered.get_future();
  waitFor(started);
  auto destroyed = std::async(std::launch::async, [&] { loader.reset(); });
  waitFor(destroyed);
  auto stopped = interrupted.get_future();
  auto finished = completed.get_future();
  assert(stopped.wait_for(0s) == std::future_status::ready);
  assert(finished.wait_for(0s) == std::future_status::ready);
}

void testUnusedAndIdleDestruction() {
  auto destroyed = std::async(std::launch::async, [] {
    Loader unused;
    unused.cancel();
    unused.cancel();
    Loader idle;
    idle.request({"empty"}, [](std::stop_token) { return Loader::Content{}; });
    waitForResults(idle);
  });
  waitFor(destroyed);
}

void testDestructorIsTerminalForReentrantStopCallback() {
  std::promise<void> entered;
  std::promise<void> callbackCompleted;
  auto callbackFinished = callbackCompleted.get_future();
  std::atomic<bool> callbackRan{false};
  auto loader = std::make_unique<Loader>();
  auto *activeLoader = loader.get();
  loader->request({"active"}, [&](std::stop_token stop) {
    std::stop_callback callback(stop, [&] {
      activeLoader->cancel();
      assert(activeLoader->takeResults().empty());
      const auto rejected = activeLoader->request({"too late"},
          [](std::stop_token) { return Loader::Content{}; });
      assert(rejected == 0);
      callbackRan = true;
      callbackCompleted.set_value();
    });
    entered.set_value();
    waitFor(callbackFinished);
    return Loader::Content{};
  });
  auto started = entered.get_future();
  waitFor(started);
  auto destroyed = std::async(std::launch::async, [&] { loader.reset(); });
  waitFor(destroyed);
  assert(callbackRan);
}

}

int main() {
  testContentIdentityGenerationAndDrain();
  testReplacementDoesNotJoinAndOnlyLatestPendingRuns();
  testCancelDoesNotJoinAndSuppressesLateFailure();
  testCancelDropsPendingAndLateCompletion();
  testCancelAndReplacementDiscardBufferedResults();
  testFailureAndSameDirectoryRetry();
  testUnknownAndEmptyProcessorFailuresDoNotKillWorker();
  testExceptionWithEmptyMessageStillReportsFailure();
  testDestructorStopsAndJoinsCooperativeWorker();
  testUnusedAndIdleDestruction();
  testDestructorIsTerminalForReentrantStopCallback();
}
