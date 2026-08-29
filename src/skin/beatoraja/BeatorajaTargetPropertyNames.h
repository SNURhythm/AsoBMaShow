#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace skin {
namespace beatoraja_target_property_detail {

inline std::optional<int> positiveSuffix(std::string_view value,
                                         std::string_view prefix) {
  if (!value.starts_with(prefix) || value.size() == prefix.size()) {
    return std::nullopt;
  }
  std::string_view digits = value.substr(prefix.size());
  if (digits.starts_with('+')) {
    digits.remove_prefix(1);
  }
  int parsed = 0;
  const auto [end, error] =
      std::from_chars(digits.data(), digits.data() + digits.size(), parsed);
  if (error != std::errc{} || end != digits.data() + digits.size() ||
      parsed <= 0) {
    return std::nullopt;
  }
  return parsed;
}

inline std::string javaFloatText(float value) {
  char buffer[64];
  std::string text;
  for (int precision = 1;
       precision <= std::numeric_limits<float>::max_digits10; ++precision) {
    const int written = std::snprintf(buffer, sizeof(buffer), "%.*g", precision,
                                      static_cast<double>(value));
    if (written < 0 || static_cast<std::size_t>(written) >= sizeof(buffer)) {
      return "0.0";
    }
    text.assign(buffer, static_cast<std::size_t>(written));
    char *parseEnd = nullptr;
    const float roundTrip = std::strtof(text.c_str(), &parseEnd);
    if (parseEnd != text.c_str() && parseEnd != nullptr && *parseEnd == '\0' &&
        roundTrip == value) {
      break;
    }
  }
  const float magnitude = std::fabs(value);
  const bool scientific = magnitude != 0.0F &&
                          (magnitude < 0.001F || magnitude >= 10'000'000.0F);
  if (scientific && text.find_first_of("eE") == std::string::npos) {
    const auto dot = text.find('.');
    const std::size_t beforeDot = dot == std::string::npos ? text.size() : dot;
    text.erase(std::remove(text.begin(), text.end(), '.'), text.end());
    const auto firstDigit = text.find_first_not_of('0');
    if (firstDigit == std::string::npos) {
      return "0.0";
    }
    const int exponent = static_cast<int>(beforeDot) -
                         static_cast<int>(firstDigit) - 1;
    text.erase(0, firstDigit);
    text.insert(1, 1, '.');
    text += "E" + std::to_string(exponent);
  }

  const auto marker = text.find_first_of("eE");
  if (marker == std::string::npos) {
    if (text.find('.') == std::string::npos) {
      text += ".0";
    }
    return text;
  }

  std::string mantissa = text.substr(0, marker);
  std::string exponent = text.substr(marker + 1);
  bool negativeExponent = false;
  if (!exponent.empty() && (exponent.front() == '+' || exponent.front() == '-')) {
    negativeExponent = exponent.front() == '-';
    exponent.erase(exponent.begin());
  }
  const auto firstDigit = exponent.find_first_not_of('0');
  exponent = firstDigit == std::string::npos ? "0" : exponent.substr(firstDigit);

  if (scientific) {
    if (mantissa.find('.') == std::string::npos) {
      mantissa += ".0";
    }
    return mantissa + "E" + (negativeExponent ? "-" : "") + exponent;
  }

  int parsedExponent = 0;
  const auto [parsedEnd, parsedError] = std::from_chars(
      exponent.data(), exponent.data() + exponent.size(), parsedExponent);
  if (parsedError != std::errc{} ||
      parsedEnd != exponent.data() + exponent.size()) {
    return mantissa;
  }
  if (negativeExponent) {
    parsedExponent = -parsedExponent;
  }
  const auto dot = mantissa.find('.');
  const std::size_t beforeDot = dot == std::string::npos ? mantissa.size() : dot;
  mantissa.erase(std::remove(mantissa.begin(), mantissa.end(), '.'),
                 mantissa.end());
  const std::ptrdiff_t decimal =
      static_cast<std::ptrdiff_t>(beforeDot) + parsedExponent;
  if (decimal <= 0) {
    return "0." + std::string(static_cast<std::size_t>(-decimal), '0') +
           mantissa;
  }
  if (decimal >= static_cast<std::ptrdiff_t>(mantissa.size())) {
    return mantissa +
           std::string(static_cast<std::size_t>(decimal) - mantissa.size(), '0') +
           ".0";
  }
  mantissa.insert(static_cast<std::size_t>(decimal), 1, '.');
  return mantissa;
}

inline std::optional<std::string> rateName(std::string_view value) {
  std::string source(value);
  source.erase(source.begin(),
               std::find_if(source.begin(), source.end(), [](unsigned char ch) {
                 return ch > 0x20U;
               }));
  source.erase(
      std::find_if(source.rbegin(), source.rend(), [](unsigned char ch) {
        return ch > 0x20U;
      }).base(),
      source.end());
  if (!source.empty() &&
      (source.back() == 'f' || source.back() == 'F' || source.back() == 'd' ||
       source.back() == 'D')) {
    source.pop_back();
  }
  char *end = nullptr;
  const float rate = std::strtof(source.c_str(), &end);
  if (end == source.c_str() || end == nullptr || *end != '\0' ||
      !std::isfinite(rate) || rate < 0.0F || rate > 100.0F) {
    return std::nullopt;
  }
  return "SCORE RATE " + javaFloatText(rate) + "%";
}

} // namespace beatoraja_target_property_detail

// Pinned TargetProperty's displayed names. StringPropertyFactory uses this
// mapping for both gameplay and AbstractResult target-name rings.
inline std::string beatorajaTargetPropertyName(std::string_view id) {
  constexpr std::array<std::pair<std::string_view, std::string_view>, 11>
      staticTargets = {{{"RATE_A-", "RANK A-"}, {"RATE_A", "RANK A"},
                        {"RATE_A+", "RANK A+"}, {"RATE_AA-", "RANK AA-"},
                        {"RATE_AA", "RANK AA"}, {"RATE_AA+", "RANK AA+"},
                        {"RATE_AAA-", "RANK AAA-"},
                        {"RATE_AAA", "RANK AAA"},
                        {"RATE_AAA+", "RANK AAA+"},
                        {"RATE_MAX-", "RANK MAX-"}, {"MAX", "MAX"}}};
  for (const auto &[targetId, name] : staticTargets) {
    if (id == targetId) {
      return std::string(name);
    }
  }
  if (id.starts_with("RATE_")) {
    if (const auto name =
            beatoraja_target_property_detail::rateName(id.substr(5))) {
      return *name;
    }
  }
  if (id == "RANK_NEXT") {
    return "NEXT RANK";
  }
  using beatoraja_target_property_detail::positiveSuffix;
  if (const auto index = positiveSuffix(id, "RIVAL_NEXT_")) {
    return "RIVAL NEXT " + std::to_string(*index);
  }
  if (const auto index = positiveSuffix(id, "RIVAL_RANK_")) {
    return *index == 1 ? "RIVAL TOP"
                       : "RIVAL RANK " + std::to_string(*index);
  }
  if (positiveSuffix(id, "RIVAL_")) {
    return "NO RIVAL";
  }
  if (const auto index = positiveSuffix(id, "IR_NEXT_")) {
    return "IR NEXT " + std::to_string(*index) + "RANK";
  }
  if (const auto index = positiveSuffix(id, "IR_RANKRATE_")) {
    if (*index < 100) {
      return "IR RANK TOP " + std::to_string(*index) + "%";
    }
  }
  if (const auto index = positiveSuffix(id, "IR_RANK_")) {
    return "IR RANK " + std::to_string(*index);
  }
  return "MAX";
}

inline std::vector<std::string> beatorajaTargetNeighbourNames(
    std::string_view selectedTarget, const std::vector<std::string> &targets) {
  std::vector<std::string> names(20);
  const auto found = std::ranges::find(targets, selectedTarget);
  if (found == targets.end() || targets.empty()) {
    return names;
  }
  const std::size_t selected = static_cast<std::size_t>(found - targets.begin());
  const std::size_t count = targets.size();
  for (std::size_t index = 0; index < 10; ++index) {
    const std::size_t previous =
        (selected + count - (10 - index) % count) % count;
    names[index] = beatorajaTargetPropertyName(targets[previous]);
    const std::size_t next = (selected + index + 1) % count;
    names[index + 10] = beatorajaTargetPropertyName(targets[next]);
  }
  return names;
}

} // namespace skin
