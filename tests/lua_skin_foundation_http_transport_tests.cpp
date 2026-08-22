#include "skin/beatoraja/LuaSkinFoundationHttpTransportTest.h"

#include <cstddef>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

} // namespace

int main() {
  const skin::LuaSkinHttpLimits limits{.maximumLines = 2,
                                       .maximumCharacters = 4};
  const std::vector<std::byte> oversized(1024 * 1024, std::byte{'a'});
  const auto result = skin::probeLuaSkinFoundationAppend(limits, oversized);
  const std::size_t hardEncodedCeiling =
      limits.maximumCharacters * 3 + limits.maximumLines * 2;
  expect(!result.continued && result.tooLarge,
         "an oversized single Foundation delegate chunk cancels immediately");
  expect(result.storedBytes <= hardEncodedCeiling,
         "Foundation scans before append and retains only a bounded prefix");
  if (failures != 0) {
    std::cerr << failures << " Foundation HTTP transport test(s) failed\n";
    return 1;
  }
  std::cout << "Foundation HTTP transport tests passed\n";
  return 0;
}
