#include "skin/beatoraja/SkinDecodeCache.h"

#include <atomic>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
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
  cache.storeSkinImage("aaaa", "images/page1.cim", decodedImage());
  cache.storeSkinImage("bbbb", "images/page1.cim", decodedImage());
  expect(cache.findSkinImage("aaaa", "images/page1.cim").has_value() &&
             cache.findSkinImage("bbbb", "images/page1.cim").has_value(),
         "cache retains each revision entry independently");
  cache.dropAll();
  expect(!cache.findSkinImage("aaaa", "images/page1.cim").has_value() &&
             !cache.findSkinImage("bbbb", "images/page1.cim").has_value(),
         "dropAll clears every revision entry");
}

void testDecodeCacheRetainsFontPagesAndEncodedByteCharges() {
  skin::SkinDecodeCache cache;
  cache.storeFontPage("aaaa", "font/page1.cim", decodedImage(), 42);
  const auto page = cache.findFontPage("aaaa", "font/page1.cim");
  expect(page.has_value() && page->width == 2 && page->height == 1,
         "font page pixels are retrievable by revision and physical key");
  expect(cache.findFontPageEncodedBytes("aaaa", "font/page1.cim") == 42,
         "font page encoded-byte charge is retrievable for warm budget "
         "reconstruction");
  cache.dropAll();
  expect(!cache.findFontPage("aaaa", "font/page1.cim").has_value() &&
             !cache.findFontPageEncodedBytes("aaaa", "font/page1.cim")
                  .has_value(),
         "dropAll clears cached font pages and their byte charges");
}

void testDecodeCacheConcurrentReadsWritesAndDrops() {
  skin::SkinDecodeCache cache;
  std::thread writer([&] {
    for (int i = 0; i < 20000; ++i) {
      const std::string revision = "rev" + std::to_string(i % 16);
      cache.storeFontPage(revision, "page" + std::to_string(i % 64),
                          decodedImage(),
                          static_cast<std::size_t>(i % 4096) + 1);
      cache.storeSkinImage(revision, "img" + std::to_string(i % 64),
                           decodedImage());
    }
  });
  std::thread reader([&] {
    for (int i = 0; i < 20000; ++i) {
      const std::string revision = "rev" + std::to_string(i % 16);
      const std::string pageKey = "page" + std::to_string(i % 64);
      const auto page = cache.findFontPage(revision, pageKey);
      if (page && (page->rgba == nullptr || page->width != 2 ||
                   page->height != 1)) {
        expect(false, "concurrent reader observed a corrupted font page");
        break;
      }
      const auto image =
          cache.findSkinImage(revision, "img" + std::to_string(i % 64));
      if (image && (image->rgba == nullptr || image->width != 2 ||
                    image->height != 1)) {
        expect(false, "concurrent reader observed a corrupted skin image");
        break;
      }
    }
  });
  std::thread dropper([&] {
    for (int i = 0; i < 500; ++i) {
      cache.dropAll();
    }
  });
  writer.join();
  reader.join();
  dropper.join();
  cache.dropAll();
  expect(!cache.findFontPage("rev0", "page0").has_value() &&
             !cache.findSkinImage("rev0", "img0").has_value() &&
             cache.decodedBytes() == 0,
         "concurrent access leaves the decode cache consistent");
}

} // namespace

int main() {
  testDecodeCacheKeepsEntriesByRevisionAndEvictsOnChange();
  testDecodeCacheRetainsFontPagesAndEncodedByteCharges();
  testDecodeCacheConcurrentReadsWritesAndDrops();
  if (failures) return 1;
  std::cout << "Skin decode cache tests passed\n";
  return 0;
}
