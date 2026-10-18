#include "view/ImageDecodeCoordinator.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <stop_token>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

template <typename Predicate>
bool waitUntil(Predicate predicate,
               std::chrono::milliseconds timeout =
                   std::chrono::milliseconds(1500)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return predicate();
}

struct ControlledLoader {
  std::mutex mutex;
  std::condition_variable cv;
  std::vector<std::pair<std::string, int>> started;
  std::map<std::string, int> calls;
  std::set<std::pair<std::string, int>> released;
  int active = 0;
  int maximumActive = 0;

  std::optional<image_decode::DecodedImageData>
  load(const image_decode::ImageDecodeRequest &request) {
    std::unique_lock lock(mutex);
    const int invocation = ++calls[request.key];
    started.emplace_back(request.key, invocation);
    ++active;
    maximumActive = std::max(maximumActive, active);
    cv.notify_all();
    cv.wait(lock, [&] {
      return released.contains({request.key, invocation});
    });
    --active;
    cv.notify_all();
    return image_decode::DecodedImageData{
        .width = 1,
        .height = 1,
        .rgba = std::make_shared<std::vector<unsigned char>>(
            4, static_cast<unsigned char>(invocation))};
  }

  void release(std::string key, int invocation = 1) {
    std::lock_guard lock(mutex);
    released.emplace(std::move(key), invocation);
    cv.notify_all();
  }

  bool hasStarted(std::string_view key, int invocation = 1) {
    std::lock_guard lock(mutex);
    return std::ranges::find(started,
                             std::pair{std::string(key), invocation}) !=
           started.end();
  }

  int callCount(std::string_view key) {
    std::lock_guard lock(mutex);
    return calls[std::string(key)];
  }

  std::vector<std::string> startOrder() {
    std::lock_guard lock(mutex);
    std::vector<std::string> result;
    for (const auto &[key, invocation] : started) {
      (void)invocation;
      result.push_back(key);
    }
    return result;
  }
};

image_decode::ImageDecodeRequest request(std::string key,
                                         bool priority = false) {
  return {.key = key,
          .path = key + ".ppm",
          .targetWidth = 64,
          .targetHeight = 64,
          .priority = priority};
}

void testTwoWorkerLimitAndPriorityOrdering() {
  ControlledLoader loader;
  image_decode::ImageDecodeCoordinator coordinator(
      [&](const auto &item) { return loader.load(item); });
  expect(coordinator.workerCount() == 2,
         "coordinator exposes the hard two-worker limit");
  const auto first = coordinator.request(request("first"));
  const auto second = coordinator.request(request("second"));
  expect(first != 0 && second != 0 &&
             waitUntil([&] {
               return loader.hasStarted("first") &&
                      loader.hasStarted("second");
             }),
         "both workers can run independently");
  const auto normal = coordinator.request(request("normal"));
  const auto priority = coordinator.request(request("priority", true));
  expect(normal != 0 && priority != 0,
         "queued requests receive consumer tickets");
  loader.release("first");
  expect(waitUntil([&] { return loader.hasStarted("priority"); }),
         "priority work starts at the first available slot");
  expect(!loader.hasStarted("normal"),
         "ordinary queued work remains behind priority work");
  loader.release("priority");
  loader.release("second");
  expect(waitUntil([&] { return loader.hasStarted("normal"); }),
         "ordinary work resumes after priority work");
  loader.release("normal");
  expect(waitUntil([&] {
           std::lock_guard lock(loader.mutex);
           return loader.active == 0;
         }),
         "all controlled work drains");
  expect(loader.maximumActive == 2,
         "worker concurrency never exceeds two");
}

void testDuplicateConsumersShareOneDecode() {
  ControlledLoader loader;
  image_decode::ImageDecodeCoordinator coordinator(
      [&](const auto &item) { return loader.load(item); });
  const auto first = coordinator.request(request("shared"));
  const auto second = coordinator.request(request("shared", true));
  expect(first != second && waitUntil([&] { return loader.hasStarted("shared"); }),
         "duplicate consumers receive distinct tickets for one work item");
  loader.release("shared");
  std::optional<image_decode::DecodedImageData> firstImage;
  std::optional<image_decode::DecodedImageData> secondImage;
  expect(waitUntil([&] {
           firstImage = coordinator.takeReady(first);
           return firstImage.has_value();
         }) &&
             waitUntil([&] {
               secondImage = coordinator.takeReady(second);
               return secondImage.has_value();
             }),
         "every live consumer receives the shared completion");
  expect(loader.callCount("shared") == 1 && firstImage->rgba == secondImage->rgba,
         "duplicate consumers share one decode allocation");
  expect(coordinator.readyBytes() == 0,
         "ready result is released after its final consumer");
}

void testQueuedAndInFlightOrphansAreDiscarded() {
  ControlledLoader loader;
  image_decode::ImageDecodeCoordinator coordinator(
      [&](const auto &item) { return loader.load(item); });
  const auto first = coordinator.request(request("block-a"));
  const auto second = coordinator.request(request("block-b"));
  expect(first != 0 && second != 0 && waitUntil([&] {
           return loader.hasStarted("block-a") &&
                  loader.hasStarted("block-b");
         }),
         "orphan test occupies both workers");
  const auto queued = coordinator.request(request("queued-orphan"));
  coordinator.cancel(queued);
  expect(coordinator.pendingCount("queued-orphan") == 0,
         "cancelling the only queued consumer removes its work");
  coordinator.cancel(first);
  loader.release("block-a");
  loader.release("block-b");
  expect(waitUntil([&] {
           std::lock_guard lock(loader.mutex);
           return loader.active == 0;
         }),
         "in-flight orphan finishes outside the coordinator lock");
  expect(loader.callCount("queued-orphan") == 0 &&
             !coordinator.takeReady(first).has_value() &&
             coordinator.readyBytes() <= 4,
         "queued orphan never runs and in-flight orphan result is discarded");
  coordinator.cancel(second);
}

void testDropPreventsStaleCompletionFromReplacingNewWork() {
  ControlledLoader loader;
  image_decode::ImageDecodeCoordinator coordinator(
      [&](const auto &item) { return loader.load(item); });
  const auto stale = coordinator.request(request("same"));
  expect(stale != 0 && waitUntil([&] { return loader.hasStarted("same", 1); }),
         "stale request reaches a worker");
  expect(coordinator.isTracked(stale),
         "a live consumer ticket is observable before eviction");
  coordinator.drop("same");
  expect(!coordinator.isTracked(stale),
         "dropping decode work makes its consumer ticket observably stale");
  const auto current = coordinator.request(request("same"));
  expect(current != 0 && waitUntil([&] { return loader.hasStarted("same", 2); }),
         "new generation can start while stale work completes");
  loader.release("same", 2);
  std::optional<image_decode::DecodedImageData> image;
  expect(waitUntil([&] {
           image = coordinator.takeReady(current);
           return image.has_value();
         }) && image->rgba && (*image->rgba)[0] == 2,
         "current generation publishes its own result");
  loader.release("same", 1);
  expect(waitUntil([&] {
           std::lock_guard lock(loader.mutex);
           return loader.active == 0;
         }) && !coordinator.takeReady(stale).has_value(),
         "late stale completion cannot overwrite or resurrect a ticket");
}

void testTerminalWaitIsNotifiedAndTakesReadyImage() {
  ControlledLoader loader;
  image_decode::ImageDecodeCoordinator coordinator(
      [&](const auto &item) { return loader.load(item); });
  const auto ticket = coordinator.request(request("terminal"));
  expect(ticket != 0 && waitUntil([&] { return loader.hasStarted("terminal"); }),
         "terminal wait test starts its controlled decode");

  std::optional<image_decode::ImageDecodeWaitResult> result;
  std::jthread waiter([&] { result = coordinator.waitTake(ticket, {}); });
  loader.release("terminal");
  waiter.join();
  expect(result && result->state == image_decode::ImageDecodeWaitState::Ready &&
             result->image && result->image->valid(),
         "terminal wait wakes and transfers the ready image");
  expect(!coordinator.isTracked(ticket),
         "taking a terminal ready result removes its consumer ticket");
}

void testTerminalWaitCancellationDoesNotCancelAnotherConsumer() {
  ControlledLoader loader;
  image_decode::ImageDecodeCoordinator coordinator(
      [&](const auto &item) { return loader.load(item); });
  const auto cancelled = coordinator.request(request("shared-terminal"));
  const auto live = coordinator.request(request("shared-terminal"));
  expect(cancelled != 0 && live != 0 &&
             waitUntil([&] { return loader.hasStarted("shared-terminal"); }),
         "duplicate terminal waiters share the same decoder work");

  std::stop_source stop;
  std::optional<image_decode::ImageDecodeWaitResult> cancelledResult;
  std::jthread waiter([&] {
    cancelledResult = coordinator.waitTake(cancelled, stop.get_token());
  });
  stop.request_stop();
  waiter.join();
  expect(cancelledResult &&
             cancelledResult->state == image_decode::ImageDecodeWaitState::Cancelled,
         "stopping one terminal waiter cancels only its ticket");
  loader.release("shared-terminal");
  const auto liveResult = coordinator.waitTake(live, {});
  expect(liveResult.state == image_decode::ImageDecodeWaitState::Ready &&
             liveResult.image,
         "a second consumer still receives the shared decoded image");
}

void testLegacyCancellationDoesNotAccumulateTerminalTickets() {
  image_decode::ImageDecodeCoordinator coordinator(
      [](const auto &) -> std::optional<image_decode::DecodedImageData> {
        return image_decode::DecodedImageData{.width=1, .height=1,
          .rgba=std::make_shared<std::vector<unsigned char>>(4, 0)};
      });
  for (int index = 0; index != 32; ++index) {
    const auto ticket = coordinator.request(request("legacy-" + std::to_string(index)));
    coordinator.cancel(ticket);
  }
  expect(coordinator.terminalTicketCount() == 0,
         "fire-and-forget cancellation leaves no terminal ticket records behind");
}

void testCompletionNotifierPublishesReadyFailureAndDrops() {
  std::atomic<int> notifications = 0;
  image_decode::ImageDecodeCoordinator coordinator(
      [](const auto &item) -> std::optional<image_decode::DecodedImageData> {
        if (item.key == "failed") return std::nullopt;
        return image_decode::DecodedImageData{.width=1, .height=1,
          .rgba=std::make_shared<std::vector<unsigned char>>(4, 0)};
      }, 1, [&] {
        // Notification must run outside the coordinator lock: a consumer may
        // query ready data immediately when signalled.
        (void)coordinator.readyBytes();
        ++notifications;
      });
  const auto ready = coordinator.request(request("ready"));
  expect(waitUntil([&] { return notifications == 1; }),
         "ready decode signals a sleeping presentation owner");
  expect(coordinator.takeReady(ready).has_value(),
         "ready result is published before the notification");
  const auto failed = coordinator.request(request("failed"));
  expect(waitUntil([&] { return notifications == 2; }) && coordinator.hasFailed(failed),
         "failed decode also wakes its consumer");
  coordinator.drop("failed");
  expect(notifications == 3 && !coordinator.isTracked(failed),
         "dropping a tracked request signals retry work");
  coordinator.drop("missing");
  coordinator.dropPrefix("missing");
  coordinator.dropAll();
  expect(notifications == 3, "empty cache drops do not keep presentation awake");

  const auto prefix = coordinator.request(request("prefix:item"));
  expect(waitUntil([&] { return notifications == 4; }), "prefix decode finishes");
  coordinator.dropPrefix("prefix:");
  expect(notifications == 5 && !coordinator.isTracked(prefix),
         "prefix drop publishes invalidated tickets");
  const auto all = coordinator.request(request("all"));
  expect(waitUntil([&] { return notifications == 6; }), "final decode finishes");
  coordinator.dropAll();
  expect(notifications == 7 && !coordinator.isTracked(all),
         "dropAll publishes invalidated tickets");
}

void testDiscardedDecodeDoesNotPublishStaleCompletion() {
  ControlledLoader loader;
  std::atomic<int> notifications = 0;
  image_decode::ImageDecodeCoordinator coordinator(
      [&](const auto &item) { return loader.load(item); }, 1,
      [&] { ++notifications; });
  const auto ticket = coordinator.request(request("dropped"));
  expect(waitUntil([&] { return loader.hasStarted("dropped"); }),
         "controlled decode starts before drop");
  coordinator.dropAll();
  expect(notifications == 1 && !coordinator.isTracked(ticket),
         "dropping in-flight work signals its cancellation");
  loader.release("dropped");
  coordinator.shutdown();
  expect(notifications == 1,
         "discarded worker result cannot publish a stale completion");
}

void testMaintenancePauseDrainsCancelledLoadersAndResumesQueuedWork() {
  ControlledLoader loader;
  image_decode::ImageDecodeCoordinator coordinator(
      [&](const auto &item) { return loader.load(item); }, 1);
  const auto active = coordinator.request(request("active"));
  {
    std::unique_lock lock(loader.mutex);
    loader.cv.wait(lock, [&] { return loader.active == 1; });
  }
  const auto queued = coordinator.request(request("queued"));
  coordinator.cancel(active); // The loader still runs after its last ticket is gone.
  std::promise<void> enteringPause, paused;
  auto pauseEntered = enteringPause.get_future();
  auto pauseFinished = paused.get_future();
  std::jthread pauser([&] {
    enteringPause.set_value();
    coordinator.pauseAndWait();
    paused.set_value();
  });
  pauseEntered.wait();
  expect(pauseFinished.wait_for(std::chrono::milliseconds(20)) ==
             std::future_status::timeout,
         "maintenance waits for a cancelled but still-running loader");
  loader.release("active");
  pauseFinished.wait();
  pauser.join();
  const auto duringPause = coordinator.request(request("during-pause", true));
  coordinator.pauseAndWait(); // Nested owners must each release their pause.
  coordinator.resume();
  expect(loader.callCount("queued") == 0 && loader.callCount("during-pause") == 0,
         "queued and new requests remain deferred while a pause owner exists");
  loader.release("queued");
  loader.release("during-pause");
  coordinator.resume();
  expect(coordinator.waitTake(queued).state == image_decode::ImageDecodeWaitState::Ready &&
             coordinator.waitTake(duringPause).state == image_decode::ImageDecodeWaitState::Ready,
         "resuming maintenance preserves and completes queued artwork requests");
  expect(loader.startOrder() == std::vector<std::string>{"active", "during-pause", "queued"},
         "paused work retains its decode priority after the active loader drains");
}

void testShutdownDrainsPausedQueueWithoutStartingIt() {
  ControlledLoader loader;
  image_decode::ImageDecodeCoordinator coordinator(
      [&](const auto &item) { return loader.load(item); }, 1);
  coordinator.pauseAndWait();
  const auto queued = coordinator.request(request("shutdown-paused"));
  // If shutdown accidentally dequeues paused work, allow it to finish so the
  // observable start count fails instead of leaving the test worker blocked.
  loader.release("shutdown-paused");
  coordinator.shutdown();
  expect(loader.callCount("shutdown-paused") == 0 &&
             coordinator.waitTake(queued).state == image_decode::ImageDecodeWaitState::Stopped,
         "shutdown wakes paused workers without starting queued file reads");
  coordinator.resume();
}

} // namespace

int main() {
  testTwoWorkerLimitAndPriorityOrdering();
  testDuplicateConsumersShareOneDecode();
  testQueuedAndInFlightOrphansAreDiscarded();
  testDropPreventsStaleCompletionFromReplacingNewWork();
  testTerminalWaitIsNotifiedAndTakesReadyImage();
  testTerminalWaitCancellationDoesNotCancelAnotherConsumer();
  testLegacyCancellationDoesNotAccumulateTerminalTickets();
  testCompletionNotifierPublishesReadyFailureAndDrops();
  testDiscardedDecodeDoesNotPublishStaleCompletion();
  testMaintenancePauseDrainsCancelledLoadersAndResumesQueuedWork();
  testShutdownDrainsPausedQueueWithoutStartingIt();
  if (failures != 0) {
    std::cerr << failures << " image decode coordinator test(s) failed\n";
    return 1;
  }
  std::cout << "Image decode coordinator tests passed\n";
  return 0;
}
