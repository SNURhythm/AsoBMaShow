#include "skin/beatoraja/SkinProcessResidentMemory.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <string_view>

namespace {

using namespace skin;

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

ProcessResidentMemoryNativeQueryResult successfulSample() noexcept {
  return {.succeeded = true, .complete = true, .residentBytes = 123'456};
}

ProcessResidentMemoryNativeQueryResult zeroSample() noexcept {
  return {.succeeded = true, .complete = true, .residentBytes = 0};
}

ProcessResidentMemoryNativeQueryResult failedSample() noexcept {
  return {.succeeded = false, .complete = true, .residentBytes = 123'456};
}

ProcessResidentMemoryNativeQueryResult shortSample() noexcept {
  return {.succeeded = true, .complete = false, .residentBytes = 123'456};
}

ProcessResidentMemoryNativeQueryResult maximumSample() noexcept {
  return {.succeeded = true,
          .complete = true,
          .residentBytes = std::numeric_limits<std::uint64_t>::max()};
}

void testNativeQueryResultsRemainOptionalAndByteExact() {
  const auto success = currentProcessResidentBytes(successfulSample);
  expect(success && *success == 123'456,
         "a complete successful native result preserves resident bytes");

  const auto zero = currentProcessResidentBytes(zeroSample);
  expect(zero && *zero == 0,
         "a successful zero-byte native result is not confused with failure");

  const auto error = currentProcessResidentBytes(failedSample);
  expect(!error, "a native query error is unavailable rather than zero");

  const auto shortCount = currentProcessResidentBytes(shortSample);
  expect(!shortCount,
         "a short native task-info result is unavailable rather than partial");

  const auto maximum = currentProcessResidentBytes(maximumSample);
  expect(maximum && *maximum == std::numeric_limits<std::uint64_t>::max(),
         "resident bytes are not scaled, clamped, or converted");
}

} // namespace

int main() {
  testNativeQueryResultsRemainOptionalAndByteExact();
  return failures == 0 ? 0 : 1;
}
