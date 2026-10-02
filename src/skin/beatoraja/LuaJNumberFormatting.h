#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <string>

namespace skin {

// LuaDouble.tojstring narrows non-integral doubles to Float.toString. Use
// decimal digit generation with the pinned Java float stopping boundaries,
// including its narrower interval at powers of two. Binary32 needs at most
// 114 bits for these scaled integers, so no allocating big integer is needed.
inline std::string luaJFloatString(float value) {
  if (std::isnan(value)) return "NaN";
  if (std::isinf(value)) return value < 0 ? "-Infinity" : "Infinity";
  if (value == 0.0F) return std::signbit(value) ? "-0.0" : "0.0";
  const std::string sign = value < 0 ? "-" : "";
  const auto bits = std::bit_cast<std::uint32_t>(std::abs(value));
  const int encodedExponent = static_cast<int>(bits >> 23);
  const std::uint32_t significand =
      (bits & 0x7fffffU) | (encodedExponent == 0 ? 0 : 0x800000U);
  const int significantBits = std::bit_width(significand);
  const int binaryExponent = encodedExponent == 0
                                 ? significantBits - 150
                                 : encodedExponent - 127;
  const int trailingZeros = std::countr_zero(significand);
  const int fractionBits = significantBits - trailingZeros;
  const int tinyBits = std::max(0, fractionBits - binaryExponent - 1);
  std::string digits;
  int decimalExponent = 0;
  if (tinyBits == 0 && binaryExponent <= 62) {
    std::uint64_t integral = static_cast<std::uint64_t>(std::abs(value));
    const int discardedBits = binaryExponent - significantBits - 1;
    std::uint64_t threshold = discardedBits > 0
                                  ? std::uint64_t{1} << discardedBits : 0;
    std::uint64_t unit = 1;
    while (threshold >= 10) {
      threshold /= 10;
      unit *= 10;
      ++decimalExponent;
    }
    if (unit > 1) integral = integral / unit + (integral % unit >= unit / 2);
    digits = std::to_string(integral);
    decimalExponent += static_cast<int>(digits.size()) - 1;
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
  } else {
    using Wide = __uint128_t;
    const auto power5 = [](int exponent) {
      Wide result = 1;
      while (exponent-- > 0) result *= 5;
      return result;
    };
    const double normalized = std::ldexp(static_cast<double>(significand),
                                         1 - significantBits);
    decimalExponent = static_cast<int>(std::floor(
        (normalized - 1.5) * 0.289529654 + 0.176091259 +
        binaryExponent * 0.301029995663981));
    const int numeratorFives = std::max(0, -decimalExponent);
    const int denominatorFives = std::max(0, decimalExponent);
    int numeratorTwos = numeratorFives + tinyBits + binaryExponent;
    int denominatorTwos = denominatorFives + tinyBits;
    int marginTwos = numeratorTwos - significantBits;
    numeratorTwos -= fractionBits - 1;
    const int commonTwos = std::min(numeratorTwos, denominatorTwos);
    numeratorTwos -= commonTwos;
    denominatorTwos -= commonTwos;
    marginTwos -= commonTwos + (fractionBits == 1 ? 1 : 0);
    if (marginTwos < 0) {
      numeratorTwos -= marginTwos;
      denominatorTwos -= marginTwos;
      marginTwos = 0;
    }
    Wide remainder = Wide{significand >> trailingZeros} *
                     power5(numeratorFives) << numeratorTwos;
    const Wide denominator = power5(denominatorFives) << denominatorTwos;
    Wide margin = power5(numeratorFives) << marginTwos;
    const Wide tenDenominator = denominator * 10;
    const auto fiveBits = [&](int exponent) {
      if (exponent == 0) return 0;
      if (exponent >= 27) return exponent * 3;
      auto value = power5(exponent);
      int count = 0;
      while (value != 0) { ++count; value >>= 1; }
      return count;
    };
    const int numeratorBits = fractionBits + numeratorTwos + fiveBits(numeratorFives);
    const int denominatorBits = denominatorTwos + 1 + fiveBits(denominatorFives + 1);
    const int arithmeticBits = std::max(numeratorBits, denominatorBits) < 32 ? 32
                               : std::max(numeratorBits, denominatorBits) < 64 ? 64
                                                                                  : 0;
    const auto signedWord = [arithmeticBits](Wide number) {
      if (arithmeticBits == 32) {
        return static_cast<std::int64_t>(std::bit_cast<std::int32_t>(
            static_cast<std::uint32_t>(number)));
      }
      return std::bit_cast<std::int64_t>(static_cast<std::uint64_t>(number));
    };
    bool low = false;
    bool high = false;
    do {
      const int digit = static_cast<int>(remainder / denominator);
      remainder = (remainder % denominator) * 10;
      margin *= 10;
      if (arithmeticBits != 0) {
        // Java's fast path uses signed words, including overflow in b+m.
        // Preserve that visible last-digit rounding instead of correcting it.
        const auto signedMargin = signedWord(margin);
        if (!digits.empty() && signedMargin <= 0) {
          low = high = true;
        } else {
          low = signedWord(remainder) < signedMargin;
          high = signedWord(remainder + margin) > signedWord(tenDenominator);
        }
      } else {
        low = remainder < margin;
        high = remainder + margin >= tenDenominator;
      }
      if (digits.empty() && digit == 0 && !high) {
        --decimalExponent;
      } else {
        digits.push_back(static_cast<char>('0' + digit));
      }
      if (digits.size() == 1 &&
          (decimalExponent < -3 || decimalExponent >= 8)) {
        low = high = false;
      }
    } while (!low && !high);
    const bool roundUp = high &&
        (!low || remainder * 2 > tenDenominator ||
         (remainder * 2 == tenDenominator && (digits.back() & 1) != 0));
    if (roundUp) {
      auto index = digits.size();
      while (index > 0 && digits[index - 1] == '9') digits[--index] = '0';
      if (index == 0) {
        digits.front() = '1';
        ++decimalExponent;
      } else {
        ++digits[index - 1];
      }
    }
  }
  if (decimalExponent >= 0 && decimalExponent < 7) {
    const auto point = static_cast<std::size_t>(decimalExponent + 1);
    if (digits.size() <= point) {
      return sign + digits + std::string(point - digits.size(), '0') + ".0";
    }
    return sign + digits.substr(0, point) + "." + digits.substr(point);
  }
  if (decimalExponent >= -3 && decimalExponent < 0) {
    return sign + "0." + std::string(-decimalExponent - 1, '0') + digits;
  }
  return sign + digits.front() + "." +
         (digits.size() > 1 ? digits.substr(1) : "0") + "E" +
         std::to_string(decimalExponent);
}

} // namespace skin
