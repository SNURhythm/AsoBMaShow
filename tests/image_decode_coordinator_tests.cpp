#include "view/ImageDecodeCoordinator.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
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
  coordinator.drop("same");
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

} // namespace

int main() {
  testTwoWorkerLimitAndPriorityOrdering();
  testDuplicateConsumersShareOneDecode();
  testQueuedAndInFlightOrphansAreDiscarded();
  testDropPreventsStaleCompletionFromReplacingNewWork();
  if (failures != 0) {
    std::cerr << failures << " image decode coordinator test(s) failed\n";
    return 1;
  }
  std::cout << "Image decode coordinator tests passed\n";
  return 0;
}
