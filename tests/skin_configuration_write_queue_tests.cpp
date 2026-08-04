#include "skin/SkinConfigurationWriteQueue.h"

#include <atomic>
#include <cstdint>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace skin;

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

SkinConfigurationWriteRequest requestWithFrame(std::uint64_t frameSerial) {
  SkinConfigurationWriteRequest request;
  request.sessionSerial = 12;
  request.profileId = {.opaque = "profile-12"};
  request.entry = {.package = {.directoryName = "queue-package",
                               .collisionKey = "queue-package"},
                   .packageRelativePath = "skin/play.luaskin",
                   .collisionKey = "skin/play.luaskin"};
  request.expectedRevisionDigest = "revision-12";
  request.expectedConfigurationDigest = "configuration-12";
  request.frameSerial = frameSerial;
  return request;
}

void testEnqueueDeepOwnsBatchesAndPreservesOrder() {
  SkinConfigurationWriteQueue queue;
  auto first = requestWithFrame(101);
  first.orderedWrites.emplace_back(SetSkinOption{.key = "first", .value = 7});
  first.orderedWrites.emplace_back(SetSkinFilePath{.key = "second",
                                                   .declaredValue = "two.png"});
  auto second = requestWithFrame(102);
  second.orderedWrites.emplace_back(SetSkinOffset{.key = "third",
                                                   .value = {.x = 3, .y = 4}});

  expect(queue.enqueue(first) == SkinConfigurationEnqueueResult::Enqueued,
         "the first write batch enqueues");
  first.expectedRevisionDigest = "changed-after-enqueue";
  std::get<SetSkinOption>(first.orderedWrites.front()).value = 99;
  expect(queue.enqueue(std::move(second)) ==
             SkinConfigurationEnqueueResult::Enqueued,
         "the second write batch enqueues");

  const auto drained = queue.drain();
  expect(drained.size() == 2 && drained[0].frameSerial == 101 &&
             drained[1].frameSerial == 102,
         "drain preserves request FIFO order");
  expect(drained.size() >= 1 && drained[0].expectedRevisionDigest == "revision-12",
         "enqueued requests are deep-owned independently of caller mutation");
  expect(drained.size() >= 1 && drained[0].orderedWrites.size() == 2 &&
             std::get<SetSkinOption>(drained[0].orderedWrites[0]).value == 7 &&
             std::get<SetSkinFilePath>(drained[0].orderedWrites[1]).declaredValue ==
                 "two.png",
         "each queued batch preserves its authored internal write order");
}

void testQueueAcceptsExactlyItsCapacity() {
  SkinConfigurationWriteQueue queue;
  for (std::size_t index = 0; index < SkinConfigurationWriteQueue::maxPending;
       ++index) {
    expect(queue.enqueue(requestWithFrame(index)) ==
               SkinConfigurationEnqueueResult::Enqueued,
           "each request through the exact capacity enqueues");
  }
  expect(queue.enqueue(requestWithFrame(999)) ==
             SkinConfigurationEnqueueResult::QueueFull,
         "the request beyond exact capacity is rejected without displacement");

  const auto drained = queue.drain();
  expect(drained.size() == SkinConfigurationWriteQueue::maxPending &&
             drained.front().frameSerial == 0 && drained.back().frameSerial ==
                                                   SkinConfigurationWriteQueue::maxPending - 1,
         "a full queue retains every accepted request in order");
}

void testDrainReleasesCapacityForReuse() {
  SkinConfigurationWriteQueue queue;
  expect(queue.enqueue(requestWithFrame(1)) == SkinConfigurationEnqueueResult::Enqueued &&
             queue.enqueue(requestWithFrame(2)) ==
                 SkinConfigurationEnqueueResult::Enqueued,
         "initial queue requests enqueue");
  const auto firstDrain = queue.drain();
  expect(firstDrain.size() == 2, "drain returns all queued requests");

  expect(queue.enqueue(requestWithFrame(3)) == SkinConfigurationEnqueueResult::Enqueued,
         "a drained queue accepts a later request");
  const auto secondDrain = queue.drain();
  expect(secondDrain.size() == 1 && secondDrain[0].frameSerial == 3,
         "queue reuse returns only the later request");
}

void testCloseRejectsFutureWritesButRetainsPendingWrites() {
  SkinConfigurationWriteQueue queue;
  expect(queue.enqueue(requestWithFrame(77)) == SkinConfigurationEnqueueResult::Enqueued,
         "a pending request enqueues before close");
  queue.close();
  queue.close();

  expect(queue.enqueue(requestWithFrame(78)) == SkinConfigurationEnqueueResult::Closed,
         "close is idempotent and rejects every later write");
  const auto drained = queue.drain();
  expect(drained.size() == 1 && drained[0].frameSerial == 77,
         "close does not discard writes already accepted");
}

void testConcurrentProducersLoseOrDuplicateNoAcceptedRequests() {
  SkinConfigurationWriteQueue queue;
  constexpr std::uint64_t producerCount = 4;
  constexpr std::uint64_t requestsPerProducer = 48;
  std::atomic<std::uint64_t> ready{0};
  std::atomic<bool> start{false};
  std::atomic<bool> rejected{false};
  std::vector<std::thread> producers;
  producers.reserve(producerCount);

  for (std::uint64_t producer = 0; producer < producerCount; ++producer) {
    producers.emplace_back([&, producer] {
      ready.fetch_add(1, std::memory_order_release);
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      for (std::uint64_t index = 0; index < requestsPerProducer; ++index) {
        if (queue.enqueue(requestWithFrame(producer * 1'000 + index)) !=
            SkinConfigurationEnqueueResult::Enqueued) {
          rejected.store(true, std::memory_order_relaxed);
        }
      }
    });
  }
  while (ready.load(std::memory_order_acquire) != producerCount) {
    std::this_thread::yield();
  }
  start.store(true, std::memory_order_release);
  for (auto &producer : producers) {
    producer.join();
  }

  const auto drained = queue.drain();
  std::set<std::uint64_t> frameSerials;
  for (const auto &request : drained) {
    frameSerials.insert(request.frameSerial);
  }
  expect(!rejected.load(std::memory_order_relaxed) &&
             drained.size() == producerCount * requestsPerProducer &&
             frameSerials.size() == producerCount * requestsPerProducer,
         "concurrent producers retain each accepted request exactly once");
  for (std::uint64_t producer = 0; producer < producerCount; ++producer) {
    for (std::uint64_t index = 0; index < requestsPerProducer; ++index) {
      expect(frameSerials.contains(producer * 1'000 + index),
             "concurrent producer request is present without relying on scheduling order");
    }
  }
}

} // namespace

int main() {
  testEnqueueDeepOwnsBatchesAndPreservesOrder();
  testQueueAcceptsExactlyItsCapacity();
  testDrainReleasesCapacityForReuse();
  testCloseRejectsFutureWritesButRetainsPendingWrites();
  testConcurrentProducersLoseOrDuplicateNoAcceptedRequests();
  return failures == 0 ? 0 : 1;
}
