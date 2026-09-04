#include "skin/beatoraja/SkinDecodeCache.h"

#include <iostream>
#include <memory>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void expect(bool value, std::string_view message) {
  if (!value) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

image_decode::DecodedImageData decodedImage() {
  return {.width = 2,
          .height = 1,
          .rgba = std::make_shared<std::vector<unsigned char>>(8, 0)};
}

void testDecodeCacheKeepsEntriesByRevisionAndEvictsOnChange() {
  skin::SkinDecodeCache cache;
  auto &entryA = cache.mutableEntry("aaaa");
  entryA.fontPages.emplace("font/page1.cim", decodedImage());
  auto &entryB = cache.mutableEntry("bbbb");
  expect(entryA.fontPages.size() == 1,
         "cache retains the first revision entry after a second revision");
  expect(cache.entry("aaaa") == &entryA &&
             cache.entry("bbbb") == &entryB,
         "entry lookup returns the same entry for the same revision");
  cache.dropAll();
  expect(cache.entry("aaaa") == nullptr,
         "dropAll clears every revision entry");
}

} // namespace

int main() {
  testDecodeCacheKeepsEntriesByRevisionAndEvictsOnChange();
  if (failures) return 1;
  std::cout << "Skin decode cache tests passed\n";
  return 0;
}