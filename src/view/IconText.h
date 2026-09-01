#pragma once

#include <cstdint>
#include <string>

namespace ui_icons {

inline constexpr const char *kFontAwesomeSolidPath =
    "assets/fonts/fa-solid-900.ttf";
inline constexpr uint32_t kXmark = 0xf00d;
inline constexpr uint32_t kSquare = 0xf0c8;
inline constexpr uint32_t kSquareCheck = 0xf14a;
inline constexpr uint32_t kDrag = 0xf58e;
inline constexpr uint32_t kMusic = 0xf001;
inline constexpr uint32_t kTasks = 0xf0ae;
inline constexpr uint32_t kIrUploads = 0xf0ee;
inline constexpr uint32_t kSettings = 0xf013;
inline constexpr uint32_t kCollapse = 0xf077;
inline constexpr uint32_t kExpand = 0xf078;
inline constexpr uint32_t kHide = 0xf070;

inline std::string textForCodepoint(uint32_t codepoint) {
  std::string result;
  if (codepoint <= 0x7f) {
    result.push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7ff) {
    result.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
    result.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  } else if (codepoint <= 0xffff) {
    result.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
    result.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
    result.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  } else if (codepoint <= 0x10ffff) {
    result.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
    result.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
    result.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
    result.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  }
  return result;
}

} // namespace ui_icons
