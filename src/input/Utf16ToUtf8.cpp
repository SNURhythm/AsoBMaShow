#include "Utf16ToUtf8.h"

#include <cstdint>

namespace {

constexpr std::uint32_t kHighSurrogateStart = 0xD800;
constexpr std::uint32_t kHighSurrogateEnd = 0xDBFF;
constexpr std::uint32_t kLowSurrogateStart = 0xDC00;
constexpr std::uint32_t kLowSurrogateEnd = 0xDFFF;
constexpr std::uint32_t kReplacementCharacter = 0xFFFD;

void appendUtf8(std::string &output, std::uint32_t codePoint) {
  if (codePoint <= 0x7F) {
    output.push_back(static_cast<char>(codePoint));
  } else if (codePoint <= 0x7FF) {
    output.push_back(static_cast<char>(0xC0 | (codePoint >> 6U)));
    output.push_back(static_cast<char>(0x80 | (codePoint & 0x3FU)));
  } else if (codePoint <= 0xFFFF) {
    output.push_back(static_cast<char>(0xE0 | (codePoint >> 12U)));
    output.push_back(
        static_cast<char>(0x80 | ((codePoint >> 6U) & 0x3FU)));
    output.push_back(static_cast<char>(0x80 | (codePoint & 0x3FU)));
  } else {
    output.push_back(static_cast<char>(0xF0 | (codePoint >> 18U)));
    output.push_back(
        static_cast<char>(0x80 | ((codePoint >> 12U) & 0x3FU)));
    output.push_back(
        static_cast<char>(0x80 | ((codePoint >> 6U) & 0x3FU)));
    output.push_back(static_cast<char>(0x80 | (codePoint & 0x3FU)));
  }
}

} // namespace

std::string utf16ToUtf8(std::u16string_view value) {
  std::string output;
  output.reserve(value.size());
  for (std::size_t index = 0; index < value.size(); ++index) {
    const std::uint32_t first = value[index];
    std::uint32_t codePoint = first;
    if (first >= kHighSurrogateStart && first <= kHighSurrogateEnd) {
      if (index + 1 < value.size()) {
        const std::uint32_t second = value[index + 1];
        if (second >= kLowSurrogateStart && second <= kLowSurrogateEnd) {
          codePoint = 0x10000U + ((first - kHighSurrogateStart) << 10U) +
                      (second - kLowSurrogateStart);
          ++index;
        } else {
          codePoint = kReplacementCharacter;
        }
      } else {
        codePoint = kReplacementCharacter;
      }
    } else if (first >= kLowSurrogateStart && first <= kLowSurrogateEnd) {
      codePoint = kReplacementCharacter;
    }
    appendUtf8(output, codePoint);
  }
  return output;
}
