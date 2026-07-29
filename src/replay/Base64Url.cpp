#include "Base64Url.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace replay {
namespace {

constexpr std::string_view kAlphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

int decodeCharacter(char value) noexcept {
  if (value >= 'A' && value <= 'Z') {
    return value - 'A';
  }
  if (value >= 'a' && value <= 'z') {
    return value - 'a' + 26;
  }
  if (value >= '0' && value <= '9') {
    return value - '0' + 52;
  }
  if (value == '-') {
    return 62;
  }
  if (value == '_') {
    return 63;
  }
  return -1;
}

} // namespace

std::string base64UrlEncode(std::span<const std::byte> input) {
  if (input.empty()) {
    return {};
  }
  if (input.size() > (std::numeric_limits<std::size_t>::max() - 2) / 4 * 3) {
    return {};
  }
  const std::size_t outputSize = ((input.size() + 2) / 3) * 4;
  std::string output;
  output.reserve(outputSize);
  for (std::size_t offset = 0; offset < input.size(); offset += 3) {
    const std::size_t available = input.size() - offset;
    const std::uint32_t first = std::to_integer<unsigned char>(input[offset]);
    const std::uint32_t second =
        available > 1 ? std::to_integer<unsigned char>(input[offset + 1]) : 0;
    const std::uint32_t third =
        available > 2 ? std::to_integer<unsigned char>(input[offset + 2]) : 0;
    const std::uint32_t value = (first << 16U) | (second << 8U) | third;
    output.push_back(kAlphabet[(value >> 18U) & 0x3fU]);
    output.push_back(kAlphabet[(value >> 12U) & 0x3fU]);
    output.push_back(available > 1 ? kAlphabet[(value >> 6U) & 0x3fU] : '=');
    output.push_back(available > 2 ? kAlphabet[value & 0x3fU] : '=');
  }
  return output;
}

std::optional<std::vector<std::byte>>
base64UrlDecodeBounded(std::string_view input, std::size_t maximumOutputBytes,
                       std::string &diagnostic) {
  diagnostic.clear();
  if (input.empty()) {
    return std::vector<std::byte>{};
  }

  const std::size_t firstPadding = input.find('=');
  const std::size_t dataLength =
      firstPadding == std::string_view::npos ? input.size() : firstPadding;
  const std::size_t padding = input.size() - dataLength;
  if (padding > 2 || (padding != 0 && input.size() % 4 != 0) ||
      input.substr(dataLength).find_first_not_of('=') !=
          std::string_view::npos ||
      dataLength % 4 == 1) {
    diagnostic = "Base64URL padding or length is invalid";
    return std::nullopt;
  }
  if ((padding == 1 && dataLength % 4 != 3) ||
      (padding == 2 && dataLength % 4 != 2)) {
    diagnostic = "Base64URL padding does not match its data length";
    return std::nullopt;
  }
  for (std::size_t index = 0; index < dataLength; ++index) {
    if (decodeCharacter(input[index]) < 0) {
      diagnostic = "Base64URL contains an invalid character";
      return std::nullopt;
    }
  }

  const std::size_t remainder = dataLength % 4;
  const std::size_t outputSize = (dataLength / 4) * 3 + (remainder == 2   ? 1
                                                         : remainder == 3 ? 2
                                                                          : 0);
  if (outputSize > maximumOutputBytes) {
    diagnostic = "Base64URL output exceeds the configured limit";
    return std::nullopt;
  }
  if (remainder == 2 && (decodeCharacter(input[dataLength - 1]) & 0x0f) != 0) {
    diagnostic = "Base64URL has non-canonical tail bits";
    return std::nullopt;
  }
  if (remainder == 3 && (decodeCharacter(input[dataLength - 1]) & 0x03) != 0) {
    diagnostic = "Base64URL has non-canonical tail bits";
    return std::nullopt;
  }

  std::vector<std::byte> output;
  output.reserve(outputSize);
  std::size_t offset = 0;
  while (offset < dataLength) {
    const std::size_t available = dataLength - offset;
    std::uint32_t value =
        static_cast<std::uint32_t>(decodeCharacter(input[offset])) << 18U;
    value |= static_cast<std::uint32_t>(decodeCharacter(input[offset + 1]))
             << 12U;
    if (available > 2) {
      value |= static_cast<std::uint32_t>(decodeCharacter(input[offset + 2]))
               << 6U;
    }
    if (available > 3) {
      value |= static_cast<std::uint32_t>(decodeCharacter(input[offset + 3]));
    }
    output.push_back(static_cast<std::byte>((value >> 16U) & 0xffU));
    if (available > 2) {
      output.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
    }
    if (available > 3) {
      output.push_back(static_cast<std::byte>(value & 0xffU));
    }
    offset += std::min<std::size_t>(4, available);
  }
  return output;
}

} // namespace replay
