#pragma once

#include <algorithm>
#include <cctype>
#include <string>

namespace assist_options {
inline constexpr const char *kOff = "OFF";
inline constexpr const char *kDrag = "DRAG";

inline std::string normalize(std::string option) {
  option.erase(option.begin(), std::find_if_not(option.begin(), option.end(),
                                                [](unsigned char ch) {
                                                  return std::isspace(ch) != 0;
                                                }));
  option.erase(
      std::find_if_not(option.rbegin(), option.rend(),
                       [](unsigned char ch) { return std::isspace(ch) != 0; })
          .base(),
      option.end());
  std::transform(option.begin(), option.end(), option.begin(),
                 [](unsigned char ch) {
                   if (ch == '_' || ch == ' ') {
                     return '-';
                   }
                   return static_cast<char>(std::toupper(ch));
                 });
  if (option == "DRAG" || option == "DRAG-MODE") {
    return kDrag;
  }
  return kOff;
}

inline bool isEnabled(const std::string &option) {
  return normalize(option) != kOff;
}

inline bool isDragMode(const std::string &option) {
  return normalize(option) == kDrag;
}
} // namespace assist_options
