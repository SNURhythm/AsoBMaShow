#include "MusicSelectSearchHistory.h"

#include <algorithm>
#include <ranges>
#include <utf8proc.h>

namespace {
bool javaWhitespace(utf8proc_int32_t codepoint) {
  switch (codepoint) {
  case 0x0009:
  case 0x000a:
  case 0x000b:
  case 0x000c:
  case 0x000d:
  case 0x001c:
  case 0x001d:
  case 0x001e:
  case 0x001f: return true;
  case 0x00a0:
  case 0x2007:
  case 0x202f: return false;
  default: break;
  }
  const auto category = utf8proc_category(codepoint);
  return category == UTF8PROC_CATEGORY_ZS ||
         category == UTF8PROC_CATEGORY_ZL ||
         category == UTF8PROC_CATEGORY_ZP;
}

bool javaBlank(std::string_view text) {
  std::size_t offset = 0;
  while (offset < text.size()) {
    utf8proc_int32_t codepoint = 0;
    const auto consumed = utf8proc_iterate(
        reinterpret_cast<const utf8proc_uint8_t *>(text.data() + offset),
        static_cast<utf8proc_ssize_t>(text.size() - offset), &codepoint);
    if (consumed <= 0 || !javaWhitespace(codepoint)) return false;
    offset += static_cast<std::size_t>(consumed);
  }
  return true;
}
} // namespace

bool MusicSelectSearchHistory::remember(std::string text, bool hasResults,
                                        std::size_t maximum) {
  if (!hasResults || !acceptsText(text)) return false;
  const auto duplicate = std::ranges::find(entries_, text);
  if (duplicate != entries_.end()) entries_.erase(duplicate);
  if (entries_.size() >= maximum) entries_.erase(entries_.begin());
  entries_.push_back(std::move(text));
  return true;
}

bool MusicSelectSearchHistory::acceptsText(std::string_view text) {
  return !javaBlank(text);
}
