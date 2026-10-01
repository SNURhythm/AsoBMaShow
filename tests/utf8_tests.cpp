#include "text/Utf8.h"
#include <utf8proc.h>

#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
using asobmashow::text::decodeUtf8;
using asobmashow::text::validUtf8;

void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

// Independent encoder for the exhaustive scalar corpus.
void appendScalar(std::string &output, char32_t scalar) {
  if (scalar < 0x80) output.push_back(static_cast<char>(scalar));
  else {
    const int count = scalar < 0x800 ? 2 : scalar < 0x10000 ? 3 : 4;
    const unsigned prefix = count == 2 ? 0xc0 : count == 3 ? 0xe0 : 0xf0;
    output.push_back(static_cast<char>(prefix | (scalar >> (6 * (count - 1)))));
    for (int part = count - 2; part >= 0; --part)
      output.push_back(static_cast<char>(0x80 | ((scalar >> (6 * part)) & 0x3f)));
  }
}

bool referenceDecode(std::string_view input, std::vector<char32_t> &output) {
  output.clear();
  while (!input.empty()) {
    utf8proc_int32_t scalar = 0;
    const auto consumed = utf8proc_iterate(
        reinterpret_cast<const utf8proc_uint8_t *>(input.data()),
        static_cast<utf8proc_ssize_t>(input.size()), &scalar);
    if (consumed <= 0) { output.clear(); return false; }
    output.push_back(static_cast<char32_t>(scalar));
    input.remove_prefix(static_cast<std::size_t>(consumed));
  }
  return true;
}

void checkCorrectness() {
  std::vector<char32_t> actual{U'X'};
  require(validUtf8({}) && decodeUtf8({}, actual) && actual.empty(),
          "default string_view is empty valid UTF-8 and clears prior output");
  const std::vector<std::pair<std::string, std::vector<char32_t>>> examples{
      {"", {}}, {std::string("A\0B", 3), {U'A', U'\0', U'B'}},
      {"한😀e\xcc\x81\r\n", {U'한', U'😀', U'e', U'\u0301', U'\r', U'\n'}},
      {"\xef\xbb\xbf\xef\xbf\xbf\xf4\x8f\xbf\xbf", {0xfeff, 0xffff, 0x10ffff}}};
  for (const auto &[input, expected] : examples)
    require(validUtf8(input) && decodeUtf8(input, actual) && actual == expected,
            "fixed Unicode example mismatch");
  const std::vector<std::string> malformed{
      "\x80", "\xc0\x80", "\xc1\xbf", "\xc2", "\xe0\x80\xaf",
      "\xed\xa0\x80", "\xed\xbf\xbf", "\xf0\x80\x80\x80",
      "\xf4\x90\x80\x80", "\xf5\x80\x80\x80", "\xff",
      "\xe2\x82", "\xf0\x9f\x98", "\xe2" "A\xac"};
  std::size_t boundaryCases = 0;
  for (std::size_t prefix = 0; prefix <= 256; ++prefix) {
    for (const auto &bad : malformed) {
      const auto input = std::string(prefix, 'A') + bad;
      require(!validUtf8(input) && !decodeUtf8(input, actual) && actual.empty(),
              "malformed UTF-8 accepted or partial output exposed");
      ++boundaryCases;
    }
  }
  std::string encoded;
  std::vector<char32_t> expected;
  std::size_t scalarCount = 0;
  const auto flush = [&] {
    require(validUtf8(encoded) && decodeUtf8(encoded, actual) && actual == expected,
            "exhaustive scalar round-trip mismatch");
    encoded.clear(); expected.clear();
  };
  for (char32_t scalar = 0; scalar <= 0x10ffff; ++scalar) {
    if (scalar >= 0xd800 && scalar <= 0xdfff) continue;
    appendScalar(encoded, scalar); expected.push_back(scalar); ++scalarCount;
    if (expected.size() == 257) flush();
  }
  flush();
  std::mt19937 rng(0x54455854);
  for (int trial = 0; trial < 50000; ++trial) {
    std::string bytes(rng() % 97, '\0');
    for (auto &byte : bytes) byte = static_cast<char>(rng() & 0xff);
    const bool valid = referenceDecode(bytes, expected);
    require(validUtf8(bytes) == valid && decodeUtf8(bytes, actual) == valid &&
                actual == expected, "random differential UTF-8 mismatch");
  }
  std::cout << "UTF-8 correctness"
            << " scalars=" << scalarCount << " malformed=" << boundaryCases
            << " random=50000";
  std::cout << '\n';
}

} // namespace

int main() {
  try {
    checkCorrectness();
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
