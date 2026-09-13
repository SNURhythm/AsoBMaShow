#pragma once

#include <cstddef>
#include <string>
#include <string_view>

[[nodiscard]] inline std::string sanitizeExportFileNamePart(
    const std::string &value, std::string_view fallback, std::size_t maxLength) {
  std::string result;
  result.reserve(value.size());
  for (const unsigned char ch : value) {
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') || ch == '-' || ch == '_') {
      result.push_back(static_cast<char>(ch));
    } else if (ch == ' ' || ch == '.' || ch == '[' || ch == ']') {
      result.push_back('_');
    }
  }

  while (!result.empty() && result.back() == '_') {
    result.pop_back();
  }
  if (result.empty()) {
    return std::string(fallback);
  }
  return result.substr(0, maxLength);
}
