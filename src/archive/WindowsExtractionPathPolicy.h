#pragma once

#include <string_view>

namespace archive_file {

inline bool isSafeWindowsExtractionComponent(std::string_view utf8Component) noexcept {
  if (utf8Component.empty() || utf8Component.front() == ' ' ||
      utf8Component.back() == ' ' || utf8Component.back() == '.') {
    return false;
  }
  for (const unsigned char byte : utf8Component) {
    if (byte < 32 || std::string_view("<>:\"/\\|?*").find(byte) != std::string_view::npos) {
      return false;
    }
  }

  auto basename = utf8Component.substr(0, utf8Component.find('.'));
  while (!basename.empty() && basename.back() == ' ') {
    basename.remove_suffix(1);
  }
  const auto equalsAsciiInsensitive = [](std::string_view actual, std::string_view uppercase) {
    if (actual.size() != uppercase.size()) {
      return false;
    }
    for (std::size_t index = 0; index < actual.size(); ++index) {
      char character = actual[index];
      if (character >= 'a' && character <= 'z') {
        character -= 'a' - 'A';
      }
      if (character != uppercase[index]) {
        return false;
      }
    }
    return true;
  };
  constexpr std::string_view reserved[] = {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"};
  for (const auto device : reserved) {
    if (equalsAsciiInsensitive(basename, device)) {
      return false;
    }
  }
  if (basename.size() >= 4 &&
      (equalsAsciiInsensitive(basename.substr(0, 3), "COM") ||
       equalsAsciiInsensitive(basename.substr(0, 3), "LPT"))) {
    const auto digit = basename.substr(3);
    if ((digit.size() == 1 && digit.front() >= '1' && digit.front() <= '9') ||
        digit == "\xC2\xB9" || digit == "\xC2\xB2" || digit == "\xC2\xB3") {
      return false;
    }
  }
  return true;
}

inline bool isSafeWindowsExtractionPath(std::string_view utf8RelativePath) noexcept {
  while (true) {
    const auto separator = utf8RelativePath.find_first_of("/\\");
    if (!isSafeWindowsExtractionComponent(utf8RelativePath.substr(0, separator))) {
      return false;
    }
    if (separator == std::string_view::npos) {
      return true;
    }
    utf8RelativePath.remove_prefix(separator + 1);
  }
}

}
