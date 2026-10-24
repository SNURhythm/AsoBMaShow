#include "NativeDocument.h"
#include "../targets.h"
#include "../ui/PresentationSchedule.h"
#include <algorithm>

namespace native_document {
void State::requestDismiss() {
  dismissed.store(true);
  ui::notifyCompletion();
}
std::unique_ptr<Surface> create(const std::string &text, Frame frame, Appearance appearance) {
#if defined(ASOBMASHOW_NATIVE_DOCUMENT_TEST_STUB)
  return nullptr;
#elif defined(__APPLE__)
  return detail::createApple(text, frame, appearance);
#elif TARGET_OS_ANDROID
  return detail::createAndroid(text, frame, appearance);
#elif TARGET_OS_WINDOWS
  return detail::createWindows(text, frame, appearance);
#else
  return nullptr;
#endif
}
std::vector<text_range::Range> utf16Ranges(std::string_view text,
    const std::vector<text_range::Range> &ranges) {
  std::vector<text_range::Range> ordered;
  const auto boundary = [&](std::size_t offset) {
    return offset <= text.size() && (offset == text.size() ||
        (static_cast<unsigned char>(text[offset]) & 0xc0) != 0x80);
  };
  for (const auto &range : ranges) {
    if (range.begin < range.end && boundary(range.begin) && boundary(range.end))
      ordered.push_back(range);
  }
  std::sort(ordered.begin(), ordered.end(), [](const auto &a, const auto &b) {
    return a.begin < b.begin;
  });
  std::vector<text_range::Range> merged;
  for (const auto &range : ordered) {
    if (!merged.empty() && range.begin <= merged.back().end)
      merged.back().end = std::max(merged.back().end, range.end);
    else merged.push_back(range);
  }
  std::size_t byte = 0, units = 0;
  const auto advance = [&](std::size_t target) {
    while (byte < target) {
      const auto lead = static_cast<unsigned char>(text[byte]);
      const std::size_t width = lead < 0x80 ? 1 : lead >= 0xc2 && lead <= 0xdf ? 2 :
          lead >= 0xe0 && lead <= 0xef ? 3 : lead >= 0xf0 && lead <= 0xf4 ? 4 : 0;
      if (width == 0 || width > target - byte) return false;
      unsigned int codepoint = lead & (width == 1 ? 0x7f : width == 2 ? 0x1f : width == 3 ? 0x0f : 0x07);
      for (std::size_t i = 1; i < width; ++i) {
        const auto next = static_cast<unsigned char>(text[byte + i]);
        if ((next & 0xc0) != 0x80) return false;
        codepoint = (codepoint << 6) | (next & 0x3f);
      }
      if ((width == 2 && codepoint < 0x80) || (width == 3 && codepoint < 0x800) ||
          (width == 4 && codepoint < 0x10000) || codepoint > 0x10ffff ||
          (codepoint >= 0xd800 && codepoint <= 0xdfff)) return false;
      byte += width;
      units += width == 4 ? 2 : 1;
    }
    return true;
  };
  std::vector<text_range::Range> converted;
  converted.reserve(merged.size());
  for (const auto &range : merged) {
    if (!advance(range.begin)) return {};
    const auto begin = units;
    if (!advance(range.end)) return {};
    converted.push_back({begin, units});
  }
  return converted;
}
}
