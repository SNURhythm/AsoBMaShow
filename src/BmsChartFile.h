#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <string_view>

namespace asobmshow::bms_chart_file {

inline constexpr std::array<std::string_view, 3> kBmsChartExtensions = {
    ".bms", ".bme", ".bml"};

namespace detail {

inline char asciiLower(char value) {
  return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A'))
                                      : value;
}

inline wchar_t asciiLower(wchar_t value) {
  return value >= L'A' && value <= L'Z'
             ? static_cast<wchar_t>(value + (L'a' - L'A'))
             : value;
}

inline bool equalsAsciiCaseInsensitive(std::string_view value,
                                       std::string_view expected) {
  if (value.size() != expected.size()) {
    return false;
  }
  for (std::size_t index = 0; index < expected.size(); ++index) {
    if (asciiLower(value[index]) != expected[index]) {
      return false;
    }
  }
  return true;
}

inline bool equalsAsciiCaseInsensitive(std::wstring_view value,
                                       std::string_view expected) {
  if (value.size() != expected.size()) {
    return false;
  }
  for (std::size_t index = 0; index < expected.size(); ++index) {
    if (asciiLower(value[index]) != static_cast<wchar_t>(expected[index])) {
      return false;
    }
  }
  return true;
}

template <typename StringView>
inline bool endsWithAsciiCaseInsensitive(StringView value,
                                         std::string_view expected) {
  return value.size() >= expected.size() &&
         equalsAsciiCaseInsensitive(value.substr(value.size() - expected.size()),
                                    expected);
}

} // namespace detail

inline bool isBmsChartExtension(std::string_view extension) {
  for (std::string_view expected : kBmsChartExtensions) {
    if (detail::equalsAsciiCaseInsensitive(extension, expected)) {
      return true;
    }
  }
  return false;
}

inline bool isBmsChartExtension(std::wstring_view extension) {
  for (std::string_view expected : kBmsChartExtensions) {
    if (detail::equalsAsciiCaseInsensitive(extension, expected)) {
      return true;
    }
  }
  return false;
}

inline bool isBmsChartFileName(std::string_view fileName) {
  for (std::string_view expected : kBmsChartExtensions) {
    if (detail::endsWithAsciiCaseInsensitive(fileName, expected)) {
      return true;
    }
  }
  return false;
}

inline bool isBmsChartFileName(std::wstring_view fileName) {
  for (std::string_view expected : kBmsChartExtensions) {
    if (detail::endsWithAsciiCaseInsensitive(fileName, expected)) {
      return true;
    }
  }
  return false;
}

inline bool isBmsChartPath(const std::filesystem::path &path) {
#ifdef _WIN32
  return isBmsChartExtension(path.extension().wstring());
#else
  return isBmsChartExtension(path.extension().string());
#endif
}

} // namespace asobmshow::bms_chart_file
