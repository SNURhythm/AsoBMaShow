#include "view/DecodedImageCache.h"

#include <iostream>
#include <string_view>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

image_decode::DecodedImageData image(int width, int height,
                                     unsigned char value = 0) {
  return {.width = width,
          .height = height,
          .rgba = std::make_shared<std::vector<unsigned char>>(
              static_cast<std::size_t>(width * height * 4), value)};
}

void testExactAccountingReplacementAndLruTouch() {
  image_decode::DecodedImageCache cache(24);
  expect(cache.put("a", image(2, 1, 1)) &&
             cache.put("b", image(2, 1, 2)) &&
             cache.put("c", image(2, 1, 3)),
         "three exact eight-byte images fit the budget");
  expect(cache.bytes() == 24 && cache.size() == 3,
         "RGBA bytes are accounted exactly");
  expect(cache.get("a").has_value(), "cache hit touches LRU recency");
  expect(cache.put("d", image(2, 1, 4)), "inserting over budget succeeds");
  expect(cache.contains("a") && !cache.contains("b") &&
             cache.contains("c") && cache.contains("d"),
         "least-recently-used entry is evicted after a touch");
  expect(cache.put("c", image(1, 1, 5)) && cache.bytes() == 20,
         "replacement subtracts the old image before adding the new one");
}

void testPinsAndClearEvictable() {
  image_decode::DecodedImageCache cache(16);
  cache.put("pinned", image(2, 1));
  cache.put("free", image(2, 1));
  expect(cache.pin("pinned"), "existing cache entry can be pinned");
  cache.put("next", image(2, 1));
  expect(cache.contains("pinned") && !cache.contains("free") &&
             cache.contains("next"),
         "budget eviction preserves pinned entries");
  cache.clearEvictable();
  expect(cache.size() == 1 && cache.contains("pinned") && cache.bytes() == 8,
         "memory pressure clears only unpinned entries");
  expect(cache.unpin("pinned"), "pin count can be released");
  cache.clearEvictable();
  expect(cache.size() == 0 && cache.bytes() == 0,
         "released entry becomes evictable");
}

void testSingleOversizedItemAvoidsPermanentThrash() {
  image_decode::DecodedImageCache cache(8);
  expect(cache.put("oversized", image(2, 2)),
         "one valid oversized image is retained");
  expect(cache.size() == 1 && cache.bytes() == 16 &&
             cache.contains("oversized"),
         "oversized singleton may exceed the budget");
  expect(cache.put("small", image(1, 1)),
         "later image can replace the oversized singleton");
  expect(!cache.contains("oversized") && cache.contains("small") &&
             cache.bytes() == 4,
         "LRU restores the configured budget when another image arrives");
  expect(!cache.put("invalid", {}), "invalid decoded images are rejected");
}

} // namespace

int main() {
  testExactAccountingReplacementAndLruTouch();
  testPinsAndClearEvictable();
  testSingleOversizedItemAvoidsPermanentThrash();
  if (failures != 0) {
    std::cerr << failures << " decoded image cache test(s) failed\n";
    return 1;
  }
  std::cout << "Decoded image cache tests passed\n";
  return 0;
}
