#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

namespace long_note_mode {
inline constexpr int kUnknownValue = 0;
inline constexpr int kLnValue = 1;
inline constexpr int kCnValue = 2;
inline constexpr int kHcnValue = 3;
inline constexpr const char *kLnId = "LN";
inline constexpr const char *kCnId = "CN";
inline constexpr const char *kHcnId = "HCN";
inline constexpr std::array<int, 3> kPlayableValues{kLnValue, kCnValue,
                                                    kHcnValue};
inline constexpr std::array<const char *, 3> kPlayableIds{kLnId, kCnId,
                                                          kHcnId};

inline int normalizeValue(int lnMode) {
  return lnMode >= kLnValue && lnMode <= kHcnValue ? lnMode : kUnknownValue;
}

inline int normalizeSelectedValue(int lnMode,
                                  int fallback = kLnValue) {
  const int normalized = normalizeValue(lnMode);
  if (normalized > kUnknownValue) {
    return normalized;
  }
  const int normalizedFallback = normalizeValue(fallback);
  return normalizedFallback > kUnknownValue ? normalizedFallback : kLnValue;
}

inline std::string normalizeId(std::string value) {
  auto isSpace = [](unsigned char ch) { return std::isspace(ch) != 0; };
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(),
                           [&](unsigned char ch) { return !isSpace(ch); }));
  value.erase(std::find_if(value.rbegin(), value.rend(),
                           [&](unsigned char ch) { return !isSpace(ch); })
                  .base(),
              value.end());
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) {
                   if (ch == '_' || ch == ' ') {
                     return '-';
                   }
                   return static_cast<char>(std::toupper(ch));
                 });
  if (value == "1" || value == kLnId || value == "LONGNOTE" ||
      value == "LONG-NOTE") {
    return kLnId;
  }
  if (value == "2" || value == kCnId || value == "CHARGENOTE" ||
      value == "CHARGE-NOTE") {
    return kCnId;
  }
  if (value == "3" || value == kHcnId || value == "HELLCHARGENOTE" ||
      value == "HELL-CHARGE-NOTE" || value == "HELL-CHARGE") {
    return kHcnId;
  }
  return value;
}

inline bool isValidId(const std::string &value) {
  return value == kLnId || value == kCnId || value == kHcnId;
}

inline std::string parseId(const std::string &value,
                           const std::string &fallback = kLnId) {
  const std::string normalized = normalizeId(value);
  return isValidId(normalized) ? normalized : fallback;
}

inline int valueFromId(const std::string &value,
                       int fallback = kLnValue) {
  const std::string normalized = normalizeId(value);
  if (normalized == kCnId) {
    return kCnValue;
  }
  if (normalized == kHcnId) {
    return kHcnValue;
  }
  if (normalized == kLnId) {
    return kLnValue;
  }
  return normalizeSelectedValue(fallback);
}

inline std::string idFromValue(int lnMode,
                               const std::string &fallback = kLnId) {
  switch (normalizeValue(lnMode)) {
  case kCnValue:
    return kCnId;
  case kHcnValue:
    return kHcnId;
  case kLnValue:
    return kLnId;
  default:
    return fallback;
  }
}

inline std::string sqlValidValuePredicate(const std::string &expression) {
  return expression + " BETWEEN " + std::to_string(kLnValue) + " AND " +
         std::to_string(kHcnValue);
}
} // namespace long_note_mode
