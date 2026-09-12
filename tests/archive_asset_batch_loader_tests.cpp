#include "audio/ArchiveAssetBatchLoader.h"

#include <future>
#include <iostream>
#include <map>
#include <stdexcept>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

const std::vector<std::filesystem::path> paths = {"first", "second", "third", "fourth"};

void testPartialFailureRetriesOnlyMissingEntries(bool cancelAfterDelivery) {
  std::atomic_bool cancelled = false;
  std::promise<void> firstConsumed;
  auto consumed = firstConsumed.get_future();
  std::map<std::filesystem::path, unsigned> deliveries;
  std::mutex mutex;
  unsigned fallbackCalls = 0;
  auto consumer = [&](archive_file::FileData &&file) {
    std::lock_guard lock(mutex);
    ++deliveries[file.path];
    require(file.bytes == std::vector<unsigned char>{42}, "entry bytes reach decoder intact");
    if (file.path == "first") {
      firstConsumed.set_value();
    }
    return true;
  };
  auto concurrent = [&](const auto &, const auto &, auto onFile, auto workers,
                        auto budget, auto *, auto checkpoint) {
    require(workers == 2 && budget == 16, "extractor receives its bounded budget");
    require(checkpoint() && onFile({.path = "first", .bytes = {42}}),
            "first entry delivered before reader failure");
    require(consumed.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
            "entry consumption overlaps incomplete extraction");
    cancelled = cancelAfterDelivery;
    return false;
  };
  auto streaming = [&](const auto &, const auto &missing, auto onFile, auto budget,
                       auto *, auto) {
    require(budget == 16, "fallback retains the extraction memory capacity");
    ++fallbackCalls;
    require(missing == std::vector<std::filesystem::path>{"second", "third", "fourth"},
            "fallback excludes entries already delivered");
    for (const auto &path : missing) {
      require(onFile({.path = path, .bytes = {42}}), "fallback entry accepted");
      require(onFile({.path = path, .bytes = {42}}), "repeated backend delivery is deduplicated");
    }
    return true;
  };
  std::string error;
  const bool loaded = audio::ConsumeArchiveAssetBatch(
      "fixture.zip", paths, 4, 32, consumer, &error, cancelled, concurrent, streaming);
  require(loaded != cancelAfterDelivery, "cancellation determines batch completion");
  require(fallbackCalls == (cancelAfterDelivery ? 0U : 1U),
          "cancelled partial extraction never starts fallback");
  require(deliveries.size() == (cancelAfterDelivery ? 1U : paths.size()),
          "only expected assets are consumed");
  for (const auto &[path, count] : deliveries) {
    (void)path;
    require(count == 1, "each asset is consumed exactly once across fallback");
  }
}

void testSerialOnlyReaderConsumesEveryEntryOnce(std::size_t workers) {
  std::atomic_bool cancelled = false;
  std::atomic_uint consumed = 0;
  const auto caller = std::this_thread::get_id();
  unsigned serialCalls = 0;
  auto concurrent = [](const auto &, const auto &, auto, auto, auto, auto *, auto) -> bool {
    throw std::runtime_error("serial-only operation must not use concurrent reader");
  };
  auto streaming = [&](const auto &, const auto &requested, auto onFile, auto budget,
                       auto *, auto) {
    require(budget == (workers == 1 ? 32 : 16),
            "serial extraction shares total capacity with asynchronous consumption");
    ++serialCalls;
    for (const auto &path : requested) {
      require(onFile({.path = path, .bytes = {42}}), "serial entry accepted");
      require(onFile({.path = path, .bytes = {42}}), "serial repeated delivery accepted once");
    }
    return true;
  };
  require(audio::ConsumeArchiveAssetBatch(
              "fixture.7z", paths, workers, 32,
              [&](archive_file::FileData &&) {
                if (workers == 1) {
                  require(std::this_thread::get_id() == caller,
                          "single-worker budget does not create extra consumer threads");
                }
                ++consumed;
                return true;
              },
              nullptr, cancelled, concurrent, streaming),
          "serial-only archive loads successfully");
  require(serialCalls == 1 && consumed == paths.size(),
          "serial-only extraction stays one pass with complete consumption");
}

void testConsumerFailureDoesNotRestartExtraction() {
  std::atomic_bool cancelled = false;
  std::promise<void> rejected;
  auto rejection = rejected.get_future();
  unsigned fallbackCalls = 0;
  auto consumer = [&](archive_file::FileData &&) {
    rejected.set_value();
    return false;
  };
  auto concurrent = [&](const auto &, const auto &, auto onFile, auto, auto,
                        auto *, auto checkpoint) {
    require(onFile({.path = "first", .bytes = {42}}), "consumer receives first entry");
    require(rejection.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
            "consumer signals rejection");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (checkpoint() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    }
    require(!checkpoint(), "reader observes consumer rejection before its next operation");
    return false;
  };
  auto streaming = [&](const auto &, const auto &, auto, auto, auto *, auto) {
    ++fallbackCalls;
    return false;
  };
  require(!audio::ConsumeArchiveAssetBatch(
              "fixture.zip", paths, 4, 32, consumer, nullptr, cancelled,
              concurrent, streaming), "consumer rejection fails the load");
  require(fallbackCalls == 0, "consumer rejection never restarts extraction");
}

void testInlineConsumptionRejectsIncompleteDeliveryAndConsumerFailure() {
  std::atomic_bool cancelled = false;
  auto concurrent = [](const auto &, const auto &, auto, auto, auto, auto *, auto) -> bool {
    throw std::runtime_error("inline operation does not create extractor threads");
  };
  for (const bool reject : {false, true}) {
    unsigned consumed = 0;
    auto streaming = [&](const auto &, const auto &, auto onFile, auto,
                         auto *, auto checkpoint) {
      const bool accepted = onFile({.path = "first", .bytes = {42}});
      require(accepted != reject, "inline consumer result reaches reader");
      require(checkpoint() != reject, "inline rejection stops subsequent reader work");
      return true;
    };
    require(!audio::ConsumeArchiveAssetBatch(
                "fixture.7z", paths, 1, 32,
                [&](archive_file::FileData &&) { ++consumed; return !reject; },
                nullptr, cancelled, concurrent, streaming),
            "inline batch rejects incomplete delivery or consumer failure");
    require(consumed == 1, "inline path consumes only the delivered entry");
  }
}

void testOversizedDeliveryNeverReachesConsumer(std::size_t workers,
                                             bool singlePath) {
  std::atomic_bool cancelled = false;
  std::atomic_uint consumed = 0;
  const std::vector<std::filesystem::path> requested = singlePath
      ? std::vector<std::filesystem::path>{"first"} : paths;
  auto concurrent = [](const auto &, const auto &, auto, auto, auto, auto *, auto) {
    return false;
  };
  auto streaming = [](const auto &, const auto &entries, auto onFile, auto,
                      auto *, auto) {
    for (const auto &path : entries) {
      archive_file::FileData file{.path = path, .bytes = {42}};
      file.bytes.reserve(9);
      if (!onFile(std::move(file))) {
        return false;
      }
    }
    return true;
  };
  std::string error;
  require(!audio::ConsumeArchiveAssetBatch(
              "oversized.7z", requested, workers, 8,
              [&](archive_file::FileData &&) { ++consumed; return true; },
              &error, cancelled, concurrent, streaming),
          "loader rejects deliveries exceeding the encoded memory capacity");
  require(consumed == 0, "oversized allocation never reaches an audio consumer");
  require(!error.empty(), "memory rejection reports a diagnostic");
}

void testExtractionLimitPreventsOversizedAllocation(std::size_t workers,
                                                   bool singlePath,
                                                   std::uint64_t maximumBytes) {
  std::atomic_bool cancelled = false;
  unsigned consumed = 0;
  unsigned streamingCalls = 0;
  const std::vector<std::filesystem::path> requested = singlePath
      ? std::vector<std::filesystem::path>{"first"} : paths;
  auto concurrent = [&](const auto &, const auto &, auto, auto, auto budget,
                        auto *, auto) {
    require(budget <= maximumBytes / 2,
            "concurrent extraction reserves room for queued and decoding bytes");
    return false;
  };
  auto streaming = [&](const auto &, const auto &, auto onFile, std::uint64_t budget,
                       std::string *error, auto checkpoint) {
    ++streamingCalls;
    require(checkpoint(), "bounded streaming preserves cancellation checkpoint");
    const auto memberBytes = (workers <= 1 || singlePath)
        ? maximumBytes + 1 : maximumBytes / 2 + 1;
    if (memberBytes > budget) {
      *error = "entry exceeds memory limit";
      return false;
    }
    return onFile({.path = "first", .bytes = {42}});
  };
  std::string error;
  require(!audio::ConsumeArchiveAssetBatch(
              "oversized.7z", requested, workers, maximumBytes,
              [&](archive_file::FileData &&) { ++consumed; return true; },
              &error, cancelled, concurrent, streaming),
          "bounded extraction rejects a member before allocating its buffer");
  require(streamingCalls == 1 && consumed == 0 && !error.empty(),
          "oversized member is rejected in one streaming pass without consumption");
}

void testTinyAndOddBudgetsRetainExactLimit(std::uint64_t maximumBytes) {
  std::atomic_bool cancelled = false;
  std::atomic_uint consumed = 0;
  auto concurrent = [](const auto &, const auto &, auto, auto, auto, auto *, auto) {
    return false;
  };
  auto streaming = [&](const auto &, const auto &requested, auto onFile, auto budget,
                       auto *, auto) {
    require(budget == 1, "tiny or odd budget reserves one extraction byte");
    for (const auto &path : requested) {
      if (!onFile({.path = path, .bytes = {42}})) {
        return false;
      }
    }
    return true;
  };
  std::string error;
  const bool loaded = audio::ConsumeArchiveAssetBatch(
      "tiny.7z", paths, 4, maximumBytes,
      [&](archive_file::FileData &&) { ++consumed; return true; },
      &error, cancelled, concurrent, streaming);
  require(loaded == (maximumBytes > 0), "zero budget cannot load nonempty assets");
  require(consumed == (maximumBytes > 0 ? paths.size() : 0),
          "tiny budgets do not round up extraction plus pipeline beyond the total");
}

}

int main() {
  try {
    testOversizedDeliveryNeverReachesConsumer(1, false);
    testOversizedDeliveryNeverReachesConsumer(4, true);
    testOversizedDeliveryNeverReachesConsumer(2, false);
    testOversizedDeliveryNeverReachesConsumer(4, false);
    for (const auto budget : {32ull, 64ull * 1024ull * 1024ull}) {
      testExtractionLimitPreventsOversizedAllocation(1, false, budget);
      testExtractionLimitPreventsOversizedAllocation(4, true, budget);
      testExtractionLimitPreventsOversizedAllocation(2, false, budget);
      testExtractionLimitPreventsOversizedAllocation(4, false, budget);
    }
    testTinyAndOddBudgetsRetainExactLimit(0);
    testTinyAndOddBudgetsRetainExactLimit(1);
    testTinyAndOddBudgetsRetainExactLimit(3);
    testPartialFailureRetriesOnlyMissingEntries(false);
    testPartialFailureRetriesOnlyMissingEntries(true);
    testSerialOnlyReaderConsumesEveryEntryOnce(1);
    testSerialOnlyReaderConsumesEveryEntryOnce(2);
    testConsumerFailureDoesNotRestartExtraction();
    testInlineConsumptionRejectsIncompleteDeliveryAndConsumerFailure();
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
