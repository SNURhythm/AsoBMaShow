#pragma once

#include "LuaJNumberFormatting.h"
#include "LuaSkinRuntime.h"

#include <cmath>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>

namespace skin {

inline std::optional<double> luaJStringNumberValue(std::string_view text) {
  // LuaString.scannumber trims ASCII spaces only. Its integer scan allows
  // a minus after the optional hex prefix and falls back to decimal parsing
  // only for base ten. Keep this separate from binding-source safe subsets.
  while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
  while (!text.empty() && text.back() == ' ') text.remove_suffix(1);
  if (text.empty()) return std::nullopt;
  const bool hex = text.size() >= 2 && text[0] == '0' &&
                   (text[1] == 'x' || text[1] == 'X');
  const int base = hex ? 16 : 10;
  std::string_view digits = hex ? text.substr(2) : text;
  if (digits.empty()) return std::nullopt;
  const bool negative = !digits.empty() && digits.front() == '-';
  if (negative) digits.remove_prefix(1);
  std::uint64_t accumulated = 0;
  bool integer = true;
  for (const unsigned char character : digits) {
    // LuaJ subtracts the lowercase offset from every remaining hex byte,
    // including the punctuation between 'Z' and 'a' (digit aliases 4..9).
    const int digit = base <= 10 || (character >= '0' && character <= '9')
                          ? character - '0'
                      : character >= 'A' && character <= 'Z' ? character - 'A' + 10
                      : character - 'a' + 10;
    if (digit < 0 || digit >= base) {
      integer = false;
      break;
    }
    // The pinned Java scanner wraps long arithmetic and rejects negative
    // intermediate results, rather than checking unsigned multiplication.
    accumulated = accumulated * static_cast<unsigned>(base) + digit;
    if (accumulated > static_cast<std::uint64_t>(
                          std::numeric_limits<std::int64_t>::max())) {
      integer = false;
      break;
    }
  }
  if (integer) {
    const double value = static_cast<double>(accumulated);
    return negative ? -value : value;
  }
  if (hex) return std::nullopt;

  // LuaString.scandouble examines at most 64 bytes, including its syntax
  // check; trailing bytes beyond that prefix are intentionally ignored.
  text = text.substr(0, 64);
  for (const char character : text) {
    if (!((character >= '0' && character <= '9') || character == '+' ||
          character == '-' || character == '.' || character == 'e' ||
          character == 'E')) {
      return std::nullopt;
    }
  }
  char decimal[65]{};
  std::memcpy(decimal, text.data(), text.size());
  char *end = nullptr;
  const double value = std::strtod(decimal, &end);
  if (end != decimal + text.size()) return std::nullopt;
  return value;
}

inline double luaJStringNumber(std::string_view text) {
  return luaJStringNumberValue(text).value_or(0);
}

inline std::int64_t luaJToLong(double value) noexcept {
  if (std::isnan(value)) return 0;
  if (value >=
      static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
    return std::numeric_limits<std::int64_t>::max();
  }
  if (value < static_cast<double>(std::numeric_limits<std::int64_t>::min())) {
    return std::numeric_limits<std::int64_t>::min();
  }
  return static_cast<std::int64_t>(value);
}

inline int luaJToInt(std::int64_t value) noexcept {
  const std::uint32_t lowBits = static_cast<std::uint32_t>(value);
  if (lowBits <= static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
    return static_cast<int>(lowBits);
  }
  return static_cast<int>(static_cast<std::int64_t>(lowBits) -
                          (std::int64_t{1} << 32U));
}

inline int luaJToInt(double value) noexcept {
  return luaJToInt(luaJToLong(value));
}

inline bool luaJToBoolean(const LuaScalar &value) noexcept {
  if (const auto *boolean = std::get_if<bool>(&value)) return *boolean;
  return !std::holds_alternative<std::nullptr_t>(value);
}

inline double luaJToNumber(const LuaScalar &value) noexcept {
  if (const auto *integer = std::get_if<std::int64_t>(&value)) {
    return static_cast<double>(*integer);
  }
  if (const auto *floating = std::get_if<double>(&value)) return *floating;
  if (const auto *text = std::get_if<std::string>(&value)) {
    return luaJStringNumber(*text);
  }
  return 0.0;
}

inline int luaJToInt(const LuaScalar &value) noexcept {
  if (const auto *integer = std::get_if<std::int64_t>(&value)) {
    return luaJToInt(*integer);
  }
  return luaJToInt(luaJToNumber(value));
}

inline std::int64_t luaJToLong(const LuaScalar &value) noexcept {
  if (const auto *integer = std::get_if<std::int64_t>(&value)) return *integer;
  return luaJToLong(luaJToNumber(value));
}

inline double luaJToFloat(const LuaScalar &value) noexcept {
  return static_cast<double>(static_cast<float>(luaJToNumber(value)));
}

inline std::string luaJToString(const LuaScalar &value) {
  if (const auto *text = std::get_if<std::string>(&value)) return *text;
  if (std::holds_alternative<std::nullptr_t>(value)) return "nil";
  if (const auto *boolean = std::get_if<bool>(&value)) {
    return *boolean ? "true" : "false";
  }
  if (const auto *integer = std::get_if<std::int64_t>(&value)) {
    return std::to_string(*integer);
  }
  const double number = std::get<double>(value);
  const auto integer = luaJToLong(number);
  if (static_cast<double>(integer) == number) return std::to_string(integer);
  if (std::isnan(number)) return "nan";
  if (std::isinf(number)) return number < 0 ? "-inf" : "inf";
  return luaJFloatString(static_cast<float>(number));
}

} // namespace skin
