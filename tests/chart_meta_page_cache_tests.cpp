#include "../src/repositories/ChartMetaPageCache.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

std::vector<int> records(std::size_t offset, std::size_t limit) {
  std::vector<int> result;
  for (std::size_t index = 0; index < limit; ++index) {
    result.push_back(static_cast<int>(offset + index + 1));
  }
  return result;
}

void testLazyAlignedPagesAndBounds() {
  BoundedPageCache<int> cache;
  expect(cache.size() == 0 && cache.get(0) == 0,
         "An unconfigured cache must return its fallback");
  std::vector<std::pair<std::size_t, std::size_t>> requests;
  cache.reset(259, [&](std::size_t offset, std::size_t limit) {
    requests.emplace_back(offset, limit);
    return records(offset, std::min(limit, 259 - offset));
  });
  expect(cache.size() == 259 && requests.empty(),
         "Reset must retain count without loading records");
  const auto &reader = cache;
  expect(reader.get(128) == 129 && reader.get(255) == 256,
         "Indexed reads must use the correct page-local row");
  expect(requests == std::vector<std::pair<std::size_t, std::size_t>>{{128, 128}},
         "Reads within one page must issue one aligned bounded request");
  expect(reader.get(258) == 259, "The final partial page must remain readable");
  expect(reader.get(259) == 0 &&
             reader.get(std::numeric_limits<std::size_t>::max()) == 0,
         "Out-of-range indexes must return fallback without arithmetic overflow");
  expect(requests.size() == 2 &&
             requests.back() == std::make_pair(std::size_t{256}, std::size_t{128}),
         "Out-of-range reads must not query the loader");
}

void testLruAndResidentReferences() {
  BoundedPageCache<int> cache;
  std::vector<std::size_t> requests;
  cache.reset(1024, [&](std::size_t offset, std::size_t limit) {
    requests.push_back(offset);
    return records(offset, limit);
  });
  const int &first = cache.get(0);
  for (std::size_t index = 128; index < 768; index += 128) {
    expect(cache.get(index) == static_cast<int>(index + 1),
           "Each resident page must expose its requested record");
  }
  expect(first == 1 && &cache.get(0) == &first,
         "Loading other pages and refreshing recency must preserve resident references");
  expect(cache.get(768) == 769 && first == 1,
         "Eviction must preserve the recently touched page reference");
  expect(cache.get(256) == 257 && requests.size() == 7,
         "Eviction must leave the other five cached pages resident");
  expect(cache.get(128) == 129 && requests.size() == 8 && requests.back() == 128,
         "The seventh page must evict the least recently used of six pages");
}

void testEvictionReleasesRecords() {
  BoundedPageCache<std::shared_ptr<int>> cache;
  std::weak_ptr<int> evictedRecord;
  cache.reset(1024, [&](std::size_t offset, std::size_t limit) {
    std::vector<std::shared_ptr<int>> result;
    for (std::size_t index = 0; index < limit; ++index) {
      result.push_back(std::make_shared<int>(static_cast<int>(offset + index)));
    }
    return result;
  });
  evictedRecord = cache.get(0);
  const auto activeRecord = cache.get(1);
  for (std::size_t index = 128; index <= 768; index += 128) {
    (void)cache.get(index);
  }
  expect(evictedRecord.expired(), "Eviction must release ownership of old page records");
  expect(*activeRecord == 1, "A caller-owned record must survive cache eviction");
  std::weak_ptr<int> releasedRecord = cache.get(768);
  cache.releasePages();
  expect(releasedRecord.expired(), "Releasing pages must destroy cached records");
}

void testReleaseResetAndClear() {
  BoundedPageCache<int> cache;
  int loads = 0;
  auto owner = std::make_shared<int>(10);
  std::weak_ptr<int> loaderOwner = owner;
  cache.reset(1, [&, owner](std::size_t, std::size_t) {
    ++loads;
    return std::vector<int>{*owner};
  });
  owner.reset();
  expect(cache.get(0) == 10 && loads == 1, "Configured loader must populate the cache");
  cache.releasePages();
  expect(cache.size() == 1 && cache.get(0) == 10 && loads == 2,
         "Releasing pages must preserve count and loader for subsequent queries");
  cache.reset(1, [](std::size_t, std::size_t) { return std::vector<int>{20}; });
  expect(loaderOwner.expired() && cache.get(0) == 20,
         "Reset must discard old pages and release the previous loader");
  const int &fallback = cache.get(1);
  cache.clear();
  expect(cache.size() == 0 && cache.get(0) == 0 && &cache.get(0) == &fallback,
         "Clear must empty the cache while keeping its fallback reference stable");
  cache.reset(1, {});
  expect(cache.get(0) == 0, "An absent loader must return fallback");
}

void testEmptyAndShortPagesRetry() {
  BoundedPageCache<int> cache;
  int attempts = 0;
  cache.reset(128, [&](std::size_t offset, std::size_t limit) {
    ++attempts;
    if (attempts == 1) {
      return std::vector<int>{};
    }
    if (attempts == 2) {
      return std::vector<int>{1};
    }
    return records(offset, limit);
  });
  expect(cache.get(0) == 0, "An empty result must return fallback");
  expect(cache.get(0) == 1 && attempts == 2,
         "An empty result must not prevent a later successful retry");
  expect(cache.get(127) == 128 && attempts == 3,
         "A missing row in a short page must be retried rather than negatively cached");
  expect(cache.get(0) == 1 && attempts == 3,
         "A repaired page must serve subsequent reads from cache");
  cache.reset(128, [](std::size_t, std::size_t) { return std::vector<int>{1}; });
  expect(cache.get(127) == 0, "An unavailable row in a nonempty page must return fallback");
}

void testExceptionsRetryWithoutEviction() {
  BoundedPageCache<int> cache;
  bool fail = true;
  int attempts = 0;
  cache.reset(1024, [&](std::size_t offset, std::size_t limit) {
    ++attempts;
    if (offset == 768 && fail) {
      throw std::runtime_error("temporary page query failure");
    }
    return records(offset, limit);
  });
  const int &first = cache.get(0);
  for (std::size_t index = 128; index < 768; index += 128) {
    (void)cache.get(index);
  }
  bool threw = false;
  try {
    (void)cache.get(768);
  } catch (const std::runtime_error &) {
    threw = true;
  }
  expect(threw, "Loader exceptions must propagate to the caller");
  expect(first == 1 && cache.get(0) == 1 && attempts == 7,
         "A failed load must not evict existing records");
  fail = false;
  expect(cache.get(768) == 769 && attempts == 8,
         "A failed load must be retryable on the next read");
}

void testConstructorConfiguresLoader() {
  int loads = 0;
  BoundedPageCache<int> cache(1, [&](std::size_t, std::size_t) {
    ++loads;
    return std::vector<int>{42};
  });
  expect(cache.size() == 1 && loads == 0,
         "Construction must configure count without eagerly loading records");
  expect(cache.get(0) == 42 && loads == 1,
         "Construction must install the supplied loader");
}

using ChartMetaRecord = std::shared_ptr<std::string>;

BoundedPageCache<ChartMetaRecord> shortSelectionPageCache() {
  return {128, [firstLoad = true](std::size_t, std::size_t limit) mutable {
    std::vector<ChartMetaRecord> result;
    if (firstLoad) {
      firstLoad = false;
      result.push_back(std::make_shared<std::string>("selected-chart"));
      return result;
    }
    for (std::size_t index = 0; index < limit; ++index) {
      result.push_back(std::make_shared<std::string>(
          "repaired-" + std::to_string(index)));
    }
    return result;
  }};
}

void testSelectionPathSnapshotSurvivesShortPageRepair() {
  auto cache = shortSelectionPageCache();
  std::weak_ptr<std::string> selectionLifetime;
  {
    const ChartMetaRecord record = cache.get(0);
    selectionLifetime = record;
    expect(*cache.get(127) == "repaired-127",
           "A previous-row lookup must successfully repair the short page");
    expect(*cache.get(0) == "repaired-0",
           "The repaired cache must no longer own the original selection");
    expect(!selectionLifetime.expired(),
           "Selection path must own its snapshot across short-page repair");
    expect(*record == "selected-chart",
           "Selection path must retain the original chart after another get");
    cache.releasePages();
    expect(*record == "selected-chart",
           "Selection path snapshot must also survive releasing pages");
  }
  expect(selectionLifetime.expired(),
         "Selection path snapshot must release ownership when the action ends");
}

void testSelectionCallbackSnapshotSurvivesShortPageRepair() {
  auto cache = shortSelectionPageCache();
  std::weak_ptr<std::string> selectionLifetime;
  auto onSelected = [&](const ChartMetaRecord &record, int) {
    const ChartMetaRecord item = record;
    selectionLifetime = item;
    auto refreshVisibleRows = [&] {
      expect(*cache.get(127) == "repaired-127",
             "Rebinding visible rows must successfully repair the short page");
    };
    refreshVisibleRows();
    expect(!selectionLifetime.expired(),
           "Selection callback must own its snapshot across short-page repair");
    expect(*item == "selected-chart",
           "Selection callback must retain its input after reentrant cache access");
    cache.clear();
    expect(*item == "selected-chart",
           "Selection callback snapshot must also survive clearing the cache");
  };
  onSelected(cache.get(0), 0);
  expect(selectionLifetime.expired(),
         "Selection callback must release its snapshot when the callback returns");
}

}

int main() {
  testLazyAlignedPagesAndBounds();
  testLruAndResidentReferences();
  testEvictionReleasesRecords();
  testReleaseResetAndClear();
  testEmptyAndShortPagesRetry();
  testExceptionsRetryWithoutEviction();
  testConstructorConfiguresLoader();
  testSelectionPathSnapshotSurvivesShortPageRepair();
  testSelectionCallbackSnapshotSurvivesShortPageRepair();
  std::cout << "ChartMetaPageCache tests passed\n";
}
