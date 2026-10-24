#pragma once
#include "../PlatformOpen.h"
#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace document_links {
struct Link { std::size_t begin, end; std::string url; };
inline bool startsWith(std::string_view text, std::string_view prefix) {
  if (text.size() < prefix.size()) return false;
  for (std::size_t i = 0; i < prefix.size(); ++i) {
    const char c = text[i];
    if ((c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c) != prefix[i]) return false;
  }
  return true;
}
inline bool delimiter(std::string_view text) {
  const auto c = static_cast<unsigned char>(text.front());
  if (c <= 0x20 || c == 0x7f || std::string_view("<>\"'\\").find(c) != std::string_view::npos) return true;
  for (const auto end : {"。", "、", "「", "」", "『", "』", "（", "）", "，", "！", "？"})
    if (text.starts_with(end)) return true;
  return false;
}
inline std::vector<Link> find(std::string_view text) {
  std::vector<Link> result;
  for (std::size_t begin = 0; begin < text.size();) {
    const auto tail = text.substr(begin);
    const bool bare = startsWith(tail, "www.");
    if (!bare && !startsWith(tail, "https://") && !startsWith(tail, "http://")) { ++begin; continue; }
    // Do not interpret a scheme embedded in an identifier or another protocol.
    if (begin && (std::isalnum(static_cast<unsigned char>(text[begin - 1])) ||
                  std::string_view("_/@").find(text[begin - 1]) != std::string_view::npos)) { ++begin; continue; }
    auto end = begin;
    while (end < text.size() && !delimiter(text.substr(end))) ++end;
    const auto scannedEnd = end;
    while (end > begin) {
      const char c = text[end - 1];
      if (std::string_view(".,;:!?").find(c) != std::string_view::npos) { --end; continue; }
      const auto closing = std::string_view(")]}").find(c);
      if (closing != std::string_view::npos) {
        const auto part = text.substr(begin, end - begin);
        if (std::count(part.begin(), part.end(), c) >
            std::count(part.begin(), part.end(), std::string_view("([{")[closing])) { --end; continue; }
      }
      break;
    }
    const std::string url = (bare ? "https://" : "") + std::string(text.substr(begin, end - begin));
    if (platform_open::isWebUrl(url)) result.push_back({begin, end, url});
    begin = std::max(begin + 1, scannedEnd);
  }
  return result;
}
}
