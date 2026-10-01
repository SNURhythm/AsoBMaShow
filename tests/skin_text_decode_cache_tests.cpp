#include "skin/beatoraja/SkinTextDecodeCache.h"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

namespace {
int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void testDecodesAndSharesUnchangedText() {
  skin::SkinTextDecodeCache cache;
  const std::string text = std::string("A\0", 2) + "한😀\r\n";
  const auto first = cache.decode(text);
  expect(first && *first == std::vector<char32_t>{U'A', U'\0', U'한', U'😀',
                                                 U'\r', U'\n'},
         "UTF-8 decoding preserves scalars, embedded NUL, and line breaks");
  expect(first && cache.decode(std::string(text)) == first,
         "equal text shares immutable storage without decoding or allocating again");
  const auto changed = cache.decode("V");
  expect(changed && *changed == std::vector<char32_t>{U'V'} && changed != first,
         "changed text does not reuse stale codepoints");
  const auto empty = cache.decode("");
  expect(empty && empty->empty(), "empty text has an empty decoded result");
}

void testMalformedTextNeverBecomesCachedSuccess() {
  skin::SkinTextDecodeCache cache;
  for (const std::string value : {std::string("\xc0\xaf"),
                                  std::string("\xed\xa0\x80"),
                                  std::string("\xf4\x90\x80\x80"),
                                  std::string("\xe2\x82"),
                                  std::string("A\x80"),
                                  std::string("\xe2" "A\xac")}) {
    expect(!cache.decode(value) && !cache.decode(value),
           "malformed UTF-8 remains a miss for the renderer's diagnostic path");
  }
}

void testCollisionsAndClearKeepInFlightTextAlive() {
  // One slot forces collisions regardless of the standard library hash.
  skin::SkinTextDecodeCache cache(1, 16);
  const auto a = cache.decode("A");
  const auto b = cache.decode("B");
  expect(a && b && *a == std::vector<char32_t>{U'A'} &&
             *b == std::vector<char32_t>{U'B'},
         "colliding strings keep distinct immutable in-flight codepoints");
  expect(b && cache.decode("B") == b && cache.decode("A") != a,
         "a collision replaces its slot and full text decides a hit");
  cache.clear();
  expect(a && cache.decode("A") != a && *a == std::vector<char32_t>{U'A'},
         "session reset drops cache entries without invalidating active layouts");
}

void testMalformedReplacementCannotPoisonAReusedSlot() {
  skin::SkinTextDecodeCache cache(1, 16);
  expect(bool(cache.decode("ABC")), "valid text populates the reusable slot");
  expect(!cache.decode("X\xed\xa0\x80"), "invalid replacement is rejected");
  const auto restored = cache.decode("ABC");
  expect(restored && *restored == std::vector<char32_t>{U'A', U'B', U'C'},
         "failed decode cannot leave the old key pointing at partially changed scalars");
}

void testMovesPreserveOwnedAndInFlightValues() {
  skin::SkinTextDecodeCache source(1, 16);
  const auto retained = source.decode("A");
  skin::SkinTextDecodeCache moved(std::move(source));
  expect(retained && moved.decode("A") == retained,
         "move construction preserves cached text");
  skin::SkinTextDecodeCache assigned;
  assigned = std::move(moved);
  expect(retained && assigned.decode("A") == retained,
         "move assignment preserves cached text");
  const auto next = assigned.decode("B");
  expect(next && *next == std::vector<char32_t>{U'B'} && retained &&
             *retained == std::vector<char32_t>{U'A'},
         "eviction after a move keeps in-flight text immutable");
}

void testOversizedTextBypassesCacheWithoutEvictingHotText() {
  skin::SkinTextDecodeCache cache(2, 16);
  const auto shortText = cache.decode("hot");
  skin::SkinTextDecodeCache boundaryCache(1, 16);
  const auto boundary = boundaryCache.decode(std::string(16, 'A'));
  expect(boundary && boundary->size() == 16, "text at the byte limit is cacheable");
  expect(!cache.decode(std::string(17, 'A')) &&
             shortText && cache.decode("hot") == shortText,
         "oversized text bypasses storage and leaves hot entries resident");
  skin::SkinTextDecodeCache disabled(0, 16);
  expect(!disabled.decode("A"), "zero capacity bypasses the cache");
}
} // namespace

int main() {
  testDecodesAndSharesUnchangedText();
  testMalformedTextNeverBecomesCachedSuccess();
  testCollisionsAndClearKeepInFlightTextAlive();
  testMalformedReplacementCannotPoisonAReusedSlot();
  testMovesPreserveOwnedAndInFlightValues();
  testOversizedTextBypassesCacheWithoutEvictingHotText();
  return failures == 0 ? 0 : 1;
}
