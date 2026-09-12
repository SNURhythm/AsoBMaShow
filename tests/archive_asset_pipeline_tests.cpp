#include "audio/ArchiveAssetPipeline.h"
#include "RAII.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

class TestGate {
public:
  bool wait() {
    std::unique_lock lock(mutex);
    return changed.wait_for(lock, std::chrono::seconds(2),
                            [&] { return released; });
  }

  void release() {
    std::lock_guard lock(mutex);
    released = true;
    changed.notify_all();
  }

private:
  std::mutex mutex;
  std::condition_variable changed;
  bool released = false;
};

void testConsumptionOverlapsProductionAndRetainsBudget() {
  std::atomic_bool cancelled = false;
  std::promise<void> consuming;
  std::promise<void> pushing;
  TestGate consumerGate;
  TestGate producerGate;
  std::atomic_int consumed = 0;
  audio::ArchiveAssetPipeline pipeline(2, 8, cancelled,
      [&](archive_file::FileData &&file) {
        require(file.bytes.size() == 8, "consumer owns complete entry bytes");
        if (consumed.fetch_add(1) == 0) {
          consuming.set_value();
          require(consumerGate.wait(), "first consumer is released before timeout");
        }
        return true;
      });
  std::future<bool> producer;
  const auto cleanup = makeScopeExit([&] {
    cancelled = true;
    producerGate.release();
    consumerGate.release();
  });
  require(pipeline.push({.path = "first", .bytes = std::vector<unsigned char>(8)}),
          "first entry accepted");
  require(consuming.get_future().wait_for(std::chrono::seconds(2)) ==
              std::future_status::ready,
          "consumer runs before extraction finishes");
  producer = std::async(std::launch::async, [&] {
    require(producerGate.wait(), "second producer is released before timeout");
    pushing.set_value();
    return pipeline.push({.path = "second", .bytes = std::vector<unsigned char>(8)});
  });
  producerGate.release();
  require(pushing.get_future().wait_for(std::chrono::seconds(2)) ==
              std::future_status::ready,
          "second producer reaches push while the first consumer is gated");
  require(producer.wait_for(std::chrono::milliseconds(100)) ==
              std::future_status::timeout,
          "second push blocks while the first consumer retains the byte budget");
  require(pipeline.inFlightBytes() == 8 && consumed == 1,
          "dequeued bytes remain resident and a second consumer cannot start");
  consumerGate.release();
  require(producer.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
          "consumer completion wakes the blocked producer");
  require(producer.get(),
          "budget is reusable after consumption");
  require(pipeline.finish(), "pipeline drains successfully");
  require(consumed == 2 && pipeline.inFlightBytes() == 0,
          "finish drains entries and releases all encoded bytes");
}

void testCancellationWakesBudgetWaiter() {
  std::atomic_bool cancelled = false;
  std::promise<void> consuming;
  std::promise<void> pushing;
  TestGate consumerGate;
  TestGate producerGate;
  std::atomic_int consumed = 0;
  audio::ArchiveAssetPipeline pipeline(1, 4, cancelled,
      [&](archive_file::FileData &&) {
        if (consumed.fetch_add(1) == 0) {
          consuming.set_value();
          require(consumerGate.wait(), "active consumer is released before timeout");
        }
        return true;
      });
  std::future<bool> producer;
  const auto cleanup = makeScopeExit([&] {
    cancelled = true;
    producerGate.release();
    consumerGate.release();
  });
  require(pipeline.push({.path = "active", .bytes = std::vector<unsigned char>(4)}),
          "one entry fills the pipeline budget");
  require(consuming.get_future().wait_for(std::chrono::seconds(2)) ==
              std::future_status::ready,
          "active entry reaches the consumer before timeout");
  producer = std::async(std::launch::async, [&] {
    require(producerGate.wait(), "blocked producer is released before timeout");
    pushing.set_value();
    return pipeline.push({.path = "blocked", .bytes = {1}});
  });
  producerGate.release();
  require(pushing.get_future().wait_for(std::chrono::seconds(2)) ==
              std::future_status::ready,
          "producer reaches push before cancellation");
  require(producer.wait_for(std::chrono::milliseconds(100)) ==
              std::future_status::timeout,
          "resident entry blocks the next push before cancellation");
  cancelled = true;
  require(producer.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
          "cancellation wakes a producer before the consumer is released");
  require(!producer.get(), "cancelled producer does not transfer another buffer");
  require(pipeline.inFlightBytes() == 4 && consumed == 1,
          "cancellation does not release bytes still owned by the active consumer");
  consumerGate.release();
  require(!pipeline.finish(), "cancelled pipeline does not report completion");
  require(pipeline.inFlightBytes() == 0 && consumed == 1,
          "cancelled pipeline releases bytes without consuming the rejected entry");
}

void testOversizedCapacityIsRejectedWithoutWaitingOrMoving(bool occupied) {
  std::atomic_bool cancelled = false;
  std::atomic_uint consumed = 0;
  TestGate consumerGate;
  std::promise<void> consuming;
  audio::ArchiveAssetPipeline pipeline(1, 8, cancelled,
      [&](archive_file::FileData &&) {
        ++consumed;
        if (occupied) {
          consuming.set_value();
          require(consumerGate.wait(), "active consumer is released");
        }
        return true;
      });
  archive_file::FileData oversized{.path = "oversized", .bytes = {42}};
  oversized.bytes.reserve(9);
  const auto *storage = oversized.bytes.data();
  std::future<bool> producer;
  const auto cleanup = makeScopeExit([&] {
    cancelled = true;
    consumerGate.release();
  });
  if (occupied) {
    require(pipeline.push({.path = "active", .bytes = std::vector<unsigned char>(8)}),
            "active entry fills capacity");
    require(consuming.get_future().wait_for(std::chrono::seconds(2)) ==
                std::future_status::ready, "active entry reaches consumer");
  }
  producer = std::async(std::launch::async, [&] {
    return pipeline.push(std::move(oversized));
  });
  require(producer.wait_for(std::chrono::milliseconds(100)) ==
              std::future_status::ready, "oversized push rejects without waiting for capacity");
  require(!producer.get(), "oversized capacity is never accepted even when empty");
  require(oversized.bytes.data() == storage && oversized.bytes.size() == 1,
          "rejected buffer remains owned by producer without copying or moving");
  consumerGate.release();
  require(pipeline.finish(), "capacity rejection leaves valid entries drainable");
  require(consumed == (occupied ? 1U : 0U) && pipeline.inFlightBytes() == 0,
          "oversized entry never reaches consumer or resident accounting");
}

void testAcceptedBufferIsMovedWithoutCopy() {
  std::atomic_bool cancelled = false;
  archive_file::FileData file{.path = "exact", .bytes = std::vector<unsigned char>(8)};
  const auto *storage = file.bytes.data();
  audio::ArchiveAssetPipeline pipeline(1, 8, cancelled,
      [&](archive_file::FileData &&delivered) {
        require(delivered.bytes.data() == storage, "pipeline transfers original allocation");
        return true;
      });
  require(pipeline.push(std::move(file)), "exact budget entry is accepted");
  require(pipeline.finish(), "exact budget entry drains");
}

void testCancellationDiscardsQueuedEntry() {
  std::atomic_bool cancelled = false;
  std::promise<void> consuming;
  TestGate consumerGate;
  std::atomic_int consumed = 0;
  audio::ArchiveAssetPipeline pipeline(1, 8, cancelled,
      [&](archive_file::FileData &&) {
        if (consumed.fetch_add(1) == 0) {
          consuming.set_value();
          require(consumerGate.wait(), "active consumer is released before timeout");
        }
        return true;
      });
  const auto cleanup = makeScopeExit([&] {
    cancelled = true;
    consumerGate.release();
  });
  require(pipeline.push({.path = "active", .bytes = std::vector<unsigned char>(4)}),
          "active entry is accepted");
  require(consuming.get_future().wait_for(std::chrono::seconds(2)) ==
              std::future_status::ready,
          "active consumer starts before timeout");
  require(pipeline.push({.path = "queued", .bytes = std::vector<unsigned char>(4)}),
          "second entry fits the remaining budget and waits for the sole consumer");
  require(pipeline.inFlightBytes() == 8 && consumed == 1,
          "both active and queued buffers are charged before cancellation");
  cancelled = true;
  consumerGate.release();
  require(!pipeline.finish(), "cancellation prevents successful completion");
  require(consumed == 1 && pipeline.inFlightBytes() == 0,
          "queued entry is discarded without a callback and all bytes are released");
}

void testConsumerRejectionDiscardsQueueAndWakesProducer() {
  std::atomic_bool cancelled = false;
  std::promise<void> consuming;
  std::promise<void> pushing;
  TestGate consumerGate;
  TestGate producerGate;
  std::atomic_int consumed = 0;
  audio::ArchiveAssetPipeline pipeline(1, 8, cancelled,
      [&](archive_file::FileData &&) {
        if (consumed.fetch_add(1) == 0) {
          consuming.set_value();
          require(consumerGate.wait(), "rejecting consumer is released before timeout");
        }
        return false;
      });
  std::future<bool> producer;
  const auto cleanup = makeScopeExit([&] {
    cancelled = true;
    producerGate.release();
    consumerGate.release();
  });
  require(pipeline.push({.path = "rejected", .bytes = std::vector<unsigned char>(4)}),
          "entry reaches the rejecting consumer");
  require(consuming.get_future().wait_for(std::chrono::seconds(2)) ==
              std::future_status::ready,
          "rejecting consumer starts before timeout");
  require(pipeline.push({.path = "queued", .bytes = std::vector<unsigned char>(4)}),
          "one entry queues behind the rejecting consumer");
  producer = std::async(std::launch::async, [&] {
    require(producerGate.wait(), "waiting producer is released before timeout");
    pushing.set_value();
    return pipeline.push({.path = "blocked", .bytes = {1}});
  });
  producerGate.release();
  require(pushing.get_future().wait_for(std::chrono::seconds(2)) ==
              std::future_status::ready,
          "producer reaches the full queue before rejection");
  require(producer.wait_for(std::chrono::milliseconds(100)) ==
              std::future_status::timeout,
          "producer blocks while the rejecting consumer and queued entry retain bytes");
  require(pipeline.inFlightBytes() == 8 && consumed == 1,
          "rejection begins with one active entry and one queued entry");
  consumerGate.release();
  require(producer.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
          "consumer rejection wakes the blocked producer");
  require(!producer.get(), "consumer rejection rejects the pending push");
  require(!pipeline.finish() && !cancelled,
          "consumer false aborts the pipeline independently of external cancellation");
  require(consumed == 1 && pipeline.inFlightBytes() == 0,
          "consumer rejection discards queued bytes without another callback");
  require(!pipeline.push({.path = "late", .bytes = {1}}),
          "rejected pipeline does not accept subsequent buffers");
}

void testConsumerFailureJoinsWorkersAndPropagates() {
  std::atomic_bool cancelled = false;
  audio::ArchiveAssetPipeline pipeline(2, 16, cancelled,
      [](archive_file::FileData &&) -> bool {
        throw std::runtime_error("decode failure");
      });
  require(pipeline.push({.path = "broken", .bytes = {1, 2}}),
          "throwing consumer receives an accepted buffer");
  bool caught = false;
  try {
    (void)pipeline.finish();
  } catch (const std::runtime_error &error) {
    caught = std::string_view(error.what()) == "decode failure";
  }
  require(caught, "consumer exceptions propagate after worker teardown");
  require(pipeline.inFlightBytes() == 0,
          "consumer exceptions release all resident bytes before propagation");
  require(!pipeline.push({.path = "late", .bytes = {3}}),
          "finished pipeline rejects further buffers");
}

}

int main() {
  try {
    testOversizedCapacityIsRejectedWithoutWaitingOrMoving(false);
    testOversizedCapacityIsRejectedWithoutWaitingOrMoving(true);
    testAcceptedBufferIsMovedWithoutCopy();
    testConsumptionOverlapsProductionAndRetainsBudget();
    testCancellationWakesBudgetWaiter();
    testCancellationDiscardsQueuedEntry();
    testConsumerRejectionDiscardsQueueAndWakesProducer();
    testConsumerFailureJoinsWorkersAndPropagates();
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
