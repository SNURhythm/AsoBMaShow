#include "TextLibraryAdapters.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
using text_library_experiment::decode;
using text_library_experiment::validate;

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
  std::vector<char32_t> actual;
  const std::vector<std::pair<std::string, std::vector<char32_t>>> examples{
      {"", {}}, {std::string("A\0B", 3), {U'A', U'\0', U'B'}},
      {"한😀e\xcc\x81\r\n", {U'한', U'😀', U'e', U'\u0301', U'\r', U'\n'}},
      {"\xef\xbb\xbf\xef\xbf\xbf\xf4\x8f\xbf\xbf", {0xfeff, 0xffff, 0x10ffff}}};
  for (const auto &[input, expected] : examples)
    require(validate(input) && decode(input, actual) && actual == expected,
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
      require(!validate(input) && !decode(input, actual) && actual.empty(),
              "malformed UTF-8 accepted or partial output exposed");
      ++boundaryCases;
    }
  }
  std::string encoded;
  std::vector<char32_t> expected;
  std::size_t scalarCount = 0;
  const auto flush = [&] {
    require(validate(encoded) && decode(encoded, actual) && actual == expected,
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
    require(validate(bytes) == valid && decode(bytes, actual) == valid &&
                actual == expected, "random differential UTF-8 mismatch");
  }
  std::cout << "correctness backend=" << text_library_experiment::name
            << " scalars=" << scalarCount << " malformed=" << boundaryCases
            << " random=50000";
#if TEXT_LIBRARY_BACKEND == 2
  std::cout << " implementation=" << simdutf::get_active_implementation()->name();
#endif
  std::cout << '\n';
}

struct Case { std::string name; std::string text; };
std::vector<Case> corpus() {
  std::string mixed;
  while (mixed.size() < 256) mixed += "AV한😀 e\xcc\x81";
  std::string file;
  while (file.size() < 65536) file += "제목: 青空の向こう側 - piano mix 😀\n";
  auto badHead = file; badHead.front() = static_cast<char>(0xff);
  auto badTail = file; badTail.back() = static_cast<char>(0xff);
  return {{"ascii_8", "SCORE001"}, {"ascii_32", std::string(32, 'A')},
          {"korean_title", "별빛 아래에서 - Another"},
          {"japanese_title", "青空の向こう側 - piano mix"},
          {"mixed_256", mixed}, {"file_64k", file},
          {"bad_head_64k", badHead}, {"bad_tail_64k", badTail},
          {"truncated_32", std::string(31, 'A') + "\xe2"}};
}

void benchmark() {
  for (const auto &item : corpus()) {
    const auto iterations = std::clamp<std::size_t>(
        2 * 1024 * 1024 / item.text.size(), 512, 250000);
    for (const bool decoding : {false, true}) {
      std::vector<char32_t> output;
      output.reserve(item.text.size());
      std::uint64_t checksum = 0;
      for (int warm = 0; warm < 100; ++warm) decode(item.text, output);
      const auto start = std::chrono::steady_clock::now();
      for (std::size_t index = 0; index < iterations; ++index) {
        // Prevent loop-invariant validation or conversion from being hoisted.
        std::atomic_signal_fence(std::memory_order_seq_cst);
        __asm__ __volatile__("" : : "g"(item.text.data()) : "memory");
        if (decoding) {
          checksum += decode(item.text, output);
          checksum += output.size();
          if (!output.empty()) checksum += output.back();
          __asm__ __volatile__("" : : "g"(output.data()) : "memory");
        } else {
          checksum += validate(item.text);
        }
      }
      const auto nanos = std::chrono::duration<double, std::nano>(
          std::chrono::steady_clock::now() - start).count() / iterations;
      std::cout << "operation backend=" << text_library_experiment::name
                << " case=" << item.name << " bytes=" << item.text.size()
                << " kind=" << (decoding ? "decode" : "validate")
                << " ns=" << nanos << " checksum=" << checksum << '\n';
    }
  }
}
} // namespace

int main(int argc, char **argv) {
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--benchmark") benchmark();
    else checkCorrectness();
  } catch (const std::exception &error) {
    std::cerr << text_library_experiment::name << ": " << error.what() << '\n';
    return 1;
  }
}
