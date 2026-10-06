#include "rendering/UiBatchUploadCache.h"

#include <array>
#include <cstddef>
#include <iostream>
#include <span>
#include <vector>

namespace {
int failures = 0;
void expect(bool value, const char *message) {
  if (!value) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}
}

int main() {
  rendering::UiBatchUploadCache cache;
  std::vector<std::byte> gpuBytes;
  unsigned uploads = 0;
  bool reject = false;
  auto upload = [&](std::span<const std::byte> bytes) {
    ++uploads;
    if (reject) return false;
    gpuBytes.assign(bytes.begin(), bytes.end());
    return true;
  };
  const std::array first{std::byte{1}, std::byte{2}, std::byte{3}};
  const std::array changed{std::byte{1}, std::byte{4}, std::byte{3}};

  expect(cache.uploadIfChanged(first, upload) && uploads == 1,
         "first use uploads content to the consumer");
  expect(cache.uploadIfChanged(first, upload) && uploads == 1,
         "unchanged frames reuse previously uploaded data");
  expect(cache.uploadIfChanged(changed, upload) && uploads == 2 &&
             gpuBytes[1] == std::byte{4},
         "geometry or index byte changes reach the consumer");
  const auto shortened = std::span{changed}.first(2);
  expect(cache.uploadIfChanged(shortened, upload) && uploads == 3 &&
             gpuBytes.size() == 2,
         "different lengths cannot reuse a matching prefix");
  expect(cache.uploadIfChanged(first, upload) && uploads == 4 &&
             gpuBytes == std::vector(first.begin(), first.end()),
         "returning to an earlier scene restores its contents");

  cache.invalidate();
  gpuBytes.clear(); // Replacement GPU handles have no prior uploaded content.
  expect(cache.uploadIfChanged(first, upload) && uploads == 5 &&
             gpuBytes == std::vector(first.begin(), first.end()),
         "buffer replacement forces a full upload even for identical geometry");

  reject = true;
  expect(!cache.uploadIfChanged(changed, upload),
         "failed uploads propagate failure");
  reject = false;
  const auto beforeRetry = uploads;
  expect(cache.uploadIfChanged(changed, upload) && uploads == beforeRetry + 1 &&
             gpuBytes[1] == std::byte{4},
         "a failed copy never marks changed data as uploaded");

  const std::vector<std::byte> large(
      rendering::UiBatchUploadCache::kMaximumRetainedBytes + 1, std::byte{9});
  const auto beforeLarge = uploads;
  expect(cache.uploadIfChanged(large, upload) &&
             cache.uploadIfChanged(large, upload) && uploads == beforeLarge + 2,
         "large batches upload without retaining a second large geometry copy");
  expect(cache.uploadIfChanged(changed, upload) && uploads == beforeLarge + 3 &&
             gpuBytes == std::vector(changed.begin(), changed.end()),
         "an oversized upload invalidates previous small-batch data");

  rendering::UiBatchUploadCache separateBuffer;
  expect(separateBuffer.uploadIfChanged(changed, upload) &&
             uploads == beforeLarge + 4,
         "separate GPU buffer streams never share an upload decision");
  if (failures != 0) return 1;
  std::cout << "UI batch upload cache tests passed\n";
  return 0;
}
