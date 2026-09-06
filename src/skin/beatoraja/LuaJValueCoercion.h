#pragma once

#include "LuaSkinRuntime.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>

namespace skin {

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
    char *end = nullptr;
    const double parsed = std::strtod(text->c_str(), &end);
    if (end != text->c_str()) return parsed;
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

} // namespace skin
