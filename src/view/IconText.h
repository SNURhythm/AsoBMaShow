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
inline constexpr uint32_t kChartLine = 0xf201;
inline constexpr uint32_t kRecords = 0xf03a;
inline constexpr uint32_t kReveal = 0xf07c;
inline constexpr uint32_t kDownload = 0xf019;
inline constexpr uint32_t kMusic = 0xf001;
inline constexpr uint32_t kTasks = 0xf0ae;
inline constexpr uint32_t kPlayOptions = 0xf1de;
inline constexpr uint32_t kIrUploads = 0xf0ee;
inline constexpr uint32_t kPalette = 0xf53f;
inline constexpr uint32_t kSettings = 0xf013;
inline constexpr uint32_t kCollapse = 0xf053;
inline constexpr uint32_t kExpand = 0xf054;
inline constexpr uint32_t kHide = 0xf070;
inline constexpr uint32_t kUser = 0xf007;
inline constexpr uint32_t kKeyboard = 0xf11c;
inline constexpr uint32_t kClock = 0xf017;
inline constexpr uint32_t kRoad = 0xf018;
inline constexpr uint32_t kDisplay = 0xf108;
inline constexpr uint32_t kImage = 0xf03e;
inline constexpr uint32_t kVolume = 0xf028;
inline constexpr uint32_t kGlobe = 0xf0ac;
inline constexpr uint32_t kRuleset = 0xf0e3;
inline constexpr uint32_t kHeart = 0xf004;
inline constexpr uint32_t kRotate = 0xf2f1;
inline constexpr uint32_t kArrowDown = 0xf063;
inline constexpr uint32_t kShuffle = 0xf074;
inline constexpr uint32_t kGripLines = 0xf7a4;
inline constexpr uint32_t kLifeRing = 0xf1cd;
inline constexpr uint32_t kBars = 0xf0c9;
inline constexpr uint32_t kGauge = 0xf3fd;
inline constexpr uint32_t kPlay = 0xf04b;
inline constexpr uint32_t kFlagCheckered = 0xf11e;

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
